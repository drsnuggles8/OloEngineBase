#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Rendering/PropertyTests/GLErrorStateCheck.h"
#include "Rendering/PropertyTests/TestFailureCapture.h"

#include <cstdlib>

int main(int argc, char** argv)
{
    // RenderGraphBuildDiagnostics tests rely on the registration-order-sensitivity
    // diagnostic running. Production code gates it behind OLO_RENDERGRAPH_DIAGNOSTICS;
    // force it on here before anything reads the static cache.
#if defined(_WIN32)
    _putenv_s("OLO_RENDERGRAPH_DIAGNOSTICS", "1");
#else
    setenv("OLO_RENDERGRAPH_DIAGNOSTICS", "1", 1);
#endif

    // Initialize logging explicitly
    OloEngine::Log::Initialize();
    ::testing::InitGoogleTest(&argc, argv);
    OloEngine::Tests::TestFailureCapture::RegisterFailureListener();
    // Assert a clean glGetError() state after every test so a test that
    // pollutes the shared, process-wide GL context is pinned to its source
    // rather than misattributed to a later unrelated GPU test (issue #485).
    OloEngine::Tests::GLErrorState::RegisterListener();
    const int result = ::RUN_ALL_TESTS();

    // Tests lazily initialize the renderer (e.g. through Scene rendering) but
    // do not always shut it down. Renderer2D/Renderer3D own GPU-resource-holding
    // statics (Renderer2D's s_Data, WindSystem::s_Data, the snow/precipitation
    // systems, ...). Left to static destruction at process exit, their
    // destructors free GPU buffers and call RendererMemoryTracker /
    // GPUResourceInspector / FrameResourceManager — Meyer's singletons already
    // destroyed by then — which segfaults on the way out. Mirror the production
    // app shutdown and release these now, while those singletons are still alive.
    OloEngine::Renderer::Shutdown();

    return result;
}
