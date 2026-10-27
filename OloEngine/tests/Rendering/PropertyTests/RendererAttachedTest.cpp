#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Precipitation/PrecipitationSystem.h"
#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscaler.h"
#include "OloEngine/Renderer/Water/WaterDisturbanceSystem.h"
#include "OloEngine/Renderer/Water/WaterRainRippleSystem.h"
#include "OloEngine/Renderer/Water/WaterSpraySystem.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"
#include "OloEngine/Snow/SnowAccumulationSystem.h"
#include "OloEngine/Snow/SnowEjectaSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Task/Scheduler.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"

#include "GLErrorStateCheck.h"
#include "TestRenderBackend.h"
#include "../VulkanCoverageReport.h"

#include <glad/gl.h>

#include <chrono>
#include <functional>

namespace OloEngine::Tests
{
    namespace
    {
        // Drive one full-pipeline render tick with the GL-state + GL-error
        // hygiene this fixture guarantees, so a render here cannot poison a
        // later GPU test in the shared process-wide context (issue #485 / #505):
        //   1. Surface any GL error already pending BEFORE the tick — a
        //      setup / test-body leak the per-tick drain must not silently mask.
        //   2. Run `tick` inside a GLStateGuard(Restore) so global fixed-function
        //      / binding state (blend / stencil / viewport / program / VAO / FBO)
        //      is contained. Per-slot texture / UBO bindings are intentionally
        //      NOT restored by GLStateGuard (see its class comment).
        //   3. Fail on ANY GL error this tick produced, attributing it to the
        //      exact tick. The #505 stale-texture GL_INVALID_OPERATION this
        //      check used to tolerate is fixed at the root (per-frame reset of
        //      the water-depth / planar-reflection texture publications in
        //      RenderPipeline::PrepareFrame), so a full-pipeline render is now
        //      expected to be error-clean — no benign-error carve-out remains.
        //
        // In the --olo-rhi=vulkan process (#1556) the same tick runs as one
        // frame of the windowless VulkanContext instead -- a facade draw
        // outside its frame bracket is dropped -- and the hygiene check is the
        // Vulkan one: no validation error and no unimplemented-stub hit.
        void RunGuardedRenderTick(const char* label, const std::function<void()>& tick)
        {
            if (TestRenderBackend::IsVulkanProcess())
            {
                const TestRenderBackend::FrameResult frame = TestRenderBackend::RunFrame(tick);
                if (!frame.Ran)
                    ADD_FAILURE() << label << ": the windowless Vulkan frame bracket did not run the tick";
                if (frame.NewValidationErrors != 0u)
                    ADD_FAILURE() << label << " render tick raised " << frame.NewValidationErrors
                                  << " Vulkan validation error(s); OloEngine-Tests-<pid>.log has the VUIDs.";
                if (frame.NewStubHits != 0u)
                    ADD_FAILURE() << label << " render tick hit " << frame.NewStubHits
                                  << " VulkanRendererAPI unimplemented stub(s); the log names each one.";
                return;
            }

            if (const u32 preErr = GLErrorState::DrainAndGetFirstError(); preErr != 0u)
                ADD_FAILURE() << "GL error pending before " << label
                              << " render tick (setup/test-body leak, not this render): "
                              << GLErrorState::GlErrorString(preErr);

            {
                GLStateGuard guard(label, GLStateGuard::Policy::Restore);
                tick();
            }

            if (const u32 tickErr = GLErrorState::DrainAndGetFirstError(); tickErr != 0u)
                ADD_FAILURE() << label << " render tick left a GL error: "
                              << GLErrorState::GlErrorString(tickErr)
                              << " (the whole error queue is drained so the next test starts clean).";
        }

        // Whether a foliage regeneration is in flight anywhere in `scene`.
        // Only a started one: a component whose m_NeedsRebuild the scene never
        // consumes (no terrain, no layers) is not waited on.
        [[nodiscard]] bool FoliageRebuilding(Scene& scene)
        {
            for (const auto entity : scene.GetAllEntitiesWith<FoliageComponent>())
            {
                const auto& foliage = Entity{ entity, &scene }.GetComponent<FoliageComponent>();
                if (foliage.m_Renderer && foliage.m_Renderer->IsRebuilding())
                    return true;
            }
            return false;
        }

