#pragma once

// The same authored-scene fixture drives the L6 cost contracts and the L8
// VSM cut-over evidence. Keep scene setup and traversal identical in both.

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Core/FrameTimeTail.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AlphaCoverageMips.h"
#include "OloEngine/Renderer/Benchmark/BenchmarkManifest.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/Debug/RendererMemoryFormat.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Shadow/FoliageShadowInvalidation.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include <stb_image/stb_image_write.h>
#include "OloEngine/Memory/PlatformMemoryBackend.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glad/gl.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace FoliageCostSupport
    {
        namespace fs = std::filesystem;

        // The issue's named size. The conditional cells add two more.
        inline constexpr u32 kWidth = 1920;
        inline constexpr u32 kHeight = 1080;
        inline constexpr f32 kMockTime = 4.0f;
        // 60 Hz, the deadline the miss counts are taken against.
        inline constexpr f32 kDeadlineMs = 1000.0f / 60.0f;

        inline const fs::path kTraversalScene = fs::path("Scenes") / "FoliageMeadowToWoodland.olo";
        inline constexpr const char* kMeadowLayer = "Meadow Grass";
        inline constexpr const char* kWoodlandLayer = "Woodland Pines";
        // The path FloraTraversalEvidenceTest walks: nine poses from the
        // meadow's densest bucket to the woodland's, eye 2.6 m over the ground.
        inline constexpr u32 kTraversalSteps = 9;

        [[nodiscard]] inline fs::path EditorRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT };
        }

        [[nodiscard]] inline bool EnvFlag(const char* name)
        {
            const char* v = std::getenv(name);
            return v != nullptr && v[0] == '1';
        }

        [[nodiscard]] inline u32 EnvCount(const char* name, u32 fallback)
        {
            const char* v = std::getenv(name);
            if (v == nullptr || v[0] == '\0')
                return fallback;
            const long parsed = std::strtol(v, nullptr, 10);
            return parsed > 0 ? static_cast<u32>(parsed) : fallback;
        }

        // True when `name` is unset or empty, or lists `value` (comma-separated).
        [[nodiscard]] inline bool EnvSelects(const char* name, std::string_view value)
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

        [[nodiscard]] inline const char* PathName(RenderingPath path)
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

        [[nodiscard]] inline EditorCamera MakeCamera(const Pose& pose, u32 width, u32 height)
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
        inline constexpr std::array kAllArms{ Arm::Shipped, Arm::CpuCull, Arm::NoDensityLod, Arm::NoFoliage };

        [[nodiscard]] inline const char* ArmName(Arm arm)
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
        [[nodiscard]] inline bool IsOverflowEntry(const GPUPassTiming& pass)
        {
            return pass.Name.ToView().starts_with("<");
        }

        // The brackets whose raw per-frame samples are kept (every bracket's
        // median is kept regardless): foliage's own, and the passes a foliage
        // share is taken out of.
        [[nodiscard]] inline bool KeepRawSamples(std::string_view name)
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
            std::vector<FFoliageRebuildTimings> RebuildTimings;
            std::vector<u32> PublicationIndices;
            std::vector<f64> PublicationLatencyMs;
            u64 PeakKnownStagingArrayBytes = 0;
            u64 PeakProcessWorkingSetBytes = 0;
            u64 PeakProcessCommitBytes = 0;
            u64 StartGpuResidentBytes = 0;
            u64 PeakGpuResidentBytes = 0;
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

        [[nodiscard]] inline f64 Percentile(std::vector<f64> values, f64 q)
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

        [[nodiscard]] inline nlohmann::json TailJson(const std::vector<f64>& samples)
        {
            FrameTimeTail tail(static_cast<u32>(std::max<sizet>(samples.size(), 1u)));
            for (const f64 s : samples)
                tail.Push(static_cast<f32>(s));
            const FrameTimeTailStats stats = tail.Query(kDeadlineMs);
            return { { "count", stats.SampleCount }, { "min", stats.MinMs }, { "mean", stats.MeanMs }, { "p50", stats.P50Ms }, { "p95", stats.P95Ms }, { "p99", stats.P99Ms }, { "max", stats.MaxMs }, { "deadlineMs", stats.BudgetMs }, { "deadlineMisses", stats.OverBudgetFrames } };
        }

        [[nodiscard]] inline f64 Round4(f64 v)
        {
            return std::isfinite(v) ? std::round(v * 10000.0) / 10000.0 : v;
        }

        [[nodiscard]] inline nlohmann::json RoundedArray(const std::vector<f64>& values)
        {
            nlohmann::json out = nlohmann::json::array();
            for (const f64 v : values)
                out.push_back(Round4(v));
            return out;
        }

        [[nodiscard]] inline u64 TextureBytes(const Ref<Texture2D>& texture)
        {
            if (!texture)
                return 0u;
            const auto& spec = texture->GetSpecification();
            return RendererMemoryFormat::ImageBytes(spec.Format, spec.Width, spec.Height, texture->GetMipLevelCount(), 1u,
                                                    spec.Samples)
                .value_or(0u);
        }

        [[nodiscard]] inline u64 OwnerGpuLiveBytes(const RendererMemoryReport& report, std::string_view owner)
        {
            u64 bytes = 0;
            for (const auto& row : report.Owners)
            {
                if (row.Owner.ToView() == owner)
                    bytes += row.GpuLiveBytes;
            }
            return bytes;
        }
    } // namespace FoliageCostSupport

    using namespace FoliageCostSupport;

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
            LowLevelTasks::InitGameThreadId();
            Tasks::FNamedThreadManager::Get().AttachToThread(Tasks::ENamedThread::GameThread);
            LowLevelTasks::FScheduler::Get().StartWorkers();
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
        void Load(const fs::path& sceneAssetPath, bool useRuntimeFixture = false)
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
            loaded.SceneRef = useRuntimeFixture ? GetSceneRef() : Scene::Create();
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
        [[nodiscard]] FoliageRenderer& Plants()
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

        void AwaitRebuild(const Pose& pose)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (Plants().IsRebuilding() && std::chrono::steady_clock::now() < deadline)
                Tick(pose, 1);
            ASSERT_FALSE(Plants().IsRebuilding()) << "replacement never reached publication";
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
            auto& memoryTracker = RendererMemoryTracker::GetInstance();
            window.StartGpuResidentBytes = memoryTracker.BuildReport().Gpu.ResidentBytes();
            memoryTracker.BeginPeakWindow();
            window.WallMs.reserve(count);
            u64 publishedSerial = Plants().GetPublishedRebuildSerial();
            for (u32 i = 0; i < count; ++i)
            {
                if (beforeFrame)
                    beforeFrame(i);
                const auto t0 = std::chrono::steady_clock::now();
                RunEditorFramesOn(*m_Loaded->SceneRef, camera, 1);
                window.WallMs.push_back(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
                window.RebuildTimings.push_back(Plants().GetRebuildTimings());
                window.PeakKnownStagingArrayBytes = std::max(window.PeakKnownStagingArrayBytes, Plants().GetRebuildStagingCpuBytes());
                PlatformMemoryBackend::BackendStats memory;
                if (PlatformMemoryBackend::QueryStats(memory))
                {
                    window.PeakProcessWorkingSetBytes = std::max(window.PeakProcessWorkingSetBytes, memory.UsedPhysical);
                    window.PeakProcessCommitBytes = std::max(window.PeakProcessCommitBytes, memory.UsedVirtual);
                }
                if (Plants().GetPublishedRebuildSerial() != publishedSerial)
                {
                    publishedSerial = Plants().GetPublishedRebuildSerial();
                    window.PublicationIndices.push_back(i);
                    window.PublicationLatencyMs.push_back(Plants().GetRebuildLatencyMs());
                }
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
            window.PeakGpuResidentBytes = memoryTracker.BuildReport().Gpu.WindowPeakBytes;
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

} // namespace OloEngine::Tests
