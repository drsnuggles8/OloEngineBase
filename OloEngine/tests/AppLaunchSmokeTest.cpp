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
#ifndef OLO_TEST_EDITOR_ROOT
#define OLO_TEST_EDITOR_ROOT ""
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#include <ctime>
#endif

namespace
{
    struct LaunchResult
    {
        bool Launched = false;
        bool TimedOut = false;
        int ExitCode = 0;
        std::string Error;
        std::string Output; // child's stdout+stderr, for the failure message
    };

    // Tail of the child's captured output, for a failure message. A hung
    // startup is only diagnosable if the last thing the app managed to say
    // survives — before this, a timeout reported "the app likely hung" and
    // threw the log away, which is how a 30 s hang on CI reached a human with
    // no evidence at all attached to it.
    [[nodiscard]] std::string ReadCapturedOutput(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return "<no output captured>";
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.empty())
        {
            return "<child produced no output>";
        }

        constexpr std::size_t kMaxTail = 4000;
        if (text.size() > kMaxTail)
        {
            text = "...\n" + text.substr(text.size() - kMaxTail);
        }
        return text;
    }

#if defined(_WIN32)
    LaunchResult RunProcessWithTimeout(const std::string& exePath,
                                       const std::vector<std::string>& args,
                                       const std::string& workingDir,
                                       unsigned timeoutMs)
    {
        LaunchResult result;

        // Build a single command line: quoted exe path followed by the args.
        // The args used here ("--smoke-test", "--port", "28777") contain no
        // spaces, so they're appended verbatim.
        std::string cmdLine = "\"" + exePath + "\"";
        for (const auto& a : args)
        {
            cmdLine += ' ';
            cmdLine += a;
        }
        std::vector<char> mutableCmd(cmdLine.begin(), cmdLine.end());
        mutableCmd.push_back('\0');

        // Give the child EXPLICIT standard handles rather than whatever it would
        // otherwise inherit. Two reasons, both learned from a CI hang:
        //
        //  * stdin is bound to NUL, so anything in the app that reads a line
        //    (OloServer runs a console-command thread) gets an immediate EOF
        //    instead of parking on a handle nothing will ever write to. Without
        //    this the child's stdin depends on how the TEST process was itself
        //    launched, which is why this passed on a developer box and hung on a
        //    runner.
        //  * stdout/stderr go to a file we read back on failure, so a hang has
        //    evidence attached to it.
        //
        // A file, not a pipe: nobody drains the pipe while we are blocked in
        // WaitForSingleObject, so a chatty child would fill the buffer and
        // deadlock — swapping one hang for another.
        const std::filesystem::path outPath =
            OloEngine::Tests::TempFile("applaunchsmoke.out");
        std::error_code outEc;
        std::filesystem::create_directories(outPath.parent_path(), outEc);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        const HANDLE hNul = ::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          &sa, OPEN_EXISTING, 0, nullptr);
        const HANDLE hOut = ::CreateFileA(outPath.string().c_str(), GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        const bool haveHandles = hNul != INVALID_HANDLE_VALUE && hOut != INVALID_HANDLE_VALUE;
        if (haveHandles)
        {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = hNul;
            si.hStdOutput = hOut;
            si.hStdError = hOut;
        }
        PROCESS_INFORMATION pi{};

        const BOOL ok = ::CreateProcessA(
            nullptr, // module taken from the (quoted) command line
            mutableCmd.data(),
            nullptr, nullptr,
            haveHandles ? TRUE : FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            workingDir.empty() ? nullptr : workingDir.c_str(),
            &si, &pi);

        const auto closeIfValid = [](HANDLE h)
        {
            if (h != INVALID_HANDLE_VALUE && h != nullptr)
            {
                ::CloseHandle(h);
            }
        };

        if (!ok)
        {
            result.Error = "CreateProcessA failed (GetLastError=" + std::to_string(::GetLastError()) + ")";
            closeIfValid(hNul);
            closeIfValid(hOut);
            return result;
        }
        result.Launched = true;

        const DWORD wait = ::WaitForSingleObject(pi.hProcess, timeoutMs);
        if (wait == WAIT_TIMEOUT)
        {
            result.TimedOut = true;
            ::TerminateProcess(pi.hProcess, 1);
            ::WaitForSingleObject(pi.hProcess, 5000);
        }
        else
        {
            DWORD code = 1;
            ::GetExitCodeProcess(pi.hProcess, &code);
            result.ExitCode = static_cast<int>(code);
        }

        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);

        // Close our ends before reading, so the child's last writes are flushed
        // to the file.
        closeIfValid(hNul);
        closeIfValid(hOut);
        if (haveHandles)
        {
            result.Output = ReadCapturedOutput(outPath);
        }
        return result;
    }
#else
    LaunchResult RunProcessWithTimeout(const std::string& exePath,
                                       const std::vector<std::string>& args,
                                       const std::string& workingDir,
                                       unsigned timeoutMs)
    {
        LaunchResult result;

        const pid_t pid = ::fork();
        if (pid < 0)
        {
            result.Error = "fork failed";
            return result;
        }
        if (pid == 0)
        {
            // Child: switch working directory, then exec the target binary.
            if (!workingDir.empty() && ::chdir(workingDir.c_str()) != 0)
            {
                ::_exit(127);
            }
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(exePath.c_str()));
            for (const auto& a : args)
            {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
            ::execv(exePath.c_str(), argv.data());
            ::_exit(127); // exec failed
        }

        result.Launched = true;

        constexpr unsigned stepMs = 50;
        unsigned waited = 0;
        int status = 0;
        for (;;)
        {
            const pid_t r = ::waitpid(pid, &status, WNOHANG);
            if (r == pid)
            {
                break;
            }
            if (r < 0)
            {
                result.Error = "waitpid failed";
                return result;
            }
            if (waited >= timeoutMs)
            {
                result.TimedOut = true;
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                return result;
            }
            timespec ts{ 0, static_cast<long>(stepMs) * 1000000L };
            ::nanosleep(&ts, nullptr);
            waited += stepMs;
        }

        result.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        return result;
    }
#endif

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