        // A foliage regeneration the frames started is finished before a
        // RunFrames / RunEditorFrames returns. Once any fixture earlier in the
        // process has started the task workers, a regeneration publishes
        // asynchronously (the staged rebuild), so a fixed frame count after
        // m_NeedsRebuild could still draw the layers from before it: a test
        // that passed alone failed after another suite. A test that must watch
        // a rebuild in flight drives its scene through RunEditorFramesOn.
        void FinishFoliageRegeneration(Scene& scene, const std::function<void()>& tick)
        {
            if (!FoliageRebuilding(scene))
                return;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (FoliageRebuilding(scene) && std::chrono::steady_clock::now() < deadline)
                tick();
            EXPECT_FALSE(FoliageRebuilding(scene)) << "a foliage regeneration never reached publication within 30 s";
        }
    } // namespace

    // The process-wide renderer is brought up lazily (see SetUp). We dedup on
    // the authoritative `Renderer3D::IsInitialized()` rather than a private
    // static so that *any* suite which brings the renderer up — this fixture
    // OR the standalone `AssetSceneLoad` test — is seen by every other, no
    // matter the run order. A private flag would let a second initialiser
    // double-init Renderer3D if it ran first.
    //
    // The matching teardown is NOT here: `Renderer3D::s_Data` is a static
    // holding GL-resource `Ref<>`s that would SIGSEGV if left to destruct
    // at process exit on a dead GL context. The test `main()`
    // (OloEngineTest.cpp) calls `Renderer::Shutdown()` after
    // RUN_ALL_TESTS() — while the shared GL context is still current —
    // which releases those resources for the whole binary.

    void RendererAttachedTest::SetUpTestSuite()
    {
        // Intentionally empty: derived classes are independent test
        // suites in GoogleTest, and `SetUpTestSuite` is not inherited
        // through normal C++ inheritance — gtest looks for it on each
        // derived class via static dispatch. We use a process-wide
        // lazy init inside `SetUp` instead so renderer bring-up
        // happens at most once per process across all derived suites.
    }

    void RendererAttachedTest::TearDownTestSuite()
    {
        // See SetUpTestSuite. Renderer::Shutdown is driven once for the whole
        // process by the test main() (after RUN_ALL_TESTS), not per-suite —
        // other derived suites may still run after this one, and they share
        // the single process-wide Renderer::Init.
    }

    bool RendererAttachedTest::TemporalUpscalerUsable()
    {
        const Ref<TemporalUpscaler> upscaler = TemporalUpscaler::Create();
        return upscaler && upscaler->IsAvailable();
    }

