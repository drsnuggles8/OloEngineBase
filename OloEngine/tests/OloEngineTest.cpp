#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Interactivity.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Rendering/PropertyTests/GLErrorStateCheck.h"
#include "Rendering/PropertyTests/RendererStateCheck.h"
#include "Rendering/PropertyTests/TestFailureCapture.h"
#include "TestOptions.h"
#include "TestTempDir.h"

int main(int argc, char** argv)
{
    // Consume the `--olo-*` flags before gtest's parser sees them, so an
    // unknown one is OUR diagnostic rather than gtest silently leaving it as a
    // positional argument.
    OloEngine::Tests::ParseTestOptions(argc, argv);

    // RenderGraphBuildDiagnostics tests rely on the registration-order-sensitivity
    // diagnostic running. It is otherwise seeded from OLO_RENDERGRAPH_DIAGNOSTICS;
    // say so directly instead of writing the environment and hoping nothing has
    // read it yet.
    OloEngine::Levers::SetRenderGraphDiagnostics(true);

    // Initialize logging explicitly. The suite gets its own file: run from
    // OloEditor/ (which the visual tests require) it shared OloEngine.log with
    // a live editor and, opening truncating, erased that editor's diagnostics
    // mid-session — so a shader error under investigation became the suite's
    // shutdown noise.
    OloEngine::Log::SetLogFile("OloEngine-Tests.log");
    OloEngine::Log::Initialize();

    // Most headless tests never construct an Application, so the startup line
    // it normally prints would never appear here. A suite run with a lever set
    // — a bisection switch left exported in the shell — must say so, or the
    // resulting pass/fail is being read out of context. Silent when everything
    // is at its default. Also flushes any malformed-value warning the lazy seed
    // above deferred, now that the logger exists.
    OloEngine::Levers::LogActive();

    // No one is here to click OK. Without this, ANY blocking modal in a test run
    // parks the process forever at ~0% CPU — it presents as a hung/slow test,
    // not a failing one. Cost hours on #714 when a compute shader failed to
    // compile and the assert dialog waited for a click that never came.
    //
    // This is the process-wide answer, not the assert-specific one: it also
    // covers the auto-save recovery and unsaved-changes prompts, and any modal
    // added later that asks IsNonInteractive() as it should. Everything still
    // logs; only the blocking is removed.
    OloEngine::SetNonInteractive(true);

    // `--olo-capture-manifest=` is a TOOL RUN (issue #974): one flag should
    // give a pure capture invocation, not the whole suite plus a capture.
    // Setting the flag default BEFORE InitGoogleTest keeps an explicit
    // `--gtest_filter=` from the command line authoritative — gtest's own
    // parser overwrites this default when the user passed one. A GTEST_FILTER
    // environment variable is indistinguishable from the built-in default by
    // this point (gtest folds it into the flag at static init), so it is
    // respected explicitly rather than silently clobbered.
    const bool captureToolRun = !OloEngine::Tests::Options().CaptureManifestPath.empty();
    if (captureToolRun && std::getenv("GTEST_FILTER") == nullptr)
    {
        GTEST_FLAG_SET(filter, "BenchmarkCapture.*");
    }

    ::testing::InitGoogleTest(&argc, argv);
    OloEngine::Tests::TestFailureCapture::RegisterFailureListener();
    // Assert a clean glGetError() state after every test so a test that
    // pollutes the shared, process-wide GL context is pinned to its source
    // rather than misattributed to a later unrelated GPU test (issue #485).
    OloEngine::Tests::GLErrorState::RegisterListener();
    // Restore the process-global renderer CONFIGURATION after every test, and
    // account for every test that left it changed (issue #1074). GL state and
    // renderer configuration are different hazards: the guard above catches a
    // dirty `glGetError()` queue, this one catches a rendering path or settings
    // struct left switched to something the next test never asked for — which
    // has no GL-level symptom at all and instead makes a later visual-evidence
    // test quietly measure the wrong pipeline.
    OloEngine::Tests::RendererState::RegisterListener();
    // Give every test a freshly-emptied scratch directory on its first
    // TempDir()/TempFile() call — the clean slate the per-fixture `SetUp`
    // remove_all blocks used to provide, and which `--gtest_repeat` (same case,
    // same process, same path) would otherwise silently take away. See
    // docs/agent-rules/shared-temp-dir-test-isolation.md.
    OloEngine::Tests::RegisterCleanSlateListener();
    const int result = ::RUN_ALL_TESTS();

    // The capture-mode filter above names a test suite by string; a suite
    // rename would silently turn every capture invocation into a 0-test run
    // that exits 0 having produced nothing. gtest only applies the filter
    // inside RUN_ALL_TESTS, so the count is checked after it.
    if (captureToolRun && ::testing::UnitTest::GetInstance()->test_to_run_count() == 0)
    {
        std::fprintf(stderr,
                     "OloEngine-Tests: --olo-capture-manifest was given but the active gtest filter "
                     "matched no tests (expected the BenchmarkCapture suite).\n");
        OloEngine::Renderer::Shutdown();
        return 2;
    }

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
