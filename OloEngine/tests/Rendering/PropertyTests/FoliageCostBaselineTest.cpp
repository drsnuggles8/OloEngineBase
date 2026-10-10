// =============================================================================
// FoliageCostBaselineTest.cpp — what the flora costs, on named hardware
// (issue #1391, the last delivery-contract item of epic #1224).
//
// Two halves.
//
// THE CONTRACT (runs in the normal suite, SKIPs without a GL 4.6 context).
// The baseline is only as good as its instruments, so each one is pinned:
//   * every GPU stage foliage runs has its own timer bracket: the main-view
//     cull (dispatched at scene submission, outside every render-graph pass,
//     so before #1391 it was in the frame's total and in no pass at all), the
//     shadow views' culls, the shadow casters and the forward draws;
//   * the memory breakdown's categories add up to the streaming floor they
//     were split out of, the report's capacity rows carry them, and the cull
//     streams are booked to the foliage owner rather than to nobody;
//   * OLO_FOLIAGE_NO_DENSITY_LOD reaches the cull as well as the draw, so the
//     density-LOD A/B measures the thing it names;
//   * reading the cull back (the census each cell records) leaves the frames
//     after it as cheap as they were: it used to slow the main view's cull
//     ~20x and its draw ~6x for the rest of the process.
//
// THE MEASUREMENT (OLO_FOLIAGE_COST=1). Walks the committed
// Scenes/FoliageMeadowToWoodland.olo along the meadow-to-woodland path
// FloraTraversalEvidenceTest derives, and the Benchmark/Meadow.olo and
// Benchmark/Woodland.olo fixtures from their manifests' still cameras, at
// 1920x1080 on {Forward, Forward+, Deferred} x {CSM, VSM}. At every pose four
// arms are INTERLEAVED in rotated order (docs/agent-rules gpu-timing: an
// A-then-B run charges all of the box's clock drift to whichever arm ran
// second):
//     Shipped       the scene as authored
//     CpuCull       FoliageRenderer::SetGPUCullingEnabled(false)
//     NoDensityLod  OLO_FOLIAGE_NO_DENSITY_LOD
//     NoFoliage     the FoliageComponent disabled: the control that turns a
//                   pass total into "the foliage share of this pass" where
//                   foliage has no bracket of its own (Deferred draws it into
//                   the G-Buffer among everything else)
// It then takes 600-frame tail windows (FrameTimeTail: p99 means nothing in
// fewer) with and without an in-place whole-layer rebuild every 100 frames,
// and the conditional cells: a second and third resolution and Deferred MSAA
// 4x. Everything is written as raw samples plus per-cell summaries to
// $OLO_FOLIAGE_COST_OUT/foliage-cost.json; scripts/perf/foliage-cost-baseline.py
// runs it in fresh processes and records the host. The numbers are a
// BASELINE: nothing here asserts a budget, and nothing may be thinned to meet
// one (#1391's scope boundary).
//
// The scene clock is pinned (Time::SetMockTime) so wind is identical in every
// arm: the bend still costs what it costs, but every arm draws the same bend.
//
// Vulkan is not reachable from a headless fixture; those cells are a live
// editor session (olo_perf_pass_timings), recorded in the analysis doc.
//
// OLO_TEST_LAYER: L6
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/FrameTimeTail.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glad/gl.h>
#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The issue's named size. The conditional cells add two more.
        constexpr u32 kWidth = 1920;
        constexpr u32 kHeight = 1080;
        constexpr f32 kMockTime = 4.0f;
        // 60 Hz, the deadline the miss counts are taken against.
        constexpr f32 kDeadlineMs = 1000.0f / 60.0f;

        const fs::path kTraversalScene = fs::path("Scenes") / "FoliageMeadowToWoodland.olo";
        constexpr const char* kMeadowLayer = "Meadow Grass";
        constexpr const char* kWoodlandLayer = "Woodland Pines";
        // The path FloraTraversalEvidenceTest walks: nine poses from the
        // meadow's densest bucket to the woodland's, eye 2.6 m over the ground.
        constexpr u32 kTraversalSteps = 9;

        [[nodiscard]] fs::path EditorRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT };
        }

        [[nodiscard]] bool EnvFlag(const char* name)
        {
            const char* v = std::getenv(name);
            return v != nullptr && v[0] == '1';
        }

        [[nodiscard]] u32 EnvCount(const char* name, u32 fallback)
        {
            const char* v = std::getenv(name);
            if (v == nullptr || v[0] == '\0')
                return fallback;
            const long parsed = std::strtol(v, nullptr, 10);
            return parsed > 0 ? static_cast<u32>(parsed) : fallback;
        }

        // True when `name` is unset or empty, or lists `value` (comma-separated).
        [[nodiscard]] bool EnvSelects(const char* name, std::string_view value)
        {
            const char* v = std::getenv(name);
            if (v == nullptr || v[0] == '\0')
                return true;
            std::string_view list(v);
            while (!list.empty())
            {
                const sizet comma = list.find(',');
                if (list.substr(0, comma) == value)
                    return true;
                if (comma == std::string_view::npos)
                    break;
                list.remove_prefix(comma + 1);
            }
            return false;
        }

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        }

        struct Pose
        {
            std::string Name;
            glm::vec3 Eye{ 0.0f };
            f32 Yaw = 0.0f;   // radians; EditorCamera yaw 0 looks along -Z
            f32 Pitch = 0.0f; // radians; positive looks DOWN
            f32 FovDegrees = 60.0f;
            f32 Near = 0.1f;
            f32 Far = 2000.0f;
        };

        [[nodiscard]] EditorCamera MakeCamera(const Pose& pose, u32 width, u32 height)
        {
            EditorCamera camera(pose.FovDegrees, static_cast<f32>(width) / static_cast<f32>(height), pose.Near, pose.Far);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);
            return camera;
        }

        enum class Arm : u8
        {
            Shipped,
            CpuCull,
            NoDensityLod,
            NoFoliage
        };
        constexpr std::array kAllArms{ Arm::Shipped, Arm::CpuCull, Arm::NoDensityLod, Arm::NoFoliage };

        [[nodiscard]] const char* ArmName(Arm arm)
        {
            switch (arm)
            {
                case Arm::Shipped:
                    return "Shipped";
                case Arm::CpuCull:
                    return "CpuCull";
                case Arm::NoDensityLod:
                    return "NoDensityLod";
                case Arm::NoFoliage:
                    return "NoFoliage";
            }
            return "Unknown";
        }

        // A bracket the GPU timer pool published for one frame, summed by name:
        // a sub-pass is stamped once per shadow view, and the per-view split is
        // not what a baseline wants.
        using PassTotals = std::map<std::string, f64>;

        // The pool's own marker for brackets it ran out of room for.
        [[nodiscard]] bool IsOverflowEntry(const GPUPassTiming& pass)
        {
            return pass.Name.ToView().starts_with("<");
        }

        // The brackets whose raw per-frame samples are kept (every bracket's
        // median is kept regardless): foliage's own, and the passes a foliage
        // share is taken out of.
        [[nodiscard]] bool KeepRawSamples(std::string_view name)
        {
            return name.find("Foliage") != std::string_view::npos || name.starts_with("ScenePass") ||
                   name.find("Shadow") != std::string_view::npos;
        }

        // One measured window: the wall time of each tick and the GPU timings
        // of EXACTLY those frames, matched by frame number. Resolution lags 1-3
        // frames, so the window is drained under the same configuration rather
        // than letting the next arm's frames resolve into it.
        struct Window
        {
            std::vector<f64> WallMs;
            std::vector<u64> Frames;        // frame numbers with a valid GPU sample
            std::vector<f64> GpuMs;         // aligned with Frames
            std::vector<PassTotals> Passes; // aligned with Frames
            u32 MissingGpu = 0;             // frames whose GPU sample never resolved or was invalid
            u32 OverflowFrames = 0;         // frames the pool could not bracket in full
            // How many brackets of each name the LAST resolved frame stamped: a
            // sub-pass is stamped once per shadow view, and the per-name totals
            // above cannot say how many views that was.
            std::map<std::string, u32> LastFrameBracketCounts;
        };

        [[nodiscard]] f64 Percentile(std::vector<f64> values, f64 q)
        {
            std::erase_if(values, [](f64 v)
                          { return !std::isfinite(v); });
            if (values.empty())
                return std::nan("");
            std::ranges::sort(values);
            // Nearest rank, as FrameTimeTail and perf_trend.py compute it.
            const f64 rank = std::ceil(q * static_cast<f64>(values.size()));
            const sizet index = static_cast<sizet>(std::clamp(rank, 1.0, static_cast<f64>(values.size()))) - 1u;
            return values[index];
        }

        [[nodiscard]] nlohmann::json TailJson(const std::vector<f64>& samples)
        {
            FrameTimeTail tail(static_cast<u32>(std::max<sizet>(samples.size(), 1u)));
            for (const f64 s : samples)
                tail.Push(static_cast<f32>(s));
            const FrameTimeTailStats stats = tail.Query(kDeadlineMs);
            return { { "count", stats.SampleCount }, { "min", stats.MinMs }, { "mean", stats.MeanMs }, { "p50", stats.P50Ms }, { "p95", stats.P95Ms }, { "p99", stats.P99Ms }, { "max", stats.MaxMs }, { "deadlineMs", stats.BudgetMs }, { "deadlineMisses", stats.OverBudgetFrames } };
        }

        [[nodiscard]] f64 Round4(f64 v)
        {
            return std::isfinite(v) ? std::round(v * 10000.0) / 10000.0 : v;
        }

        [[nodiscard]] nlohmann::json RoundedArray(const std::vector<f64>& values)
        {
            nlohmann::json out = nlohmann::json::array();
            for (const f64 v : values)
                out.push_back(Round4(v));
            return out;
        }

        [[nodiscard]] u64 TextureBytes(const Ref<Texture2D>& texture)
        {
            if (!texture)
                return 0u;
            const auto& spec = texture->GetSpecification();
            return RendererMemoryFormat::ImageBytes(spec.Format, spec.Width, spec.Height, texture->GetMipLevelCount(), 1u,
                                                    spec.Samples)
                .value_or(0u);
        }

        [[nodiscard]] u64 OwnerGpuLiveBytes(const RendererMemoryReport& report, std::string_view owner)
        {
            u64 bytes = 0;
            for (const auto& row : report.Owners)
            {
                if (row.Owner.ToView() == owner)
                    bytes += row.GpuLiveBytes;
            }
            return bytes;
        }
    } // namespace

    class FoliageCostBaselineTest : public RendererAttachedTest
    {
      protected:
        struct Loaded
        {
            Ref<Scene> SceneRef;
            Entity Foliage;
            Entity Terrain;
        };

        void BuildScene() override
        {
            if (!RenderPropertyFixture::IsGpuAvailable())
                return;
            // The fixture's own scene stays empty; each subject is its own
            // loaded scene driven through RunEditorFramesOn.
            EnableRendering(kWidth, kHeight);
        }

        void SetUp() override
        {
            m_PreviousProject = Project::GetActive();
            m_PreviousAssetManager = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            RendererAttachedTest::SetUp();
            // AFTER the base SetUp: the first GPU fixture in a process moves
            // into OloEditor/ there, and restoring the directory from before
            // it would leave every later GPU suite unable to open its shaders.
            std::error_code ec;
            m_PreviousCwd = fs::current_path(ec);
        }

        void TearDown() override
        {
            FoliageRenderer::SetGPUCullingEnabled(true);
            Levers::SetFoliageNoDensityLod(false);
            Time::ClearMockTime();
            m_Loaded.reset();
            RendererAttachedTest::TearDown();
            std::error_code ec;
            if (!m_PreviousCwd.empty())
                fs::current_path(m_PreviousCwd, ec);
            if (m_PreviousProject)
                Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
            else
                Project::Unload();
            if (m_PreviousAssetManager)
                Project::SetAssetManager(m_PreviousAssetManager);
        }

        // The editor's content context: the real project, cwd OloEditor/.
        void EnterEditorProject()
        {
            std::error_code ec;
            fs::current_path(EditorRoot(), ec);
            ASSERT_FALSE(ec) << "cannot enter the editor root: " << ec.message();
            Project::Unload();
            ASSERT_TRUE(Project::Load(EditorRoot() / "SandboxProject" / "Sandbox.oloproj"));
            if (m_PreviousAssetManager)
                Project::SetAssetManager(m_PreviousAssetManager);
        }

        // Deserialize a committed scene from a temp copy (its .scenebin sidecar
        // must not land in the working tree) and tick until the flora exists.
        void Load(const fs::path& sceneAssetPath)
        {
            m_Loaded.reset();
            ASSERT_NO_FATAL_FAILURE(EnterEditorProject());
            Time::SetMockTime(kMockTime);

            const fs::path source = EditorRoot() / "SandboxProject" / "Assets" / sceneAssetPath;
            const fs::path copy = TempDir(sceneAssetPath.stem().string()) / sceneAssetPath.filename();
            std::error_code ec;
            fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << "cannot copy " << source.string() << ": " << ec.message();

            Loaded loaded;
            loaded.SceneRef = Scene::Create();
            SceneSerializer serializer(loaded.SceneRef);
            ASSERT_TRUE(serializer.Deserialize(copy)) << copy.string();
            for (auto entity : loaded.SceneRef->GetAllEntitiesWith<FoliageComponent>())
                loaded.Foliage = Entity{ entity, loaded.SceneRef.get() };
            ASSERT_TRUE(loaded.Foliage) << sceneAssetPath.string() << " has no FoliageComponent";
            ASSERT_TRUE(loaded.Foliage.HasComponent<TerrainComponent>());
            loaded.Terrain = loaded.Foliage;

            loaded.SceneRef->SetIs3DModeEnabled(true);
            loaded.SceneRef->OnViewportResize(m_Width, m_Height);
            loaded.SceneRef->SetRenderingEnabled(true);
            m_Loaded = std::move(loaded);

            // Anywhere will do: generation covers the whole terrain.
            Tick(Pose{ "load", glm::vec3(128.0f, 60.0f, 300.0f), 0.0f, 0.3f }, 3);
            ASSERT_TRUE(Plants().GetTotalInstanceCount() > 0u) << sceneAssetPath.string() << " generated no flora";
        }

        [[nodiscard]] FoliageComponent& FoliageOf()
        {
            return m_Loaded->Foliage.GetComponent<FoliageComponent>();
        }
        [[nodiscard]] const FoliageRenderer& Plants()
        {
            return *FoliageOf().m_Renderer;
        }

        void Resize(u32 width, u32 height)
        {
            m_Width = width;
            m_Height = height;
            ResizeRenderTarget(width, height);
            if (m_Loaded)
                m_Loaded->SceneRef->OnViewportResize(width, height);
        }

        void Tick(const Pose& pose, u32 frames)
        {
            RunEditorFramesOn(*m_Loaded->SceneRef, MakeCamera(pose, m_Width, m_Height), frames);
        }

        static void SetPath(RenderingPath path, u32 msaa = 1u)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path;
            settings.Deferred.MSAASampleCount = msaa;
            Renderer3D::ApplyRendererSettings();
        }

        // True when the technique asked for is the one that will run. The
        // fixture restores the shadow settings in TearDown.
        [[nodiscard]] static bool SetVsm(bool enabled)
        {
            auto& shadowMap = Renderer3D::GetShadowMap();
            ShadowSettings settings = shadowMap.GetSettings();
            settings.VSM.Enabled = enabled;
            shadowMap.SetSettings(settings);
            return shadowMap.IsVirtualShadowMapActive() == enabled;
        }

        void ApplyArm(Arm arm)
        {
            FoliageRenderer::SetGPUCullingEnabled(arm != Arm::CpuCull);
            Levers::SetFoliageNoDensityLod(arm == Arm::NoDensityLod);
            FoliageOf().m_Enabled = arm != Arm::NoFoliage;
        }

        // Render `count` frames, timing each tick and collecting the GPU
        // timings of exactly those frames. `beforeFrame(i)` runs ahead of tick i.
        [[nodiscard]] Window Measure(const Pose& pose, u32 count, const std::function<void(u32)>& beforeFrame = {})
        {
            auto& pool = GPUPassTimerPool::GetInstance();
            const EditorCamera camera = MakeCamera(pose, m_Width, m_Height);
            const u64 first = pool.GetCurrentFrameNumber() + 1u;
            const u64 last = first + count - 1u;

            std::map<u64, std::pair<f64, PassTotals>> resolved;
            std::map<std::string, u32> lastCounts;
            u32 overflow = 0;
            const auto harvest = [&]()
            {
                const auto timings = pool.GetLastFrameTimings();
                if (timings.FrameNumber < first || timings.FrameNumber > last || resolved.contains(timings.FrameNumber))
                    return;
                if (!timings.Frame.IsValid())
                    return;
                PassTotals passes;
                std::map<std::string, u32> counts;
                bool overflowed = false;
                for (const auto& pass : timings.Passes)
                {
                    if (IsOverflowEntry(pass))
                        overflowed = true;
                    else if (pass.IsValid())
                    {
                        passes[pass.Name.ToStdString()] += pass.Sample.GpuMs;
                        ++counts[pass.Name.ToStdString()];
                    }
                    else
                    {
                        // Stamped but not a measurement (refused, dropped,
                        // out of order): NaN poisons this frame's total for
                        // the bracket, so it is neither a 0 ms sample nor
                        // mistaken for a pass that did not run.
                        passes[pass.Name.ToStdString()] += std::nan("");
                    }
                }
                overflow += overflowed ? 1u : 0u;
                lastCounts = std::move(counts);
                resolved.emplace(timings.FrameNumber, std::make_pair(timings.Frame.GpuMs, std::move(passes)));
            };

            Window window;
            window.WallMs.reserve(count);
            for (u32 i = 0; i < count; ++i)
            {
                if (beforeFrame)
                    beforeFrame(i);
                const auto t0 = std::chrono::steady_clock::now();
                RunEditorFramesOn(*m_Loaded->SceneRef, camera, 1);
                window.WallMs.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
                harvest();
            }
            // Drain: the last frames resolve 1-3 frames later, under the SAME
            // configuration.
            for (u32 i = 0; i < GPUPassTimerPool::kSlotCount + 1u && resolved.size() < count; ++i)
            {
                RunEditorFramesOn(*m_Loaded->SceneRef, camera, 1);
                harvest();
            }

            for (auto& [frame, sample] : resolved)
            {
                window.Frames.push_back(frame);
                window.GpuMs.push_back(sample.first);
                window.Passes.push_back(std::move(sample.second));
            }
            window.MissingGpu = count - static_cast<u32>(resolved.size());
            window.OverflowFrames = overflow;
            window.LastFrameBracketCounts = std::move(lastCounts);
            return window;
        }

        // The meadow-to-woodland path through the LOADED scene: the densest
        // 16 m bucket of each species, the segment between them, the eye 2.6 m
        // over the nearest plants' mean altitude.
        [[nodiscard]] std::optional<std::vector<Pose>> DeriveTraversal()
        {
            const auto& foliage = FoliageOf();
            const auto layerIndex = [&](std::string_view name) -> std::optional<u32>
            {
                for (i32 i = 0; i < foliage.m_Layers.Num(); ++i)
                {
                    if (foliage.m_Layers[i].Name.ToView() == name)
                        return static_cast<u32>(i);
                }
                return std::nullopt;
            };
            const auto meadowLayer = layerIndex(kMeadowLayer);
            const auto woodlandLayer = layerIndex(kWoodlandLayer);
            if (!meadowLayer || !woodlandLayer)
                return std::nullopt;

            const auto& terrain = m_Loaded->Terrain.GetComponent<TerrainComponent>();
            const glm::mat4 model = m_Loaded->Terrain.GetComponent<TransformComponent>().GetTransform();
            const auto& records = Plants().GetInstanceRegistry().GetRecords();
            constexpr u32 kGrid = 16u;
            const f32 cellX = terrain.m_WorldSizeX / static_cast<f32>(kGrid);
            const f32 cellZ = terrain.m_WorldSizeZ / static_cast<f32>(kGrid);

            const auto hotspot = [&](u32 layer) -> std::optional<glm::vec3>
            {
                std::vector<u32> counts(static_cast<sizet>(kGrid) * kGrid, 0u);
                std::vector<glm::dvec3> sums(counts.size(), glm::dvec3(0.0));
                for (const auto& record : records)
                {
                    if (record.m_LayerIndex != layer)
                        continue;
                    const i32 cx = static_cast<i32>(record.m_Position.x / cellX);
                    const i32 cz = static_cast<i32>(record.m_Position.z / cellZ);
                    if (cx < 0 || cz < 0 || cx >= static_cast<i32>(kGrid) || cz >= static_cast<i32>(kGrid))
                        continue;
                    const sizet bucket = static_cast<sizet>(cz) * kGrid + static_cast<sizet>(cx);
                    ++counts[bucket];
                    sums[bucket] += glm::dvec3(glm::vec3(model * glm::vec4(record.m_Position, 1.0f)));
                }
                const auto best = std::ranges::max_element(counts);
                if (*best == 0u)
                    return std::nullopt;
                const sizet bucket = static_cast<sizet>(std::distance(counts.begin(), best));
                return glm::vec3(sums[bucket] / static_cast<f64>(*best));
            };
            const auto ground = [&](const glm::vec3& at) -> std::optional<f32>
            {
                for (const f32 radius : { 10.0f, 25.0f, 50.0f })
                {
                    f64 sum = 0.0;
                    u32 n = 0;
                    for (const auto& record : records)
                    {
                        const glm::vec3 world = glm::vec3(model * glm::vec4(record.m_Position, 1.0f));
                        const f32 dx = world.x - at.x;
                        const f32 dz = world.z - at.z;
                        if (dx * dx + dz * dz <= radius * radius)
                        {
                            sum += world.y;
                            ++n;
                        }
                    }
                    if (n > 0u)
                        return static_cast<f32>(sum / static_cast<f64>(n));
                }
                return std::nullopt;
            };

            const auto meadow = hotspot(*meadowLayer);
            const auto woodland = hotspot(*woodlandLayer);
            if (!meadow || !woodland)
                return std::nullopt;
            glm::vec3 travel = *woodland - *meadow;
            travel.y = 0.0f;
            const f32 span = glm::length(travel);
            if (span < 1.0f)
                return std::nullopt;
            const glm::vec3 forward = travel / span;
            const f32 yaw = std::atan2(-forward.x, -forward.z);
            const glm::vec3 start = *meadow - forward * 0.25f * span;
            const glm::vec3 end = *woodland - forward * 0.15f * span;

            std::vector<Pose> path;
            for (u32 step = 0; step < kTraversalSteps; ++step)
            {
                const f32 t = static_cast<f32>(step) / static_cast<f32>(kTraversalSteps - 1u);
                glm::vec3 at = glm::mix(start, end, t);
                const auto height = ground(at);
                if (!height)
                    return std::nullopt;
                at.y = *height + 2.6f;
                path.push_back(Pose{ "Step" + std::to_string(step), at, yaw, 0.04f });
            }
            return path;
        }

        // A benchmark manifest's still cameras, as poses.
        [[nodiscard]] static std::optional<std::pair<fs::path, std::vector<Pose>>> ManifestPoses(const char* manifestFile)
        {
            std::string error;
            const auto manifest =
                Benchmark::LoadBenchmarkManifest(EditorRoot() / "assets" / "benchmark" / "manifests" / manifestFile, error);
            if (!manifest)
                return std::nullopt;
            std::vector<Pose> poses;
            for (const auto& camera : manifest->Cameras)
            {
                if (camera.Motion)
                    continue; // the moving camera is a temporal-quality probe, not a cost pose
                poses.push_back(Pose{ camera.Id.ToStdString(), camera.Position, glm::radians(camera.YawDegrees),
                                      glm::radians(camera.PitchDegrees), camera.FovDegrees, camera.NearClip,
                                      camera.FarClip });
            }
            return std::make_pair(fs::path(manifest->ScenePath.ToStdString()), std::move(poses));
        }

        [[nodiscard]] nlohmann::json MemoryJson()
        {
            const FoliageRenderer& plants = Plants();
            const FFoliageMemoryBreakdown memory = plants.GetMemoryBreakdown();
            const RendererMemoryReport report = RendererMemoryTracker::GetInstance().BuildReport();

            nlohmann::json slots = nlohmann::json::array();
            for (const u64 bytes : memory.CullStreamBytes)
                slots.push_back(bytes);
            nlohmann::json out = {
                { "breakdownBytes",
                  { { "instanceBuffers", memory.InstanceBufferBytes },
                    { "cullGroupTables", memory.CullLayerBytes },
                    { "cullStreamsPerViewSlot", slots },
                    { "cardGeometry", memory.CardGeometryBytes },
                    { "cardTextures", memory.CardTextureBytes },
                    { "impostorAtlases", memory.ImpostorAtlasBytes },
                    { "meshGeometry", memory.MeshGeometryBytes },
                    { "meshPartTextures", memory.MeshPartTextureBytes },
                    { "pinnedTotal", memory.PinnedBytes() } } },
                { "streamingStats",
                  { { "pinnedGpuBytes", plants.GetStreamingStats().PinnedGpuBytes },
                    { "optionalGpuBytes", plants.GetStreamingStats().OptionalGpuBytes },
                    { "canonicalCpuBytes", plants.GetStreamingStats().CanonicalCpuBytes },
                    { "residentLayers", plants.GetStreamingStats().ResidentLayers },
                    { "pendingLayers", plants.GetStreamingStats().PendingLayers } } },
                { "reportGpuResidentBytes", report.Gpu.ResidentBytes() },
                { "reportCpuResidentBytes", report.Cpu.ResidentBytes() },
            };
            nlohmann::json owners = nlohmann::json::array();
            for (const auto& row : report.Owners)
            {
                if (row.Owner.ToView().find("Foliage") == std::string_view::npos)
                    continue;
                owners.push_back({ { "owner", row.Owner.ToStdString() },
                                   { "lifetime", ToString(row.Lifetime) },
                                   { "gpuLiveBytes", row.GpuLiveBytes },
                                   { "cpuLiveBytes", row.CpuLiveBytes },
                                   { "allocations", row.AllocationCount } });
            }
            out["reportOwners"] = std::move(owners);
            nlohmann::json capacity = nlohmann::json::array();
            for (const auto& row : report.Capacity)
            {
                if (row.Owner.ToView() != "FoliageRenderer")
                    continue;
                capacity.push_back({ { "category", row.Category.ToStdString() },
                                     { "gpu", row.IsGpu },
                                     { "bytes", row.CapacityBytes.value_or(0u) } });
            }
            out["reportCapacityRows"] = std::move(capacity);

            nlohmann::json layers = nlohmann::json::array();
            const auto& records = plants.GetInstanceRegistry().GetRecords();
            std::vector<u32> placed(static_cast<sizet>(FoliageOf().m_Layers.Num()), 0u);
            for (const auto& record : records)
            {
                if (record.m_LayerIndex < placed.size())
                    ++placed[record.m_LayerIndex];
            }
            for (i32 i = 0; i < FoliageOf().m_Layers.Num(); ++i)
            {
                const FoliageLayer& layer = FoliageOf().m_Layers[i];
                const ImpostorAtlas* atlas = plants.GetImpostorAtlas(static_cast<u32>(i));
                layers.push_back({ { "name", layer.Name.ToStdString() },
                                   { "placed", placed[static_cast<sizet>(i)] },
                                   { "useAuthoredMesh", layer.UseAuthoredMesh },
                                   { "useImpostor", layer.UseImpostor },
                                   { "useDensityLod", layer.UseDensityLod },
                                   { "impostorAtlasResolution", layer.ImpostorAtlasResolution },
                                   { "impostorAtlasBytes", atlas != nullptr ? TextureBytes(atlas->Albedo) + TextureBytes(atlas->NormalDepth) : 0u } });
            }
            out["layers"] = std::move(layers);
            return out;
        }

        // Survivors of the main view's cull, per layer. STALLS (a readback),
        // so it is taken outside every timed window.
        [[nodiscard]] nlohmann::json MainViewCensus()
        {
            nlohmann::json out = nlohmann::json::object();
            const FoliageRenderer& plants = Plants();
            for (u32 i = 0; i < plants.GetLayerCount(); ++i)
            {
                FoliageGPUCuller::Readback readback;
                if (plants.ReadbackCull(i, FoliageGPUCuller::ViewSlot::Main, readback))
                    out[FoliageOf().m_Layers[static_cast<i32>(i)].Name.ToStdString()] = readback.Visible;
            }
            return out;
        }

        std::optional<Loaded> m_Loaded;
        u32 m_Width = kWidth;
        u32 m_Height = kHeight;
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssetManager;
        fs::path m_PreviousCwd;
    };

    // ── The contract ─────────────────────────────────────────────────────────

    // Every GPU stage foliage runs is bracketed, on the path and shadow
    // technique each stage exists on, and no frame overflows the pool's
    // per-frame bracket budget (an overflow would silently drop the brackets
    // past it into an "untimed" count).
    TEST_F(FoliageCostBaselineTest, EveryFoliageGpuStageHasItsOwnTimerBracket)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_NO_FATAL_FAILURE(Load(kTraversalScene));
        const auto path = DeriveTraversal();
        ASSERT_TRUE(path.has_value()) << "no meadow-to-woodland path through the committed scene";
        const Pose& meadow = path->front();

        struct Expectation
        {
            RenderingPath Path;
            bool Vsm;
            std::vector<std::string> Brackets;
        };
        const std::vector<Expectation> cells{
            { RenderingPath::Forward, false, { "FoliageCull", "ShadowPass/FoliageCull", "ShadowPass/FoliageCasters", "FoliagePass" } },
            { RenderingPath::ForwardPlus, false, { "FoliageCull", "ShadowPass/FoliageCull", "ShadowPass/FoliageCasters", "FoliagePass" } },
            // Deferred draws foliage into the G-Buffer inside ScenePass; its
            // share there is measured by difference (the NoFoliage arm).
            { RenderingPath::Deferred, false, { "FoliageCull", "ShadowPass/FoliageCull", "ShadowPass/FoliageCasters", "ScenePass" } },
            { RenderingPath::Deferred, true, { "FoliageCull", "ShadowPass/FoliageCull", "ShadowPass/FoliageCasters" } },
        };
        for (const auto& cell : cells)
        {
            SCOPED_TRACE(std::string(PathName(cell.Path)) + (cell.Vsm ? " VSM" : " CSM"));
            SetPath(cell.Path);
            ASSERT_TRUE(SetVsm(cell.Vsm)) << "the shadow technique did not take";
            Tick(meadow, 8);
            const Window window = Measure(meadow, 6);
            ASSERT_FALSE(window.Passes.empty()) << "no frame's GPU timings resolved";
            EXPECT_EQ(window.OverflowFrames, 0u) << "frames ran out of GPU timer brackets";
            const PassTotals& frame = window.Passes.back();
            for (const auto& bracket : cell.Brackets)
                EXPECT_TRUE(frame.contains(bracket) && std::isfinite(frame.at(bracket)))
                    << "no valid '" << bracket << "' bracket";
            const auto culls = window.LastFrameBracketCounts.find("ShadowPass/FoliageCull");
            const u32 shadowCulls = culls == window.LastFrameBracketCounts.end() ? 0u : culls->second;
            std::printf("[foliage-cost] %s %s: %u shadow-view cull bracket(s), %zu bracket names\n", PathName(cell.Path),
                        cell.Vsm ? "VSM" : "CSM", shadowCulls, frame.size());
            // The CSM region culls all its cascades inside ONE bracket; the
            // VSM culls each virtual view just before drawing it, in a bracket
            // of its own. More than one is what says the VSM views draw foliage.
            if (cell.Vsm)
                EXPECT_GT(shadowCulls, 1u) << "the VSM views did not cull foliage per view";
            else
                EXPECT_EQ(shadowCulls, 1u);
        }
    }

    // The breakdown splits the streaming floor exactly, the report carries it
    // as capacity rows, and the backings it describes are booked to the
    // foliage owner. The cull streams are the case this exists for: they are
    // created on the first cull after a generation, outside GenerateInstances'
    // owner scope, and were booked to nobody (main view) or to ShadowPass.
    TEST_F(FoliageCostBaselineTest, MemoryBreakdownSplitsTheFloorAndIsBookedToFoliage)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetPath(RenderingPath::Forward);
        ASSERT_TRUE(SetVsm(false));
        ASSERT_NO_FATAL_FAILURE(Load(kTraversalScene));
        const auto path = DeriveTraversal();
        ASSERT_TRUE(path.has_value());
        Tick(path->front(), 6);

        const FFoliageMemoryBreakdown memory = Plants().GetMemoryBreakdown();
        // The stats refresh once a frame, before that frame's culls; after a
        // steady frame the two agree to the byte.
        EXPECT_EQ(memory.PinnedBytes(), Plants().GetStreamingStats().PinnedGpuBytes);
        EXPECT_GT(memory.InstanceBufferBytes, 0u);
        EXPECT_GT(memory.CullLayerBytes, 0u);
        EXPECT_GT(memory.CullStreamBytes[static_cast<u32>(FoliageGPUCuller::ViewSlot::Main)], 0u) << "main view";
        EXPECT_GT(memory.CullStreamBytes[FoliageGPUCuller::ShadowSlot(0)], 0u) << "first shadow view";
        EXPECT_GT(memory.CardGeometryBytes, 0u);
        EXPECT_GT(memory.CardTextureBytes, 0u);
        EXPECT_GT(memory.MeshGeometryBytes, 0u) << "the pines and shrubs author a mesh";

        // The impostor category is the atlases the layers actually hold,
        // computed here from the public atlas rather than from the category's
        // own walk.
        u64 atlases = 0;
        for (u32 i = 0; i < Plants().GetLayerCount(); ++i)
        {
            if (const ImpostorAtlas* atlas = Plants().GetImpostorAtlas(i))
                atlases += TextureBytes(atlas->Albedo) + TextureBytes(atlas->NormalDepth);
        }
        EXPECT_GT(atlases, 0u) << "the woodland pines bake an impostor";
        EXPECT_EQ(memory.ImpostorAtlasBytes, atlases);

        const RendererMemoryReport report = RendererMemoryTracker::GetInstance().BuildReport();
        u64 pinnedRows = 0;
        u32 rows = 0;
        for (const auto& row : report.Capacity)
        {
            if (row.Owner.ToView() != "FoliageRenderer" || !row.Category.ToView().starts_with("Pinned: "))
                continue;
            ++rows;
            pinnedRows += row.CapacityBytes.value_or(0u);
        }
        EXPECT_EQ(rows, 7u) << "one capacity row per pinned category";
        EXPECT_EQ(pinnedRows, memory.PinnedBytes());

        // Booked physically to the foliage owner. On GL the tracker books a
        // format estimate from the same table GetMemoryBreakdown sizes from,
        // so the owner holds at least the pinned floor; a cull stream booked
        // to nobody falls short of it by that stream.
        const u64 owned = OwnerGpuLiveBytes(report, "Foliage pinned representations");
        std::printf("[foliage-cost] pinned %llu B (cull streams %llu B), booked to the foliage owner %llu B\n",
                    static_cast<unsigned long long>(memory.PinnedBytes()),
                    static_cast<unsigned long long>(memory.CullStreamTotalBytes()), static_cast<unsigned long long>(owned));
        EXPECT_GE(owned, memory.PinnedBytes()) << "pinned backings are booked to another owner (or none)";
    }

    // The density-LOD lever reaches the cull, not only the draw: with it set
    // the main view keeps more of a density-LOD layer far away, and with it
    // cleared the shipped count comes back.
    TEST_F(FoliageCostBaselineTest, NoDensityLodLeverReachesTheCull)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetPath(RenderingPath::Forward);
        ASSERT_TRUE(SetVsm(false));
        ASSERT_NO_FATAL_FAILURE(Load(kTraversalScene));
        const auto path = DeriveTraversal();
        ASSERT_TRUE(path.has_value());
        // Looking back over the meadow from the woodland end: the grass is
        // tens of metres away, where its density LOD has thinned it.
        Pose back = path->back();
        back.Yaw += glm::pi<f32>();

        const auto survivors = [&]()
        {
            Tick(back, 4);
            u32 total = 0;
            for (u32 i = 0; i < Plants().GetLayerCount(); ++i)
            {
                if (!FoliageOf().m_Layers[static_cast<i32>(i)].UseDensityLod)
                    continue;
                FoliageGPUCuller::Readback readback;
                if (Plants().ReadbackCull(i, FoliageGPUCuller::ViewSlot::Main, readback))
                    total += readback.Visible;
            }
            return total;
        };

        const u32 shipped = survivors();
        Levers::SetFoliageNoDensityLod(true);
        const u32 unthinned = survivors();
        Levers::SetFoliageNoDensityLod(false);
        const u32 restored = survivors();

        std::printf("[foliage-cost] density-LOD survivors: shipped %u, lever %u, restored %u\n", shipped, unthinned, restored);
        ASSERT_GT(shipped, 0u) << "the pose sees no density-LOD layer at all";
        EXPECT_GT(unthinned, shipped) << "the lever did not stop the cull's density drop";
        EXPECT_EQ(restored, shipped) << "clearing the lever did not restore the shipped cull";
    }

    // Reading the cull back is a diagnostic, so it must leave the frames after
    // it costing what they did before it. It read the live draw buffers
    // straight back, and the GL driver then kept them where the CPU could read
    // them: on an RTX 4090 the main view's cull went from 0.09 to 1.9 ms and
    // its forward draw from 3 to 21 ms, with identical survivor counts, for the
    // rest of the process (#1391).
    TEST_F(FoliageCostBaselineTest, ReadingTheCullBackLeavesLaterFramesAsCheap)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        SetPath(RenderingPath::Forward);
        ASSERT_TRUE(SetVsm(false));
        ASSERT_NO_FATAL_FAILURE(Load(kTraversalScene));
        const auto path = DeriveTraversal();
        ASSERT_TRUE(path.has_value());
        const Pose& pose = (*path)[path->size() / 2u];

        const auto medians = [&]()
        {
            Tick(pose, 8);
            const Window window = Measure(pose, 12);
            // Only frames that stamped the bracket: a missing one is a broken
            // instrument, which the finiteness check below fails on, not a
            // zero-cost cull.
            std::vector<f64> cull;
            std::vector<f64> draw;
            for (const auto& frame : window.Passes)
            {
                if (frame.contains("FoliageCull"))
                    cull.push_back(frame.at("FoliageCull"));
                if (frame.contains("FoliagePass"))
                    draw.push_back(frame.at("FoliagePass"));
            }
            return std::pair{ Percentile(cull, 0.5), Percentile(draw, 0.5) };
        };

        const auto [cullBefore, drawBefore] = medians();
        u32 readLayers = 0;
        for (u32 i = 0; i < Plants().GetLayerCount(); ++i)
        {
            FoliageGPUCuller::Readback readback;
            readLayers += Plants().ReadbackCull(i, FoliageGPUCuller::ViewSlot::Main, readback) ? 1u : 0u;
        }
        const auto [cullAfter, drawAfter] = medians();
        std::printf("[foliage-cost] main view before/after a cull readback: cull %.3f/%.3f ms, draw %.2f/%.2f ms\n",
                    cullBefore, cullAfter, drawBefore, drawAfter);

        ASSERT_GT(readLayers, 0u) << "nothing was read back, so nothing was tested";
        ASSERT_TRUE(std::isfinite(cullBefore) && std::isfinite(cullAfter))
            << "no valid 'FoliageCull' bracket to compare (see FoliageRenderer::DispatchMainViewCulling)";
        if (cullBefore <= 0.0)
            GTEST_SKIP() << "the timestamps are too coarse to time a sub-millisecond cull on this device";
        // On the CULL, whose regression was 13-22x: a 4x bound sits far from it
        // and far from what a neighbour's load does to two adjacent ~1 s
        // windows. The draw (4-6x when broken) is logged, not asserted.
        EXPECT_LT(cullAfter, 4.0 * cullBefore + 0.05) << "the readback left the main view's cull slower";
    }

    // ── The measurement ──────────────────────────────────────────────────────

    TEST_F(FoliageCostBaselineTest, MeasureBaseline)
    {
        if (!EnvFlag("OLO_FOLIAGE_COST"))
            GTEST_SKIP() << "set OLO_FOLIAGE_COST=1 to measure (scripts/perf/foliage-cost-baseline.py runs it)";
        OLO_ENSURE_GPU_OR_SKIP();

        const u32 rounds = EnvCount("OLO_FOLIAGE_COST_ROUNDS", 3u);
        const u32 warmup = EnvCount("OLO_FOLIAGE_COST_WARMUP", 12u);
        const u32 frames = EnvCount("OLO_FOLIAGE_COST_FRAMES", 16u);
        const u32 tailFrames = EnvCount("OLO_FOLIAGE_COST_TAIL_FRAMES", 600u);
        const u32 rebuildEvery = EnvCount("OLO_FOLIAGE_COST_REBUILD_EVERY", 100u);
        const char* outEnv = std::getenv("OLO_FOLIAGE_COST_OUT");
        const fs::path outDir = (outEnv != nullptr && outEnv[0] != '\0') ? fs::path(outEnv) : TempDir("out");
        std::error_code ec;
        fs::create_directories(outDir, ec);
        ASSERT_FALSE(ec) << outDir.string();

        nlohmann::json result;
        result["issue"] = 1391;
        result["host"] = {
            { "glVendor", reinterpret_cast<const char*>(glGetString(GL_VENDOR)) },
            { "glRenderer", reinterpret_cast<const char*>(glGetString(GL_RENDERER)) },
            { "glVersion", reinterpret_cast<const char*>(glGetString(GL_VERSION)) },
#if defined(NDEBUG)
            { "buildConfig", "optimised (NDEBUG)" },
#else
            { "buildConfig", "Debug" },
#endif
        };
        result["protocol"] = { { "width", kWidth },
                               { "height", kHeight },
                               { "mockTimeSeconds", kMockTime },
                               { "rounds", rounds },
                               { "warmupFramesPerBlock", warmup },
                               { "measuredFramesPerBlock", frames },
                               { "tailFrames", tailFrames },
                               { "rebuildEvery", rebuildEvery },
                               { "deadlineMs", kDeadlineMs },
                               { "armOrder", "rotated by one per round (Latin square over 4 arms)" },
                               { "wallMs", "one guarded Scene::OnUpdateEditor tick: CPU submission plus driver back-pressure; headless, no present" },
                               { "gpuMs", "GPUPassTimerPool whole-frame span, valid samples only, matched to the tick by frame number" } };

        struct Subject
        {
            std::string Id;
            fs::path Scene;
            std::vector<Pose> Poses;
        };
        std::vector<Subject> subjects;

        // ── Memory, once per subject, and the poses ──────────────────────────
        nlohmann::json memory = nlohmann::json::object();
        SetPath(RenderingPath::Forward);
        ASSERT_TRUE(SetVsm(false));
        {
            ASSERT_NO_FATAL_FAILURE(Load(kTraversalScene));
            const auto path = DeriveTraversal();
            ASSERT_TRUE(path.has_value()) << "no meadow-to-woodland path through the committed scene";
            subjects.push_back({ "Traversal", kTraversalScene, *path });
            Tick(path->front(), warmup);
            memory["Traversal"] = MemoryJson();
        }
        for (const auto& [id, manifestFile] : { std::pair{ "Meadow", "meadow.diagnostic.yaml" },
                                                std::pair{ "Woodland", "woodland.diagnostic.yaml" } })
        {
            const auto manifest = ManifestPoses(manifestFile);
            ASSERT_TRUE(manifest.has_value()) << manifestFile;
            subjects.push_back({ id, manifest->first, manifest->second });
            ASSERT_NO_FATAL_FAILURE(Load(manifest->first));
            Tick(manifest->second.front(), warmup);
            memory[id] = MemoryJson();
        }
        result["memory"] = std::move(memory);

        nlohmann::json poses = nlohmann::json::object();
        for (const auto& subject : subjects)
        {
            nlohmann::json list = nlohmann::json::array();
            for (const auto& pose : subject.Poses)
                list.push_back({ { "name", pose.Name },
                                 { "eye", { pose.Eye.x, pose.Eye.y, pose.Eye.z } },
                                 { "yawDegrees", glm::degrees(pose.Yaw) },
                                 { "pitchDegrees", glm::degrees(pose.Pitch) },
                                 { "fovDegrees", pose.FovDegrees } });
            poses[subject.Id] = { { "scene", subject.Scene.generic_string() }, { "poses", std::move(list) } };
        }
        result["poses"] = std::move(poses);

        // ── One interleaved A/B cell ─────────────────────────────────────────
        nlohmann::json cells = nlohmann::json::array();
        u64 coldRebuildFrames = 0;
        std::vector<f64> coldRebuildWallMs;
        // Every switch back on after NoFoliage regenerates every layer: the
        // cold rebuild, timed on its own and never inside a window.
        const auto switchArm = [&](Arm arm, const Pose& pose)
        {
            const bool wasOff = !FoliageOf().m_Enabled;
            ApplyArm(arm);
            if (wasOff && arm != Arm::NoFoliage)
            {
                const auto t0 = std::chrono::steady_clock::now();
                Tick(pose, 1);
                coldRebuildWallMs.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
                ++coldRebuildFrames;
            }
        };
        const auto measureCell = [&](const Subject& subject, const Pose& pose, RenderingPath renderPath, bool vsm,
                                     u32 msaa, std::span<const Arm> arms)
        {
            std::map<Arm, std::vector<Window>> windows;
            const sizet firstColdRebuild = coldRebuildWallMs.size();
            for (u32 round = 0; round < rounds; ++round)
            {
                for (sizet k = 0; k < arms.size(); ++k)
                {
                    const Arm arm = arms[(k + round) % arms.size()];
                    switchArm(arm, pose);
                    Tick(pose, warmup);
                    windows[arm].push_back(Measure(pose, frames));
                }
            }
            switchArm(Arm::Shipped, pose);

            nlohmann::json cell = { { "subject", subject.Id },
                                    { "pose", pose.Name },
                                    { "path", PathName(renderPath) },
                                    { "shadows", vsm ? "VSM" : "CSM" },
                                    { "msaa", msaa },
                                    { "width", m_Width },
                                    { "height", m_Height } };
            // The cell's own cold rebuilds, so a cell excluded for contention
            // takes its rebuilds out of the summary with it.
            cell["coldRebuildWallMs"] = RoundedArray(std::vector<f64>(
                coldRebuildWallMs.begin() + static_cast<std::ptrdiff_t>(firstColdRebuild), coldRebuildWallMs.end()));
            for (const Arm arm : arms)
            {
                std::vector<f64> wall;
                std::vector<f64> gpu;
                std::vector<u64> frameIds;
                std::map<std::string, std::vector<f64>> perPass;
                u32 missing = 0;
                u32 overflow = 0;
                for (const auto& window : windows[arm])
                {
                    wall.insert(wall.end(), window.WallMs.begin(), window.WallMs.end());
                    gpu.insert(gpu.end(), window.GpuMs.begin(), window.GpuMs.end());
                    frameIds.insert(frameIds.end(), window.Frames.begin(), window.Frames.end());
                    missing += window.MissingGpu;
                    overflow += window.OverflowFrames;
                    // A bracket absent from a frame is a pass that did not run
                    // that frame (0 ms), not a missing sample: every frame of a
                    // window has the same configuration.
                    for (sizet f = 0; f < window.Passes.size(); ++f)
                    {
                        for (const auto& [name, ms] : window.Passes[f])
                        {
                            auto& series = perPass[name];
                            series.resize(gpu.size() - window.Passes.size() + f, 0.0);
                            series.push_back(ms);
                        }
                    }
                }
                for (auto& [name, series] : perPass)
                    series.resize(gpu.size(), 0.0);

                nlohmann::json medians = nlohmann::json::object();
                nlohmann::json raw = nlohmann::json::object();
                u32 invalidBrackets = 0;
                for (const auto& [name, series] : perPass)
                {
                    invalidBrackets += static_cast<u32>(std::ranges::count_if(series, [](f64 v)
                                                                              { return !std::isfinite(v); }));
                    medians[name] = Round4(Percentile(series, 0.5));
                    if (KeepRawSamples(name))
                        raw[name] = RoundedArray(series);
                }
                cell["arms"][ArmName(arm)] = {
                    { "wallMs", TailJson(wall) },
                    { "gpuMs", TailJson(gpu) },
                    { "missingGpuSamples", missing },
                    { "overflowFrames", overflow },
                    { "invalidBracketSamples", invalidBrackets },
                    { "passMedianMs", std::move(medians) },
                    { "raw", { { "wallMs", RoundedArray(wall) }, { "gpuMs", RoundedArray(gpu) }, { "frame", frameIds }, { "passes", std::move(raw) } } },
                };
            }
            if (std::ranges::find(arms, Arm::Shipped) != arms.end())
            {
                cell["shippedBracketCounts"] = windows[Arm::Shipped].back().LastFrameBracketCounts;
                Tick(pose, 2);
                cell["mainViewSurvivors"] = MainViewCensus();
            }
            cells.push_back(std::move(cell));
            std::printf("[foliage-cost] %s %s %s %s msaa%u %ux%u done\n", subject.Id.c_str(), pose.Name.c_str(),
                        PathName(renderPath), vsm ? "VSM" : "CSM", msaa, m_Width, m_Height);
            std::fflush(stdout);
        };

        // ── The matrix: every subject x path x shadow technique x pose ───────
        //
        // OLO_FOLIAGE_COST_{SUBJECTS,PATHS,SHADOWS,POSES} (comma lists) narrow
        // it, and OLO_FOLIAGE_COST_EXTRAS=0 skips the conditional cells and the
        // tails: a cell a foreign GPU process overlapped is re-measured on its
        // own instead of in another full run.
        const std::array paths{ RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred };
        for (const auto& subject : subjects)
        {
            if (!EnvSelects("OLO_FOLIAGE_COST_SUBJECTS", subject.Id))
                continue;
            ASSERT_NO_FATAL_FAILURE(Load(subject.Scene));
            for (const RenderingPath renderPath : paths)
            {
                if (!EnvSelects("OLO_FOLIAGE_COST_PATHS", PathName(renderPath)))
                    continue;
                SetPath(renderPath);
                for (const bool vsm : { false, true })
                {
                    if (!EnvSelects("OLO_FOLIAGE_COST_SHADOWS", vsm ? "VSM" : "CSM"))
                        continue;
                    ASSERT_TRUE(SetVsm(vsm)) << "the shadow technique did not take";
                    for (const auto& pose : subject.Poses)
                    {
                        if (!EnvSelects("OLO_FOLIAGE_COST_POSES", pose.Name))
                            continue;
                        measureCell(subject, pose, renderPath, vsm, 1u, kAllArms);
                        ASSERT_FALSE(HasFatalFailure());
                    }
                }
            }
        }

        const char* extras = std::getenv("OLO_FOLIAGE_COST_EXTRAS");
        if (extras != nullptr && extras[0] == '0')
        {
            result["cells"] = std::move(cells);
            result["coldRebuild"] = { { "frames", coldRebuildFrames }, { "wallMs", TailJson(coldRebuildWallMs) }, { "rawWallMs", RoundedArray(coldRebuildWallMs) } };
            result["tails"] = nlohmann::json::array();
            SetPath(RenderingPath::Forward);
            std::ofstream(outDir / "foliage-cost.json") << result.dump(1);
            std::printf("[foliage-cost] wrote %s (no conditional cells, no tails)\n", (outDir / "foliage-cost.json").string().c_str());
            return;
        }

        // ── Conditional cells on the traversal: resolution and MSAA ──────────
        const Subject& traversal = subjects.front();
        const std::array<Pose, 3> ends{ traversal.Poses.front(), traversal.Poses[traversal.Poses.size() / 2u],
                                        traversal.Poses.back() };
        constexpr std::array kControlArms{ Arm::Shipped, Arm::NoFoliage };
        ASSERT_NO_FATAL_FAILURE(Load(traversal.Scene));
        ASSERT_TRUE(SetVsm(false));
        for (const auto& [w, h] : { std::pair{ 1280u, 720u }, std::pair{ 2560u, 1440u } })
        {
            Resize(w, h);
            SetPath(RenderingPath::Deferred);
            for (const auto& pose : ends)
                measureCell(traversal, pose, RenderingPath::Deferred, false, 1u, kControlArms);
        }
        Resize(kWidth, kHeight);
        const u32 msaa = std::min(4u, std::max(1u, Renderer3D::GetMaxMSAASamples()));
        SetPath(RenderingPath::Deferred, msaa);
        for (const auto& pose : ends)
            measureCell(traversal, pose, RenderingPath::Deferred, false, msaa, kControlArms);
        SetPath(RenderingPath::Deferred, 1u);
        result["cells"] = std::move(cells);
        result["coldRebuild"] = { { "frames", coldRebuildFrames }, { "wallMs", TailJson(coldRebuildWallMs) }, { "rawWallMs", RoundedArray(coldRebuildWallMs) } };

        // ── Tails: steady windows, and the same with an in-place rebuild ─────
        nlohmann::json tails = nlohmann::json::array();
        for (const RenderingPath renderPath : paths)
        {
            SetPath(renderPath);
            for (const bool vsm : { false, true })
            {
                ASSERT_TRUE(SetVsm(vsm));
                for (const auto& pose : ends)
                {
                    for (const bool rebuild : { false, true })
                    {
                        Tick(pose, warmup);
                        std::vector<u32> rebuildIndices;
                        const Window window = Measure(pose, tailFrames,
                                                      [&](u32 i)
                                                      {
                                                          if (rebuild && i > 0u && i % rebuildEvery == 0u)
                                                          {
                                                              FoliageOf().m_NeedsRebuild = true;
                                                              rebuildIndices.push_back(i);
                                                          }
                                                      });
                        std::vector<f64> rebuildWall;
                        for (const u32 i : rebuildIndices)
                            rebuildWall.push_back(window.WallMs[i]);
                        tails.push_back({ { "pose", pose.Name },
                                          { "path", PathName(renderPath) },
                                          { "shadows", vsm ? "VSM" : "CSM" },
                                          { "rebuildEvery", rebuild ? rebuildEvery : 0u },
                                          { "wallMs", TailJson(window.WallMs) },
                                          { "gpuMs", TailJson(window.GpuMs) },
                                          { "missingGpuSamples", window.MissingGpu },
                                          { "rebuildFrameWallMs", RoundedArray(rebuildWall) },
                                          { "raw", { { "wallMs", RoundedArray(window.WallMs) }, { "gpuMs", RoundedArray(window.GpuMs) } } } });
                        std::printf("[foliage-cost] tail %s %s %s rebuild=%d done\n", pose.Name.c_str(),
                                    PathName(renderPath), vsm ? "VSM" : "CSM", rebuild ? 1 : 0);
                        std::fflush(stdout);
                    }
                }
            }
        }
        result["tails"] = std::move(tails);
        SetPath(RenderingPath::Forward);

        const fs::path file = outDir / "foliage-cost.json";
        std::ofstream(file) << result.dump(1);
        std::printf("[foliage-cost] wrote %s\n", file.string().c_str());
    }
} // namespace OloEngine::Tests
