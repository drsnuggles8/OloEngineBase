#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/Log.h"
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
    return ::RUN_ALL_TESTS();
}
