// =============================================================================
// AppLaunchSmokeTest.cpp
//
// Launch smoke tests for the three shipped executables — OloServer, OloEditor,
// OloRuntime (issue #303). Each test spawns the real built binary with the
// `--smoke-test` flag, which performs full startup (runtime-DLL load, subsystem
// init, network listen for the server) and then exits cleanly with EXIT_SUCCESS
// after a few ticks. The test asserts the process exits 0 within a timeout.
//
// What this guards against: the "missing runtime DLL" regression class. FFmpeg
// (avcodec/avformat/avutil/swscale/swresample) is a *static* PE import on every
// OloEngine binary, so if a DLL isn't deployed next to the .exe the Windows
// loader fails the process before main() ever runs — a clean exit 0 proves the
// imports all resolved. The smoke flag drives the GUI apps window-less so the
// check runs without a GPU / GL 4.6 context (CI runners have none); it does not
// claim to validate the GL rendering path. See docs/agent-rules and CLAUDE.md.
//
// The binary paths are injected by CMake (OLO_TEST_OLO*_EXE); the subprocess
// working directory is the editor asset root (OLO_TEST_EDITOR_ROOT) so shaders /
// assets / Mono assemblies resolve exactly as they do for a normal run. When a
// target isn't built on this platform (or its binary is simply absent), the
// matching test SKIPs cleanly rather than failing — mirroring the renderer
// suite's "skip when the prerequisite is missing" convention.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "TestTempDir.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// CMake injects each shipped binary's on-disk path (forward-slashed). A target
// that isn't configured/built on this platform leaves its macro undefined; fall
// back to an empty string and SKIP at runtime.
#ifndef OLO_TEST_OLOSERVER_EXE
#define OLO_TEST_OLOSERVER_EXE ""
#endif
#ifndef OLO_TEST_OLOEDITOR_EXE
#define OLO_TEST_OLOEDITOR_EXE ""
#endif
#ifndef OLO_TEST_OLORUNTIME_EXE
#define OLO_TEST_OLORUNTIME_EXE ""
#endif
#ifndef OLO_TEST_OLOCTL_EXE
#define OLO_TEST_OLOCTL_EXE ""
#endif
#ifndef OLO_TEST_EDITOR_ROOT
#define OLO_TEST_EDITOR_ROOT ""
#endif

#include "TestProcessLaunch.h"

namespace
{
    using OloEngine::Tests::LaunchResult;
    using OloEngine::Tests::RunProcessWithTimeout;

    // Generous per-process cap. A real launch (DLL load + subsystem init + a few
    // ticks + shutdown) is seconds; this only fires on a genuine startup hang,
    // and stays well under the CTest per-test timeout.
    constexpr unsigned kSmokeTimeoutMs = 30000;

    void RunLaunchSmoke(const char* exePath, const std::vector<std::string>& args,
                        std::string_view monoPolicy = {}, std::string_view requiredOutput = {})
    {
        namespace fs = std::filesystem;

        if (exePath == nullptr || exePath[0] == '\0')
        {
            GTEST_SKIP() << "Target binary path not provided by the build "
                            "(app target not configured on this platform).";
        }

        std::error_code ec;
        if (!fs::exists(exePath, ec))
        {
            GTEST_SKIP() << "Binary not found at " << exePath
                         << " — build the app target to exercise this launch smoke test.";
        }

        // Run from the editor asset root so shaders / assets / Mono assemblies
        // resolve exactly as in a normal launch (see CLAUDE.md "Working directory
        // matters"). DLLs are found next to the .exe regardless of this cwd.
        const std::string workingDir = OLO_TEST_EDITOR_ROOT;

        // Startup-policy assertions need the early log lines, before the verbose
        // C# binding scan. The helper's default 4 KiB tail is for failure diagnostics.
        const LaunchResult r = RunProcessWithTimeout(exePath, args, workingDir, kSmokeTimeoutMs, 64 * 1024);

        ASSERT_TRUE(r.Launched) << "Failed to launch " << exePath << ": " << r.Error;
        ASSERT_FALSE(r.TimedOut) << exePath << " did not exit within " << kSmokeTimeoutMs
                                 << " ms — the app likely hung during startup or shutdown.\n"
                                    "The last output the child produced tells you WHICH; a log that "
                                    "stops after the subsystems come up but before the shutdown "
                                    "lines is a teardown hang, not a startup one.\n"
                                    "--- captured child output ---\n"
                                 << r.Output << "\n--- end ---";
        if (!requiredOutput.empty())
        {
            EXPECT_NE(r.Output.find(requiredOutput), std::string::npos)
                << "Expected launch path was not reached.\n"
                << r.Output;
        }
#if OLO_ENABLE_CSHARP_SCRIPTING
        if (!monoPolicy.empty())
        {
            EXPECT_NE(r.Output.find(monoPolicy), std::string::npos)
                << "Unexpected Mono debugger startup policy.\n"
                << r.Output;
        }
#else
        if (!monoPolicy.empty())
        {
            EXPECT_NE(r.Output.find("C# scripting disabled (Mono not available on this platform)"), std::string::npos)
                << "Expected the Mono-free scripting stub.\n"
                << r.Output;
        }
#endif
        EXPECT_EQ(r.ExitCode, 0) << exePath << " exited with code " << r.ExitCode
                                 << ". A non-zero exit means startup failed — most likely a missing "
                                    "runtime DLL (the regression class issue #303 targets) or a "
                                    "subsystem init crash.\n"
                                    "--- captured child output ---\n"
                                 << r.Output << "\n--- end ---";
    }
} // namespace

