#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include "Platform/OpenGL/OpenGLUtilities.h"

namespace OloEngine::Tests
{
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
            {
                // Contain global GL state the full pipeline leaves behind
                // (blend / stencil / viewport / active program / VAO / FBO)
                // so a render in this fixture cannot poison the fixed-function
                // state of the next GPU test in the same process. Per-slot
                // texture / UBO bindings are intentionally NOT restored by
                // GLStateGuard (see its class comment) — they are benign here
                // because downstream GPU tests bind their own resources before
                // drawing.
                {
                    GLStateGuard guard("RendererAttachedTest::RunFrames", GLStateGuard::Policy::Restore);
                    m_Scene->OnUpdateRuntime(ts);
                }
                // Drain the GL error queue this render tick produced. The full
                // pipeline is known to emit benign stray GL errors (e.g.
                // GL_INVALID_OPERATION binding a texture whose handle went stale
                // across cross-test render-graph/asset churn — correct pixels,
                // dirty error queue; tracked separately). Draining here extends
                // the GLStateGuard "a render leaves no global GL state behind"
                // contract to the error queue — the same containment the #485
                // production fix applied in the readback helpers (69aa9357) — so
                // a render in this fixture cannot poison the shared context of a
                // later GPU test. The process-wide #485 listener still guards
                // every non-render tick and any error a test body leaks outside a
                // RunFrames call.
                Utils::DrainGLErrors();
            }
            else
            {
                // Rendering disabled: the tick issues no draws, so there is no
                // GL state to contain — skip the guard's per-frame snapshots.
                m_Scene->OnUpdateRuntime(ts);
            }
        }
    }

    void RendererAttachedTest::RunEditorFrames(const EditorCamera& camera, u32 count, f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        for (u32 i = 0; i < count; ++i)
        {
            if (m_RenderingEnabled)
            {
                // Same GL-state hygiene as RunFrames: the editor render path
                // (OnUpdateEditor -> RenderScene3D -> EndScene) drives the full
                // render graph and leaves the same global fixed-function /
                // binding state behind. Containing it here is what lets an
                // editor-camera visual test (e.g. WaterVisualEvidenceTest) run
                // in the normal suite without poisoning the GPU tests that
                // follow it in the same process.
                {
                    GLStateGuard guard("RendererAttachedTest::RunEditorFrames", GLStateGuard::Policy::Restore);
                    m_Scene->OnUpdateEditor(ts, camera);
                }
                // Same error-queue containment as RunFrames — see the note there
                // (issue #485).
                Utils::DrainGLErrors();
            }
            else
            {
                m_Scene->OnUpdateEditor(ts, camera);
            }
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
