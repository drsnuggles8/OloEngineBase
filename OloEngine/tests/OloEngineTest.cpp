#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Interactivity.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Renderer.h"
#include "Rendering/PropertyTests/GLErrorStateCheck.h"
#include "Rendering/PropertyTests/RendererStateCheck.h"
#include "Rendering/PropertyTests/TestFailureCapture.h"
#include "Rendering/VulkanCoverageReport.h"
#include "MemoryCeiling.h"
#include "TestOptions.h"
#include "TestTempDir.h"
#include "TestXmlOutputPath.h"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    [[nodiscard]] int CurrentProcessId()
    {
#ifdef _WIN32
        return ::_getpid();
#else
        return static_cast<int>(::getpid());
#endif
    }

    // The gtest filter as it will be AFTER InitGoogleTest, read early because the
    // output filename has to be fixed before that call. A `--gtest_filter=` on the
    // command line wins over the GTEST_FILTER environment variable that gtest has
    // already folded into the flag, which is gtest's own precedence.
    [[nodiscard]] std::string EffectiveFilter(int argc, char** argv)
    {
        constexpr std::string_view kPrefix = "--gtest_filter=";
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg = argv[i];
            if (arg.starts_with(kPrefix))
                return std::string(arg.substr(kPrefix.size()));
        }
        return GTEST_FLAG_GET(filter);
    }

    // Index of the last `--gtest_output=` in argv, or -1. Last, because that is
    // the one gtest keeps when a flag is repeated.
    [[nodiscard]] int LastOutputArgIndex(int argc, char** argv)
    {
        constexpr std::string_view kPrefix = "--gtest_output=";
        int found = -1;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string_view(argv[i]).starts_with(kPrefix))
                found = i;
        }
        return found;
    }

    // Issue #1372. Give this process its own report filename when the requested
    // path is a DIRECTORY, because gtest would otherwise choose one by probing
    // the filesystem — a check-then-create race that makes two parallel
    // processes write the same file. See TestXmlOutputPath.h.
    //
    // MUST run before InitGoogleTest: that is where gtest resolves the path and
    // constructs the XML listener, and nothing afterwards can change it.
    //
    // The correction is written back to WHICHEVER source gtest will actually
    // read. Setting the flag alone would be silently undone by a
    // `--gtest_output=` on the command line, since InitGoogleTest parses argv
    // after this and the command line wins — so when the directory came from
    // argv, the argv slot is what gets rewritten. No caller in this repository
    // passes a directory on the command line today; this is here so that one
    // doing so later does not quietly reopen the race.
    void MakeGTestOutputUniqueToThisProcess(int argc, char** argv)
    {
        constexpr std::string_view kPrefix = "--gtest_output=";
        const int argIndex = LastOutputArgIndex(argc, argv);
        const std::string requested = (argIndex >= 0)
                                          ? std::string(std::string_view(argv[argIndex]).substr(kPrefix.size()))
                                          : GTEST_FLAG_GET(output);

        const char* const shardIndex = std::getenv("GTEST_SHARD_INDEX");
        const std::string unique = OloEngine::Tests::UniqueGTestOutputSpec(
            requested, EffectiveFilter(argc, argv), shardIndex != nullptr ? shardIndex : "", CurrentProcessId());
        if (unique.empty())
            return; // Not a directory, or nothing requested: leave it exactly as it was.

        if (argIndex >= 0)
        {
            // Static so the buffer outlives InitGoogleTest's read of argv.
            static std::string rewrittenArg;
            rewrittenArg = std::string(kPrefix) + unique;
            argv[argIndex] = rewrittenArg.data();
        }
        else
        {
            GTEST_FLAG_SET(output, unique);
        }
    }
} // namespace

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

    // Per-process resident-set ceiling (MemoryCeiling.h, --olo-rss-ceiling-mb).
    // A case that outgrows the runner's cgroup is otherwise killed by the
    // kernel with no test name and, on the self-hosted box, sometimes with the
    // runner. Past the ceiling the process stops itself with exit code 77 and
    // says which test was running.
    OloEngine::Tests::StartMemoryCeilingWatchdog(OloEngine::Tests::Options().RssCeilingMb);

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

    // Issue #1372: one report file per process, before gtest resolves the name.
    MakeGTestOutputUniqueToThisProcess(argc, argv);

    ::testing::InitGoogleTest(&argc, argv);
    OloEngine::Tests::TestFailureCapture::RegisterFailureListener();
    OloEngine::Tests::RegisterMemoryCeilingListener();
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
    // Say, at the END of the run and below gtest's own summary, whether this
    // run exercised the Vulkan backend or only skipped it (issue #1300).
    // The banner lands BELOW gtest's own summary because gtest prints that
    // from OnTestIterationEnd, which always precedes every listener's
    // OnTestProgramEnd — a skip reported three hundred lines above a
    // `[  PASSED  ]` is a skip nobody reads.
    OloEngine::Tests::VulkanCoverage::RegisterListener();
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
        OloEngine::Tests::StopMemoryCeilingWatchdog();
        OloEngine::Renderer::Shutdown();
        return 2;
    }

    // `--olo-require-vulkan` (issue #1300), the twin of `--olo-require-gpu`.
    // The gate itself FAILs the first refused test, which covers the ordinary
    // "no device" case. This second check covers the one it cannot see: a run
    // in which every device-gated test skipped for a reason PAST the gate, or
    // in which the active filter selected none of them at all. Either way the
    // flag's promise — "this run verified the Vulkan backend" — was not kept,
    // and an exit code is the only part of that a script reads.
    if (OloEngine::Tests::VulkanCoverage::Required() && OloEngine::Tests::VulkanCoverage::ExecutedCount() == 0)
    {
        std::fprintf(stderr,
                     "OloEngine-Tests: --olo-require-vulkan was given but no device-gated Vulkan test "
                     "executed. This run verified nothing about the Vulkan backend.\n");
        OloEngine::Tests::StopMemoryCeilingWatchdog();
        OloEngine::Renderer::Shutdown();
        // A real test failure outranks this. A script that special-cases 3
        // would otherwise be told "Vulkan was not exercised" and never learn
        // that the run ALSO had failing tests.
        return result != 0 ? result : 3;
    }

    // Tests lazily initialize the renderer (e.g. through Scene rendering) but
    // do not always shut it down. Renderer2D/Renderer3D own GPU-resource-holding
    // statics (Renderer2D's s_Data, WindSystem::s_Data, the snow/precipitation
    // systems, ...). Left to static destruction at process exit, their
    // destructors free GPU buffers and call RendererMemoryTracker /
    // GPUResourceInspector / FrameResourceManager — Meyer's singletons already
    // destroyed by then — which segfaults on the way out. Mirror the production
    // app shutdown and release these now, while those singletons are still alive.
    // Joined before the statics it reads are destroyed (CodeRabbit on #1204).
    OloEngine::Tests::StopMemoryCeilingWatchdog();
    OloEngine::Renderer::Shutdown();

    return result;
}