// OloServer is headless by design, so its --smoke-test runs the real server
// startup (incl. a network listen) end-to-end. A dedicated port avoids clashing
// with a developer's running server on the default 7777.
TEST(AppLaunchSmoke, OloServerLaunchesCleanly)
{
    RunLaunchSmoke(OLO_TEST_OLOSERVER_EXE, { "--smoke-test", "--port", "28777" }, "Mono debugging disabled");
}

TEST(AppLaunchSmoke, OloServerStopsAHeadlessSceneCleanly)
{
    const auto project = OloEngine::Tests::TempDir("server-headless-scene") / "project with spaces";
    const auto scene = project / "Assets" / "Scenes" / "Empty.olo";
    std::filesystem::create_directories(scene.parent_path());
    std::ofstream(scene) << "Scene: HeadlessShutdown\nVersion: 1\nEntities: []\n";
    // Runtime stop releases cursor capture even in an empty scene. A real
    // headless Application exists here but intentionally owns no Window.
    RunLaunchSmoke(OLO_TEST_OLOSERVER_EXE,
                   { "--smoke-test", "--port", "28777", "--project", project.string(), "--scene", scene.string() },
                   "Mono debugging disabled", "[Server] Scene loaded and started");
}

// OloEditor/OloRuntime need a real GL 4.6 context for their UI/render layers,
// which CI runners lack — so --smoke-test runs them window-less, validating that
// the binary starts and loads its DLLs. See OloEditorApp.cpp for the rationale.
TEST(AppLaunchSmoke, OloEditorLaunchesCleanly)
{
#if OLO_TEST_HOST_DEBUG
    constexpr std::string_view monoPolicy = "Mono debugging enabled";
#else
    constexpr std::string_view monoPolicy = "Mono debugging disabled";
#endif
    RunLaunchSmoke(OLO_TEST_OLOEDITOR_EXE, { "--smoke-test" }, monoPolicy);
}

TEST(AppLaunchSmoke, OloRuntimeLaunchesCleanly)
{
    RunLaunchSmoke(OLO_TEST_OLORUNTIME_EXE, { "--smoke-test" }, "Mono debugging disabled");
}

TEST(AppLaunchSmoke, OloServerAcceptsMonoDebugOptIn)
{
    RunLaunchSmoke(OLO_TEST_OLOSERVER_EXE, { "--smoke-test", "--port", "28777", "--mono-debug" }, "Mono debugging enabled");
}

TEST(AppLaunchSmoke, OloEditorAcceptsMonoDebugOptIn)
{
    RunLaunchSmoke(OLO_TEST_OLOEDITOR_EXE, { "--smoke-test", "--mono-debug" }, "Mono debugging enabled");
}

TEST(AppLaunchSmoke, OloRuntimeAcceptsMonoDebugOptIn)
{
    RunLaunchSmoke(OLO_TEST_OLORUNTIME_EXE, { "--smoke-test", "--mono-debug" }, "Mono debugging enabled");
}

// oloctl has no --smoke-test: it is not an engine app and starts no subsystems.
// `version` is its equivalent -- the one subcommand that returns without touching
// the network -- so a clean exit proves the target links and runs. This is the
// check that would have caught a new app target broken on master with CI green.
TEST(AppLaunchSmoke, OloCtlLaunchesCleanly)
{
    RunLaunchSmoke(OLO_TEST_OLOCTL_EXE, { "version" });
}