    void RendererAttachedTest::SetUp()
    {
        if (TestRenderBackend::IsVulkanProcess() && !RunsInVulkanProcess())
        {
            GTEST_SKIP() << "this suite is not ported to the --olo-rhi=vulkan process: it reads back by GL name or "
                            "issues GL calls (port it: docs/agent-rules/testing-architecture.md rule 13)";
        }
        if (TestRenderBackend::IsVulkanProcess())
        {
            // The --olo-rhi=vulkan process (#1556). Accounted like every
            // device-gated Vulkan test, so --olo-require-vulkan fails a refusal
            // and the coverage banner counts these cases. The context comes up
            // once; every later test reads its cached verdict.
            VulkanCoverage::MarkCurrentTestDeviceGated();
            const TestRenderBackend::Admission& admission = TestRenderBackend::EnsureWindowlessContext();
            if (!admission.Available)
            {
                VulkanCoverage::RecordGateRefusal(admission.Reason);
                if (VulkanCoverage::Required())
                {
                    FAIL() << "--olo-require-vulkan: RendererAttachedTest could not bring up the windowless "
                              "VulkanContext, and this run would otherwise skip every scene test and pass having "
                              "verified nothing — "
                           << admission.Reason;
                }
                GTEST_SKIP() << "RendererAttachedTest (--olo-rhi=vulkan): " << admission.Reason;
            }
            VulkanCoverage::RecordGateAdmission(admission.DeviceName);
            if (!RenderPropertyFixture::ChangeToOloEditorDir())
                FAIL() << "RendererAttachedTest: could not find OloEditor/ to run from";
            // Opens the GPU gate for this test's own OLO_ENSURE_GPU_OR_SKIP().
            RenderPropertyFixture::SetVulkanSceneTestAdmitted(true);
        }
        else if (!RenderPropertyFixture::IsGpuAvailable())
        {
            // Same two outcomes as OLO_ENSURE_GPU_OR_SKIP, kept in step with it:
            // a plain skip, or a failure under --olo-require-gpu so a GPU job
            // cannot pass by skipping its entire reason for existing.
            if (RenderPropertyFixture::GpuRequired())
            {
                FAIL() << "--olo-require-gpu: RendererAttachedTest could not create a GL 4.6 "
                          "context, and this run would otherwise skip every renderer-attached "
                          "test and pass having verified nothing"
                       << RenderPropertyFixture::GpuUnavailableDetail();
            }
            GTEST_SKIP() << "RendererAttachedTest: no usable GL 4.6 context available "
                            "(CI without GPU or headless server)."
                         << RenderPropertyFixture::GpuUnavailableDetail();
        }

        if (!Renderer3D::IsInitialized())
        {
            Renderer::Init(RendererType::Renderer3D, /*loadingWindow=*/nullptr);
        }

        // THE ENGINE'S WORKER THREADS, as Application starts them (#1630). The
        // test binary constructs no Application, so a renderer-attached test
        // ran every ParallelFor in the frame inline -- unless some earlier test
        // in the process had started the workers (FunctionalTest does, once,
        // and FlockingViaSceneTickTest stops them again). The groom coat's bake
        // and pose then cost ~90 ms a frame on one thread, and whether they did
        // depended on the test order. Checked on every SetUp because a suite
        // may stop them; StartWorkers does nothing while they run.
        if (LowLevelTasks::FScheduler::Get().GetNumWorkers() == 0u)
        {
            LowLevelTasks::FScheduler::Get().StartWorkers();
        }

        // Snapshot the process-wide renderer configuration before BuildScene
        // gets a chance to mutate it; TearDown restores it. See the member
        // comment in the header for why a leaked setting is worse than a
        // leaked GL binding.
        m_SavedSettings.Renderer = Renderer3D::GetRendererSettings();
        m_SavedSettings.PostProcess = Renderer3D::GetPostProcessSettings();
        m_SavedSettings.Shadow = Renderer3D::GetShadowMap().GetSettings();
        m_SavedSettings.Snow = Renderer3D::GetSnowSettings();
        m_SavedSettings.SkinDiffusion = Renderer3D::GetSkinDiffusionSettings();
        m_SavedSettings.Fog = Renderer3D::GetFogSettings();
        m_SavedSettings.Wind = Renderer3D::GetWindSettings();
        m_SavedSettings.SnowAccumulation = Renderer3D::GetSnowAccumulationSettings();
        m_SavedSettings.WaterDisturbance = Renderer3D::GetWaterDisturbanceSettings();
        m_SavedSettings.WaterWake = Renderer3D::GetWaterWakeSettings();
        m_SavedSettings.WaterFoam = Renderer3D::GetWaterFoamSettings();
        m_SavedSettings.SnowEjecta = Renderer3D::GetSnowEjectaSettings();
        m_SavedSettings.Precipitation = Renderer3D::GetPrecipitationSettings();
        m_SettingsSnapshotted = true;

        // Every test renders the frame sequence a fresh process would, so its
        // output does not depend on how many frames earlier tests rendered.
        Renderer3D::ResetFrameSequences();
        // ...and starts from the world state a fresh process has. Each of these
        // systems keeps particles, a field or an intensity in process-global
        // storage that outlives the scene that set it: rain left falling by
        // WaterFoamSprayRain's rain test moved pixels in FoliageWind's paused
        // frame (#1502). Scene::OnRuntimeStart resets some of them; a test that
        // never starts the runtime gets none of that.
        // RendererAttachedIsolation.EveryProcessWideSimulationSystemIsReset
        // fails when a system with a static Reset() is missing here.
        PrecipitationSystem::Reset();
        ScreenSpacePrecipitation::Reset();
        WaterDisturbanceSystem::Reset();
        WaterRainRippleSystem::Reset();
        WaterSpraySystem::Reset();
        WaterWakeSystem::Reset();
        SnowAccumulationSystem::Reset();
        SnowEjectaSystem::Reset();
        FoliageInteractionField::Reset();

        m_Scene = Scene::Create();
        // Rendering is OFF by default: the cheap smoke tests only need the
        // cross-subsystem tick to run post-init. Subclasses that want the
        // full draw path call `EnableRendering(w, h)` from `BuildScene()`.
        m_Scene->SetRenderingEnabled(false);
        m_RenderingEnabled = false;

        BuildScene();
    }

