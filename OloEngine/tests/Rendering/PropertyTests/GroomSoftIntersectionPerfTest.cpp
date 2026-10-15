// OLO_TEST_LAYER: L6
#include "OloEnginePCH.h"
#include "PerfBaselineGate.h"
#include "RendererAttachedTest.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomSoftIntersection.h"
#include "OloEngine/Groom/GroomCoat.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <filesystem>
#include <limits>
#include <glad/gl.h>

namespace OloEngine::Tests
{
    TEST(GroomSoftIntersectionPerf, AuthoredDimensionsColdCacheBudget)
    {
        // Production caches this walk with the rest geometry. Measure its
        // cold preparation cost with enough curves to exercise subsampling.
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        ASSERT_TRUE(builder.AddGroup("body", group, reason));
        const std::array<f32, 4> widths{ 0.001f, 0.001f, 0.001f, 0.001f };
        for (u32 i = 0; i < 4096u; ++i)
        {
            const f32 x = static_cast<f32>(i) * 0.001f;
            const std::array<glm::vec3, 4> points{
                glm::vec3(x, 0.0f, 0.0f), glm::vec3(x, 0.03f, 0.0f),
                glm::vec3(x, 0.06f, 0.0f), glm::vec3(x, 0.1f, 0.0f)
            };
            GroomCurveInput curve;
            curve.Points = points;
            curve.Widths = widths;
            curve.GroupId = group;
            ASSERT_TRUE(builder.AddCurve(curve, reason));
        }
        auto groom = builder.Build(reason);
        ASSERT_TRUE(groom);
        GroomCoatSettings settings;
        settings.Enabled = true;
        settings.LengthJitter = 0.2f;
        const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
        constexpr auto key = "groom_intersection_dimensions_4096";
        const auto measure = [&]() -> u64
        {
            for (u32 i = 0; i < 5u; ++i)
                (void)MeasureGroomIntersectionDimensions(*groom, &coat);
            std::array<u64, 20> samples{};
            for (auto& sample : samples)
            {
                const auto start = std::chrono::steady_clock::now();
                const auto dimensions = MeasureGroomIntersectionDimensions(*groom, &coat);
                sample = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count());
                EXPECT_EQ(dimensions.SampledCurves, 64u);
                EXPECT_GT(dimensions.MeanLength, 0.0f);
            }
            return *std::min_element(samples.begin(), samples.end());
        };
        ::testing::Test::RecordProperty("measurement_kind", "lower-bound cold rest-geometry preparation cost");
        const auto measured = PerfBaseline::MeasureBenchmarkStableNs(key, measure);
        PerfBaseline::CheckPerfRegression(key, measured);
    }

    class GroomSoftIntersectionGpuPerf : public RendererAttachedTest
    {
      protected:
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssets;
        f32 m_PreviousScale = 1.0f;
        bool m_StateCaptured = false;
        void BuildScene() override
        {
            m_PreviousProject = Project::GetActive();
            m_PreviousScale = Renderer3D::GetRenderScale();
            m_PreviousAssets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            m_StateCaptured = true;
            const std::filesystem::path editor{ OLO_TEST_EDITOR_ROOT };
            ASSERT_TRUE(Project::Load(editor / "SandboxProject/Sandbox.oloproj"));
            auto assets = Ref<EditorAssetManager>::Create();
            assets->Initialize(false);
            Project::SetAssetManager(assets);
            ASSERT_TRUE(SceneSerializer(GetSceneRef()).Deserialize((editor / "SandboxProject/Assets/Scenes/Tests/GroomSoftIntersectionHorse.olo").string()));
            auto coat = GetScene().FindEntityByName("LongCoatHorseCoat");
            ASSERT_TRUE(coat && coat.HasComponent<GroomLodComponent>());
            auto& lod = coat.GetComponent<GroomLodComponent>();
            lod.m_Enabled = false;
            lod.m_HoldFrames = 0;
            lod.m_VisibilitySteps = 0;
            lod.m_SimulationSteps = 0;
            lod.m_ShadowSteps = 0;
            EnableRendering(1280, 720);
            auto& settings = Renderer3D::GetRendererSettings();
            settings = RendererSettings{};
            settings.Path = RenderingPath::Forward;
            settings.EditorDebugDrawsEnabled = false;
            settings.ShowGrid = false;
            settings.ShowComponentGizmos = false;
            auto& post = Renderer3D::GetPostProcessSettings();
            post = PostProcessSettings{};
            post.TAAEnabled = true;
            Renderer3D::SetRenderScale(1.0f);
            Renderer3D::ApplyRendererSettings();
        }
        void TearDown() override
        {
            if (m_StateCaptured)
            {
                Time::ClearMockTime();
                Renderer3D::SetRenderScale(m_PreviousScale);
            }
            RendererAttachedTest::TearDown();
            if (!m_StateCaptured)
                return;
            Project::Unload();
            if (m_PreviousProject)
                Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
            if (m_PreviousAssets)
                Project::SetAssetManager(m_PreviousAssets);
        }
    };

    TEST_F(GroomSoftIntersectionGpuPerf, MeasuresIndependentDepthProducerCosts)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Time::SetMockTime(12.0f);
        EditorCamera camera(45.0f, 1280.0f / 720.0f, 0.02f, 100.0f);
        camera.SetViewportSize(1280.0f, 720.0f);
        camera.SetPose(glm::vec3(0, 1.6f, 5.5f), 0.0f, 0.12f);
        Renderer3D::ResetFrameSequences();
        struct InvalidateBindings
        {
            ~InvalidateBindings()
            {
                CommandDispatch::InvalidateBindingCaches();
            }
        } invalidateBindings;
        GLStateGuard guard("GroomIntersectionPerfBatch", GLStateGuard::Policy::Restore);
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        // Per-tick state snapshots introduce driver stalls that the live editor
        // does not have. Keep the entire benchmark contained, but drive the
        // production tick continuously inside one outer guard.
        const auto tick = [&]()
        { GetScene().OnUpdateEditor(Timestep{ 0.0f }, camera); };
        const auto warmupStart = std::chrono::steady_clock::now();
        u32 warmupFrames = 0;
        do
        {
            tick();
            ++warmupFrames;
        } while (std::chrono::steady_clock::now() - warmupStart < std::chrono::seconds(2));
        glFinish();
        const auto workload = Renderer3D::GetGroomRenderPass()->GetStats();
        ASSERT_EQ(workload.GroomsDrawn, 1u);
        ASSERT_GT(workload.StrandsDrawn, 0u);
        ASSERT_GT(workload.SegmentsDrawn, 0u);
        ASSERT_EQ(workload.GroomsBindingRefused, 0u);
        ::testing::Test::RecordProperty("warmup_frames", std::to_string(warmupFrames));
        ::testing::Test::RecordProperty("strands_drawn", std::to_string(workload.StrandsDrawn));
        ::testing::Test::RecordProperty("segments_drawn", std::to_string(workload.SegmentsDrawn));
        ::testing::Test::RecordProperty("measurement_kind", "lower-bound per-frame GPU pass cost");
        for (const std::string pass : { "GroomSceneDepthSnapshotPass", "GroomSceneEntitySnapshotPass", "GroomWaterDepthPass", "GroomPass" })
        {
            const std::string key = "groom_intersection_1280x720_" + pass;
            const auto measure = [&]() -> u64
            {
                auto& timer = GPUPassTimerPool::GetInstance();
                u64 previous = timer.GetCurrentFrameNumber();
                f64 minimum = std::numeric_limits<f64>::max();
                u32 samples = 0;
                std::array<f64, 20> costs{};
                u64 firstFrame = 0;
                for (u32 frame = 0; frame < 80u && samples < 20u; ++frame)
                {
                    tick();
                    const auto& stats = Renderer3D::GetGroomRenderPass()->GetStats();
                    EXPECT_EQ(stats.GroomsDrawn, workload.GroomsDrawn);
                    EXPECT_EQ(stats.StrandsDrawn, workload.StrandsDrawn);
                    EXPECT_EQ(stats.SegmentsDrawn, workload.SegmentsDrawn);
                    EXPECT_EQ(stats.GroomsBindingRefused, 0u);
                    const auto report = timer.GetLastFrameTimings();
                    if (report.FrameNumber <= previous || report.IsStale())
                        continue;
                    previous = report.FrameNumber;
                    for (const auto& timing : report.Passes)
                        if (timing.Name == pass && timing.IsValid() && std::isfinite(timing.Sample.GpuMs) && timing.Sample.GpuMs > 0.0)
                        {
                            minimum = std::min(minimum, static_cast<f64>(timing.Sample.GpuMs));
                            costs[samples] = timing.Sample.GpuMs;
                            if (samples == 0u)
                                firstFrame = report.FrameNumber;
                            ++samples;
                            break;
                        }
                }
                EXPECT_EQ(samples, 20u) << pass;
                ::testing::Test::RecordProperty(key + "_positive_samples", std::to_string(samples));
                if (samples == 20u)
                {
                    std::ranges::sort(costs);
                    ::testing::Test::RecordProperty(key + "_median_ms", std::to_string((costs[9] + costs[10]) * 0.5));
                    ::testing::Test::RecordProperty(key + "_max_ms", std::to_string(costs.back()));
                    ::testing::Test::RecordProperty(key + "_first_frame", std::to_string(firstFrame));
                    ::testing::Test::RecordProperty(key + "_last_frame", std::to_string(previous));
                }
                return samples == 20u ? static_cast<u64>(std::llround(minimum * 1e6)) : std::numeric_limits<u64>::max();
            };
            const auto ns = measure();
            ASSERT_LT(ns, std::numeric_limits<u64>::max());
            // As in the animal cost evidence, retain measured pass costs and
            // their workload. Adaptive GPU clocks make a single absolute
            // workstation threshold unsuitable for a frame regression claim.
            ::testing::Test::RecordProperty(key + "_ns_lower_bound", std::to_string(ns));
        }
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }
} // namespace OloEngine::Tests
