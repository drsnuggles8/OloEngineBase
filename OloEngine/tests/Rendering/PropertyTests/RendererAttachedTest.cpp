#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include "GLErrorStateCheck.h"

#include <glad/gl.h>

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
        //   3. Drain the errors THIS tick produced, but only treat the known
        //      benign GL_INVALID_OPERATION stray as expected (the #505 stale-
        //      handle bind — correct pixels, dirty error queue). Any OTHER error
        //      class is a genuine render-side regression and is surfaced, not
        //      swallowed. (A *new* GL_INVALID_OPERATION render bug is
        //      indistinguishable from #505 by enum and remains contained until
        //      #505 is fixed — the residual coverage gap tracked there.)
        void RunGuardedRenderTick(const char* label, const std::function<void()>& tick)
        {
            if (const u32 preErr = GLErrorState::DrainAndGetFirstError(); preErr != 0u)
                ADD_FAILURE() << "GL error pending before " << label
                              << " render tick (setup/test-body leak, not this render): "
                              << GLErrorState::GlErrorString(preErr);

            {
                GLStateGuard guard(label, GLStateGuard::Policy::Restore);
                tick();
            }

            if (const u32 tickErr =
                    GLErrorState::DrainAndGetFirstUnexpected(static_cast<u32>(GL_INVALID_OPERATION));
                tickErr != 0u)
                ADD_FAILURE() << label << " render tick left an unexpected GL error: "
                              << GLErrorState::GlErrorString(tickErr)
                              << " (only the benign #505 GL_INVALID_OPERATION stray is contained here; "
                                 "the whole error queue is drained regardless).";
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

    void RendererAttachedTest::SetUp()
    {
        if (!RenderPropertyFixture::IsGpuAvailable())
        {
            GTEST_SKIP() << "RendererAttachedTest: no usable GL 4.6 context available "
                            "(CI without GPU or headless server).";
        }

        if (!Renderer3D::IsInitialized())
        {
            Renderer::Init(RendererType::Renderer3D, /*loadingWindow=*/nullptr);
        }

        // Snapshot the process-wide renderer configuration before BuildScene
        // gets a chance to mutate it; TearDown restores it. See the member
        // comment in the header for why a leaked setting is worse than a
        // leaked GL binding.
        m_SavedRendererSettings = Renderer3D::GetRendererSettings();
        m_SavedPostProcessSettings = Renderer3D::GetPostProcessSettings();
        m_SettingsSnapshotted = true;

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
            Renderer3D::GetPostProcessSettings() = m_SavedPostProcessSettings;
            Renderer3D::GetRendererSettings() = m_SavedRendererSettings;
            Renderer3D::ApplyRendererSettings();
            m_SettingsSnapshotted = false;
        }

        // Stopping physics is conditional on whether the test enabled it.
        // For the minimal smoke tests we don't enable physics, so just
        // drop the Scene ref.
        m_Scene.Reset();
    }

    void RendererAttachedTest::RunFrames(u32 count, f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        for (u32 i = 0; i < count; ++i)
        {
            if (m_RenderingEnabled)
                RunGuardedRenderTick("RendererAttachedTest::RunFrames",
                                     [this, ts]()
                                     { m_Scene->OnUpdateRuntime(ts); });
            else
                // Rendering disabled: the tick issues no draws, so there is no
                // GL state to contain — skip the guard's per-frame snapshots.
                m_Scene->OnUpdateRuntime(ts);
        }
    }

    void RendererAttachedTest::RunEditorFrames(const EditorCamera& camera, u32 count, f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        for (u32 i = 0; i < count; ++i)
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
        }
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

        const u32 textureID = framebuffer->GetColorAttachmentRendererID(0);
        if (textureID == 0 || outWidth == 0 || outHeight == 0)
        {
            return false;
        }

        ReadbackRgba8(textureID, outWidth, outHeight, outRgba);
        return true;
    }
} // namespace OloEngine::Tests