    void RendererAttachedTest::TearDown()
    {
        // Restore the renderer configuration the test started with.
        if (m_SettingsSnapshotted)
        {
            // Restore the authored settings, then re-apply unconditionally.
            // The snapshot captures only these two structs — NOT the render
            // graph's internal "configured-for" state (ActiveGraphPath /
            // ActiveGraphAOTechnique). A struct-level comparison can therefore
            // read "unchanged" while the graph was left reconfigured by an
            // earlier ApplyRendererSettings call (e.g. a test that switched the
            // path or AO technique and then reset the structs by hand), leaking
            // a stale pipeline into later tests. Reasserting every teardown
            // makes the graph match the restored settings regardless of how the
            // test manipulated state. This is cheap: ApplyRendererSettings
            // guards the expensive ConfigureRenderGraph behind an actual
            // path/AO-technique mismatch, so an already-consistent graph costs
            // only a few scalar setters.
            Renderer3D::GetPostProcessSettings() = m_SavedSettings.PostProcess;
            Renderer3D::GetRendererSettings() = m_SavedSettings.Renderer;
            // Through SetSettings, which recreates the shadow textures on a
            // resolution change; a plain PCSS toggle only rewrites the flags.
            Renderer3D::GetShadowMap().SetSettings(m_SavedSettings.Shadow);
            Renderer3D::GetSnowSettings() = m_SavedSettings.Snow;
            Renderer3D::GetSkinDiffusionSettings() = m_SavedSettings.SkinDiffusion;
            Renderer3D::GetFogSettings() = m_SavedSettings.Fog;
            Renderer3D::GetWindSettings() = m_SavedSettings.Wind;
            Renderer3D::GetSnowAccumulationSettings() = m_SavedSettings.SnowAccumulation;
            Renderer3D::GetWaterDisturbanceSettings() = m_SavedSettings.WaterDisturbance;
            Renderer3D::GetWaterWakeSettings() = m_SavedSettings.WaterWake;
            Renderer3D::GetWaterFoamSettings() = m_SavedSettings.WaterFoam;
            Renderer3D::GetSnowEjectaSettings() = m_SavedSettings.SnowEjecta;
            Renderer3D::GetPrecipitationSettings() = m_SavedSettings.Precipitation;
            Renderer3D::ApplyRendererSettings();
            m_SettingsSnapshotted = false;
        }

        // Stopping physics is conditional on whether the test enabled it.
        // For the minimal smoke tests we don't enable physics, so just
        // drop the Scene ref.
        m_Scene.Reset();
        RenderPropertyFixture::SetVulkanSceneTestAdmitted(false);
    }

    void RendererAttachedTest::RunFrames(u32 count, f32 dtSeconds)
    {
        RunFramesInFlight(count, dtSeconds);
        if (count > 0u)
            FinishFoliageRegeneration(*m_Scene, [this, dtSeconds]()
                                      { RunFramesInFlight(1u, dtSeconds); });
    }

