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

    void RunLaunchSmoke(const char* exePath, const std::vector<std::string>& args)
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

        const LaunchResult r = RunProcessWithTimeout(exePath, args, workingDir, kSmokeTimeoutMs);

        ASSERT_TRUE(r.Launched) << "Failed to launch " << exePath << ": " << r.Error;
        ASSERT_FALSE(r.TimedOut) << exePath << " did not exit within " << kSmokeTimeoutMs
                                 << " ms — the app likely hung during startup or shutdown.\n"
                                    "The last output the child produced tells you WHICH; a log that "
                                    "stops after the subsystems come up but before the shutdown "
                                    "lines is a teardown hang, not a startup one.\n"
                                    "--- captured child output ---\n"
                                 << r.Output << "\n--- end ---";
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
    RunLaunchSmoke(OLO_TEST_OLOSERVER_EXE, { "--smoke-test", "--port", "28777" });
}

// OloEditor/OloRuntime need a real GL 4.6 context for their UI/render layers,
// which CI runners lack — so --smoke-test runs them window-less, validating that
// the binary starts and loads its DLLs. See OloEditorApp.cpp for the rationale.
TEST(AppLaunchSmoke, OloEditorLaunchesCleanly)
{
    RunLaunchSmoke(OLO_TEST_OLOEDITOR_EXE, { "--smoke-test" });
}

TEST(AppLaunchSmoke, OloRuntimeLaunchesCleanly)
{
    RunLaunchSmoke(OLO_TEST_OLORUNTIME_EXE, { "--smoke-test" });
}

// oloctl has no --smoke-test: it is not an engine app and starts no subsystems.
// `version` is its equivalent -- the one subcommand that returns without touching
// the network -- so a clean exit proves the target links and runs. This is the
// check that would have caught a new app target broken on master with CI green.
TEST(AppLaunchSmoke, OloCtlLaunchesCleanly)
{
    RunLaunchSmoke(OLO_TEST_OLOCTL_EXE, { "version" });
}
