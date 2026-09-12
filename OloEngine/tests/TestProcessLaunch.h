#pragma once

// Launch a child process with a timeout and read back everything it printed.
//
// Shared by the tests that can only be honest from OUTSIDE the process: the
// app launch smoke tests (a shipped binary must start and exit 0 with every
// runtime DLL resolved) and the memory-ceiling test (the watchdog stops a
// runaway OloEngine-Tests process, and only a second process can observe that
// without being the thing that stopped).
//
// The child gets explicit standard handles: stdin from the null device, so a
// console-reading thread sees EOF instead of parking forever, and stdout +
// stderr into a per-test file that is read back after exit. A FILE rather than
// a pipe: nobody drains a pipe while the parent is blocked waiting, so a chatty
// child would fill the buffer and hang -- swapping one hang for another. The
// tail of that file is what a timeout or a bad exit code reports, so a hung or
// killed child reaches a human with evidence attached.

#include "TestTempDir.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace OloEngine::Tests
{
    struct LaunchResult
    {
        bool Launched = false;
        bool TimedOut = false;
        int ExitCode = 0;
        std::string Error;
        std::string Output; // child's stdout+stderr (tail), for the failure message
    };

    // Tail of the child's captured output, for a failure message. A hung
    // startup is only diagnosable if the last thing the app managed to say
    // survives — before this, a timeout reported "the app likely hung" and
    // threw the log away, which is how a 30 s hang on CI reached a human with
    // no evidence at all attached to it.
    [[nodiscard]] inline std::string ReadCapturedOutput(const std::filesystem::path& path)
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
    inline LaunchResult RunProcessWithTimeout(const std::string& exePath, const std::vector<std::string>& args,
                                              const std::string& workingDir, unsigned timeoutMs)
    {
        LaunchResult result;

        // Build a single command line: quoted exe path followed by the args.
        // The args used by the callers ("--smoke-test", "--port", "28777",
        // "--gtest_filter=Suite.Case") contain no spaces, so they're appended
        // verbatim.
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
        const std::filesystem::path outPath = OloEngine::Tests::TempFile("processlaunch.out");
        std::error_code outEc;
        std::filesystem::create_directories(outPath.parent_path(), outEc);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        const HANDLE hNul =
            ::CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        const HANDLE hOut = ::CreateFileA(outPath.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

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

        const BOOL ok = ::CreateProcessA(nullptr, // module taken from the (quoted) command line
                                         mutableCmd.data(), nullptr, nullptr, haveHandles ? TRUE : FALSE,
                                         CREATE_NO_WINDOW, nullptr, workingDir.empty() ? nullptr : workingDir.c_str(),
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
    inline LaunchResult RunProcessWithTimeout(const std::string& exePath, const std::vector<std::string>& args,
                                              const std::string& workingDir, unsigned timeoutMs)
    {
        LaunchResult result;

        // Same contract as the Windows arm: the child's stdout+stderr land in a
        // file the parent reads back, and its stdin is the null device.
        const std::filesystem::path outPath = OloEngine::Tests::TempFile("processlaunch.out");
        std::error_code outEc;
        std::filesystem::create_directories(outPath.parent_path(), outEc);
        const std::string outPathStr = outPath.string();

        const pid_t pid = ::fork();
        if (pid < 0)
        {
            result.Error = "fork failed";
            return result;
        }
        if (pid == 0)
        {
            // Child: redirect the standard streams, switch working directory,
            // then exec the target binary. Only async-signal-safe calls here.
            const int nul = ::open("/dev/null", O_RDONLY);
            const int out = ::open(outPathStr.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (nul >= 0)
            {
                ::dup2(nul, STDIN_FILENO);
            }
            if (out >= 0)
            {
                ::dup2(out, STDOUT_FILENO);
                ::dup2(out, STDERR_FILENO);
            }
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
                result.Output = ReadCapturedOutput(outPath);
                return result;
            }
            timespec ts{ 0, static_cast<long>(stepMs) * 1000000L };
            ::nanosleep(&ts, nullptr);
            waited += stepMs;
        }

        // A signal death (the OOM killer, a sanitizer abort) is reported as
        // 128 + signal, the shell convention, so a caller can tell it from a
        // deliberate exit code.
        if (WIFEXITED(status))
        {
            result.ExitCode = WEXITSTATUS(status);
        }
        else if (WIFSIGNALED(status))
        {
            result.ExitCode = 128 + WTERMSIG(status);
        }
        else
        {
            result.ExitCode = 1;
        }
        result.Output = ReadCapturedOutput(outPath);
        return result;
    }
#endif
} // namespace OloEngine::Tests
