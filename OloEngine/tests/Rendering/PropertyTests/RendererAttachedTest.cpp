#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Renderer.h"

namespace OloEngine::Tests
{
    namespace
    {
        // Track whether Renderer::Init has been called this process so
        // that running multiple RendererAttachedTest-derived suites
        // in one binary doesn't double-init.
        bool s_RendererInitialised = false;
    } // namespace

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
        // See SetUpTestSuite. Renderer::Shutdown happens at process exit;
        // the GL context (RenderPropertyFixture) follows the same lifetime.
    }

    void RendererAttachedTest::SetUp()
    {
        if (!RenderPropertyFixture::IsGpuAvailable())
        {
            GTEST_SKIP() << "RendererAttachedTest: no usable GL 4.6 context available "
                            "(CI without GPU or headless server).";
        }

        if (!s_RendererInitialised)
        {
            Renderer::Init(RendererType::Renderer3D, /*loadingWindow=*/nullptr);
            s_RendererInitialised = true;
        }

        m_Scene = Scene::Create();
        // *** Current limitation ***
        // Enabling rendering on the Scene (`SetRenderingEnabled(true)`)
        // currently SEH-crashes deep inside `Scene::OnUpdateRuntime`, even
        // with no entities. The crash bisects to "Renderer::Init has been
        // called + m_RenderingEnabled == true" — Renderer3D leaves some
        // subsystem state that one of the unconditional render-path
        // helpers in OnUpdateRuntime dereferences without a guard. Needs
        // a debugger session to localise; until then the fixture exercises
        // the *foundation* (renderer comes up, OnUpdateRuntime ticks
        // cross-subsystem logic post-init) but not the actual draw path.
        //
        // Tests that need to verify rendering side-effects should continue
        // to use `GoldenImageTests`-style direct `Renderer3D::Submit*`
        // calls. The renderer-attached scene-tick path is documented as a
        // follow-up; once unblocked, flip this to `SetRenderingEnabled(true)`
        // and re-enable the rendering-conditional smoke tests below.
        m_Scene->SetRenderingEnabled(false);

        BuildScene();
    }

    void RendererAttachedTest::TearDown()
    {
        // Stopping physics is conditional on whether the test enabled it.
        // For the minimal smoke tests we don't enable physics, so just
        // drop the Scene ref.
        m_Scene.Reset();
    }

    void RendererAttachedTest::RunFrames(u32 count, f32 dtSeconds)
    {
        const Timestep ts{ dtSeconds };
        for (u32 i = 0; i < count; ++i)
            m_Scene->OnUpdateRuntime(ts);
    }

    void RendererAttachedTest::SetViewport(u32 width, u32 height)
    {
        m_Scene->OnViewportResize(width, height);
    }
} // namespace OloEngine::Tests