    void RendererAttachedTest::RunFramesInFlight(u32 count, f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        const auto tick = [this, ts]()
        {
            if (m_RenderingEnabled)
                RunGuardedRenderTick("RendererAttachedTest::RunFrames",
                                     [this, ts]()
                                     { m_Scene->OnUpdateRuntime(ts); });
            else
                // Rendering disabled: the tick issues no draws, so there is no
                // GL state to contain — skip the guard's per-frame snapshots.
                m_Scene->OnUpdateRuntime(ts);
        };
        for (u32 i = 0; i < count; ++i)
            tick();
    }

    void RendererAttachedTest::RunEditorFrames(const EditorCamera& camera, u32 count, f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        const auto tick = [this, ts, &camera]()
        {
            // Same hygiene as RunFrames — the editor render path
            // (OnUpdateEditor -> RenderScene3D -> EndScene) drives the same full
            // render graph. Containing it here is what lets an editor-camera
            // visual test (e.g. WaterVisualEvidenceTest) run in the normal suite
            // without poisoning the GPU tests that follow it.
            if (m_RenderingEnabled)
                RunGuardedRenderTick("RendererAttachedTest::RunEditorFrames",
                                     [this, ts, &camera]()
                                     { m_Scene->OnUpdateEditor(ts, camera); });
            else
                m_Scene->OnUpdateEditor(ts, camera);
        };
        for (u32 i = 0; i < count; ++i)
            tick();
        if (count > 0u)
            FinishFoliageRegeneration(*m_Scene, tick);
    }

    void RendererAttachedTest::RunEditorFramesOn(Scene& scene, const EditorCamera& camera, u32 count, f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        for (u32 i = 0; i < count; ++i)
            RunGuardedRenderTick("RendererAttachedTest::RunEditorFramesOn",
                                 [&scene, ts, &camera]()
                                 { scene.OnUpdateEditor(ts, camera); });
    }

    void RendererAttachedTest::RunInFrame(const std::function<void()>& work)
    {
        RunGuardedRenderTick("RendererAttachedTest::RunInFrame", work);
    }

    void RendererAttachedTest::SetViewport(u32 width, u32 height)
    {
        m_Scene->OnViewportResize(width, height);
    }

    void RendererAttachedTest::EnableRendering(u32 width, u32 height)
    {
        m_RenderWidth = width;
        m_RenderHeight = height;

        // 3D mode routes OnUpdateRuntime through RenderScene3D + the render
        // graph (the 2D path uses Renderer2D directly and never sizes the
        // graph). OnViewportResize sizes the Scene's cameras;
        // Renderer3D::OnWindowResize sizes the render-graph targets — without
        // the latter the graph would execute against unsized framebuffers.
        m_Scene->SetIs3DModeEnabled(true);
        m_Scene->OnViewportResize(width, height);
        Renderer3D::OnWindowResize(width, height);
        m_Scene->SetRenderingEnabled(true);
        m_RenderingEnabled = true;
    }

    void RendererAttachedTest::ResizeRenderTarget(u32 width, u32 height)
    {
        m_RenderWidth = width;
        m_RenderHeight = height;
        m_Scene->OnViewportResize(width, height);
        Renderer3D::OnWindowResize(width, height);
    }

    bool RendererAttachedTest::ReadbackComposite(std::vector<u8>& outRgba, u32& outWidth, u32& outHeight)
    {
        outWidth = 0;
        outHeight = 0;
        if (!m_RenderingEnabled)
        {
            return false;
        }

        // UIComposite is the render graph's terminal color target — the same
        // framebuffer the editor viewport samples for the 3D scene image
        // (EditorLayer's viewport panel). Reading RT0 back gives the final,
        // tone-mapped, post-processed frame.
        auto framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
        if (!framebuffer)
        {
            return false;
        }

        const auto& spec = framebuffer->GetSpecification();
        outWidth = spec.Width;
        outHeight = spec.Height;

        // By RHI handle, so the read runs on either backend (#1556).
        const RHI::ResourceHandle color = framebuffer->GetColorAttachmentHandle(0);
        if (!color.IsValid() || outWidth == 0 || outHeight == 0)
        {
            return false;
        }

        ReadbackRgba8(color, outWidth, outHeight, outRgba);
        return true;
    }
} // namespace OloEngine::Tests
