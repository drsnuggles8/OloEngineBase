// Linux implementation of CrashReporterPlatform.
//
// Mirrors the Windows implementation's contract: install fatal-signal handlers
// that funnel into the generic CrashReporter::WriteCrashReport callback, write
// a plain-text "minidump" (Linux has no native minidump format without Breakpad,
// so we capture register state + backtrace into a .dmp text file), and surface
// system info from /proc and /etc/os-release.
//
// Async-signal-safety note: like the Windows handler, the callback path runs
// std::ofstream / fmt::format / OLO_CORE_FATAL inside the signal handler. That
// is not strictly async-signal-safe, but the process is already terminating —
// the alternative (a separate writer process, breakpad-style) is overkill for
// this engine's needs. SA_RESETHAND + an std::atomic_flag re-entry guard keep
// a faulting handler from looping; sigaltstack keeps the handler reachable
// after a stack-overflow.

#include "OloEnginePCH.h"
#include "OloEngine/Debug/CrashReporterPlatform.h"

#ifdef OLO_PLATFORM_LINUX

#include "OloEngine/Core/Log.h"

#include <atomic>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <cxxabi.h>
#include <execinfo.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

namespace OloEngine::CrashReporterPlatform
{
    namespace
    {
        // Signals handled. SIGTRAP is intentionally omitted — debugger breakpoints
        // shouldn't trip the crash reporter.
        constexpr std::array<int, 5> kFatalSignals = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };

        // Stash the previous sigaction for each signal so we can restore on Uninstall.
        // sized to hold every handled signal + a small headroom.
        struct PreviousAction
        {
            int Signal = 0;
            struct sigaction Action{};
            bool Installed = false;
        };
        std::array<PreviousAction, kFatalSignals.size()> s_PreviousActions{};

        std::terminate_handler s_PreviousTerminateHandler = nullptr;

        // Callback installed by InstallHandlers. Atomically read inside the signal
        // handler — pointer reads are atomic on x86_64/aarch64, so this is safe
        // from an async-signal-safety standpoint.
        std::atomic<WriteCrashReportFn> s_WriteCrashReport{ nullptr };

        // Re-entry guard: if the WriteCrashReport callback itself crashes, the
        // second entry through this guard skips back to the OS default handler
        // instead of looping forever. C++20 default-initializes atomic_flag to
        // clear, so no explicit ATOMIC_FLAG_INIT (deprecated in C++20).
        std::atomic_flag s_HandlerEntered{};

        // Alt-stack so the handler can run even on stack overflow. 64 KiB is
        // overkill for our handler (which doesn't recurse) but matches what
        // glibc tends to use for SIGSTKSZ on recent kernels.
        constexpr size_t kAltStackSize = 1u << 16; // 64 KiB
        alignas(16) std::array<u8, kAltStackSize> s_AltStack{};
        bool s_AltStackInstalled = false;

        // Bundle siginfo_t* + ucontext_t* into one opaque platformContext blob
        // so WriteMiniDump can access both. Lives on the signal-handler stack;
        // never escapes the WriteCrashReport call.
        struct LinuxSignalContext
        {
            siginfo_t* Info = nullptr;
            ucontext_t* Context = nullptr;
        };

        const char* SignalToString(int sig)
        {
            switch (sig)
            {
                case SIGSEGV:
                    return "SIGSEGV";
                case SIGABRT:
                    return "SIGABRT";
                case SIGFPE:
                    return "SIGFPE";
                case SIGILL:
                    return "SIGILL";
                case SIGBUS:
                    return "SIGBUS";
                case SIGTRAP:
                    return "SIGTRAP";
                default:
                    return "UNKNOWN_SIGNAL";
            }
        }

        const char* SigcodeToString(int sig, int code)
        {
            // Common cross-signal codes first
            switch (code)
            {
                case SI_USER:
                    return "SI_USER (kill/raise)";
                case SI_KERNEL:
                    return "SI_KERNEL";
                case SI_QUEUE:
                    return "SI_QUEUE (sigqueue)";
                case SI_TIMER:
                    return "SI_TIMER";
                case SI_TKILL:
                    return "SI_TKILL";
                default:
                    break;
            }

            switch (sig)
            {
                case SIGSEGV:
                    switch (code)
                    {
                        case SEGV_MAPERR:
                            return "SEGV_MAPERR (address not mapped)";
                        case SEGV_ACCERR:
                            return "SEGV_ACCERR (invalid permissions)";
                        default:
                            return "UNKNOWN_SEGV_CODE";
                    }
                case SIGBUS:
                    switch (code)
                    {
                        case BUS_ADRALN:
                            return "BUS_ADRALN (invalid alignment)";
                        case BUS_ADRERR:
                            return "BUS_ADRERR (non-existent address)";
                        case BUS_OBJERR:
                            return "BUS_OBJERR (object-specific HW error)";
                        default:
                            return "UNKNOWN_BUS_CODE";
                    }
                case SIGFPE:
                    switch (code)
                    {
                        case FPE_INTDIV:
                            return "FPE_INTDIV (integer divide by zero)";
                        case FPE_INTOVF:
                            return "FPE_INTOVF (integer overflow)";
                        case FPE_FLTDIV:
                            return "FPE_FLTDIV (floating-point divide by zero)";
                        case FPE_FLTOVF:
                            return "FPE_FLTOVF (floating-point overflow)";
                        case FPE_FLTUND:
                            return "FPE_FLTUND (floating-point underflow)";
                        case FPE_FLTRES:
                            return "FPE_FLTRES (floating-point inexact)";
                        case FPE_FLTINV:
                            return "FPE_FLTINV (invalid floating-point operation)";
                        case FPE_FLTSUB:
                            return "FPE_FLTSUB (subscript out of range)";
                        default:
                            return "UNKNOWN_FPE_CODE";
                    }
                case SIGILL:
                    switch (code)
                    {
                        case ILL_ILLOPC:
                            return "ILL_ILLOPC (illegal opcode)";
                        case ILL_ILLOPN:
                            return "ILL_ILLOPN (illegal operand)";
                        case ILL_ILLADR:
                            return "ILL_ILLADR (illegal addressing mode)";
                        case ILL_ILLTRP:
                            return "ILL_ILLTRP (illegal trap)";
                        case ILL_PRVOPC:
                            return "ILL_PRVOPC (privileged opcode)";
                        case ILL_PRVREG:
                            return "ILL_PRVREG (privileged register)";
                        case ILL_COPROC:
                            return "ILL_COPROC (coprocessor error)";
                        case ILL_BADSTK:
                            return "ILL_BADSTK (internal stack error)";
                        default:
                            return "UNKNOWN_ILL_CODE";
                    }
                default:
                    return "UNKNOWN_CODE";
            }
        }

        void SignalHandler(int sig, siginfo_t* info, void* ctxRaw)
        {
            // Re-entry guard: a fault inside the callback would otherwise loop
            // back here forever. test_and_set returns the previous value, so the
            // first entry sees false and proceeds; subsequent entries bail to
            // the OS default action via raise().
            if (s_HandlerEntered.test_and_set(std::memory_order_acq_rel))
            {
                // Already handling a crash — restore default handler and re-raise
                // so the OS produces a core dump (or terminates cleanly).
                struct sigaction dfl{};
                dfl.sa_handler = SIG_DFL;
                ::sigemptyset(&dfl.sa_mask);
                ::sigaction(sig, &dfl, nullptr);
                ::raise(sig);
                _exit(128 + sig);
            }

            auto* ctx = static_cast<ucontext_t*>(ctxRaw);

            // Format a human-readable detail string. fmt::format inside a signal
            // handler isn't async-signal-safe but matches the Windows path and
            // is acceptable for a terminal process state.
            std::string detail;
            if (info != nullptr)
            {
                if (sig == SIGSEGV || sig == SIGBUS)
                {
                    detail = fmt::format("{} at address 0x{:016X} ({})",
                                         SignalToString(sig),
                                         reinterpret_cast<uintptr_t>(info->si_addr),
                                         SigcodeToString(sig, info->si_code));
                }
                else
                {
                    detail = fmt::format("{} (code {} — {})",
                                         SignalToString(sig),
                                         info->si_code,
                                         SigcodeToString(sig, info->si_code));
                }
            }
            else
            {
                detail = SignalToString(sig);
            }

            LinuxSignalContext linuxCtx{ info, ctx };

            if (auto Cb = s_WriteCrashReport.load(std::memory_order_acquire))
            {
                Cb("Linux Fatal Signal", detail, &linuxCtx);
            }

            // Restore default handler and re-raise so the OS produces a core dump
            // (if ulimit allows) and the parent shell sees the expected exit code.
            struct sigaction dfl{};
            dfl.sa_handler = SIG_DFL;
            ::sigemptyset(&dfl.sa_mask);
            ::sigaction(sig, &dfl, nullptr);
            ::raise(sig);
            _exit(128 + sig);
        }

        [[noreturn]] void TerminateHandler()
        {
            std::string detail = "std::terminate() called";

            // Mirror the Windows path: try to extract info from a current exception.
            if (auto eptr = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(eptr);
                }
                catch (const std::exception& e)
                {
                    detail = fmt::format("std::terminate() with exception: {}", e.what());
                }
                catch (...)
                {
                    detail = "std::terminate() with unknown exception type";
                }
            }

            if (auto Cb = s_WriteCrashReport.load(std::memory_order_acquire))
            {
                Cb("std::terminate", detail, nullptr);
            }

            if (s_PreviousTerminateHandler)
            {
                s_PreviousTerminateHandler();
            }
            std::abort();
        }

        // Read an entire small file (e.g. /etc/os-release, /proc/meminfo) into a
        // string. Returns empty string on failure — caller decides how to render
        // a missing file.
        std::string SlurpFile(const char* path)
        {
            std::ifstream f(path);
            if (!f.is_open())
            {
                return {};
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }

        // Parse /etc/os-release-style `KEY="value"` lines. Returns "" if missing.
        std::string ExtractOsReleaseField(const std::string& osRelease, const std::string& key)
        {
            const std::string needle = key + "=";
            sizet pos = osRelease.find(needle);
            // Walk back to a newline (or start of file) to ensure this is a
            // top-level key, not a substring of another value.
            while (pos != std::string::npos)
            {
                if (pos == 0 || osRelease[pos - 1] == '\n')
                {
                    break;
                }
                pos = osRelease.find(needle, pos + 1);
            }
            if (pos == std::string::npos)
            {
                return {};
            }
            sizet start = pos + needle.size();
            sizet end = osRelease.find('\n', start);
            if (end == std::string::npos)
            {
                end = osRelease.size();
            }
            std::string value = osRelease.substr(start, end - start);
            // Strip surrounding double quotes if present.
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }

        // Look up a single field in /proc/cpuinfo's first processor block. Returns
        // empty string if the key is absent.
        std::string FirstCpuinfoField(const std::string& cpuinfo, const std::string& key)
        {
            const std::string needle = key;
            sizet pos = cpuinfo.find(needle);
            while (pos != std::string::npos)
            {
                if (pos == 0 || cpuinfo[pos - 1] == '\n')
                {
                    break;
                }
                pos = cpuinfo.find(needle, pos + 1);
            }
            if (pos == std::string::npos)
            {
                return {};
            }
            sizet colon = cpuinfo.find(':', pos);
            if (colon == std::string::npos)
            {
                return {};
            }
            sizet end = cpuinfo.find('\n', colon);
            if (end == std::string::npos)
            {
                end = cpuinfo.size();
            }
            std::string value = cpuinfo.substr(colon + 1, end - colon - 1);
            sizet first = value.find_first_not_of(" \t");
            if (first == std::string::npos)
            {
                return {};
            }
            return value.substr(first);
        }

        // Try to demangle a backtrace_symbols entry of the form
        // `module(_ZN... +0xoffset) [0xaddr]`. Returns the raw line if we can't
        // parse / demangle.
        std::string DemangleBacktraceFrame(const char* raw)
        {
            if (raw == nullptr || *raw == '\0')
            {
                return "<null>";
            }

            std::string s(raw);
            sizet open = s.find('(');
            sizet plus = s.find('+', open == std::string::npos ? 0 : open);
            sizet close = s.find(')', plus == std::string::npos ? 0 : plus);
            if (open == std::string::npos || plus == std::string::npos ||
                close == std::string::npos || plus <= open + 1)
            {
                return s;
            }

            std::string module = s.substr(0, open);
            std::string mangled = s.substr(open + 1, plus - open - 1);
            std::string offsetAndAddr = s.substr(plus, s.size() - plus);
            if (mangled.empty())
            {
                return s;
            }

            int status = 0;
            char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
            std::string result;
            if (status == 0 && demangled != nullptr)
            {
                result = module + "(" + demangled + offsetAndAddr;
            }
            else
            {
                result = s;
            }
            if (demangled != nullptr)
            {
                ::free(demangled); // abi::__cxa_demangle requires free, not delete
            }
            return result;
        }

        void WriteRegisterDump(std::ostream& out, ucontext_t* ctx)
        {
            if (ctx == nullptr)
            {
                out << "(no register context)\n";
                return;
            }

#if defined(__x86_64__)
            const auto& gregs = ctx->uc_mcontext.gregs;
            out << fmt::format("RIP: 0x{:016X}    RSP: 0x{:016X}    RBP: 0x{:016X}\n",
                               static_cast<u64>(gregs[REG_RIP]),
                               static_cast<u64>(gregs[REG_RSP]),
                               static_cast<u64>(gregs[REG_RBP]));
            out << fmt::format("RAX: 0x{:016X}    RBX: 0x{:016X}    RCX: 0x{:016X}\n",
                               static_cast<u64>(gregs[REG_RAX]),
                               static_cast<u64>(gregs[REG_RBX]),
                               static_cast<u64>(gregs[REG_RCX]));
            out << fmt::format("RDX: 0x{:016X}    RSI: 0x{:016X}    RDI: 0x{:016X}\n",
                               static_cast<u64>(gregs[REG_RDX]),
                               static_cast<u64>(gregs[REG_RSI]),
                               static_cast<u64>(gregs[REG_RDI]));
            out << fmt::format("R8:  0x{:016X}    R9:  0x{:016X}    R10: 0x{:016X}\n",
                               static_cast<u64>(gregs[REG_R8]),
                               static_cast<u64>(gregs[REG_R9]),
                               static_cast<u64>(gregs[REG_R10]));
            out << fmt::format("R11: 0x{:016X}    R12: 0x{:016X}    R13: 0x{:016X}\n",
                               static_cast<u64>(gregs[REG_R11]),
                               static_cast<u64>(gregs[REG_R12]),
                               static_cast<u64>(gregs[REG_R13]));
            out << fmt::format("R14: 0x{:016X}    R15: 0x{:016X}    EFL: 0x{:016X}\n",
                               static_cast<u64>(gregs[REG_R14]),
                               static_cast<u64>(gregs[REG_R15]),
                               static_cast<u64>(gregs[REG_EFL]));
#elif defined(__aarch64__)
            const auto& mc = ctx->uc_mcontext;
            out << fmt::format("PC:  0x{:016X}    SP:  0x{:016X}    PSTATE: 0x{:016X}\n",
                               static_cast<u64>(mc.pc),
                               static_cast<u64>(mc.sp),
                               static_cast<u64>(mc.pstate));
            for (int i = 0; i < 31; i += 3)
            {
                out << fmt::format("X{:<2}: 0x{:016X}", i, static_cast<u64>(mc.regs[i]));
                if (i + 1 < 31)
                {
                    out << fmt::format("    X{:<2}: 0x{:016X}", i + 1, static_cast<u64>(mc.regs[i + 1]));
                }
                if (i + 2 < 31)
                {
                    out << fmt::format("    X{:<2}: 0x{:016X}", i + 2, static_cast<u64>(mc.regs[i + 2]));
                }
                out << "\n";
            }
#else
            out << "(register dump not implemented for this architecture)\n";
#endif
        }

        void WriteBacktrace(std::ostream& out)
        {
            constexpr int kMaxFrames = 64;
            void* frames[kMaxFrames];
            const int nFrames = ::backtrace(frames, kMaxFrames);
            if (nFrames <= 0)
            {
                out << "(backtrace unavailable)\n";
                return;
            }

            char** symbols = ::backtrace_symbols(frames, nFrames);
            if (symbols == nullptr)
            {
                out << fmt::format("(captured {} frames but symbol resolution failed)\n", nFrames);
                return;
            }

            // Skip the top 2 frames (this function + WriteMiniDump) so the dump
            // starts at the signal handler, then the faulting frame. Keep the
            // index numbering meaningful by starting at 0 at the visible top.
            constexpr int kSkip = 2;
            const int startFrame = (nFrames > kSkip) ? kSkip : 0;
            for (int i = startFrame; i < nFrames; ++i)
            {
                out << fmt::format("[{:>2}] 0x{:016X}  {}\n",
                                   i - startFrame,
                                   reinterpret_cast<uintptr_t>(frames[i]),
                                   DemangleBacktraceFrame(symbols[i]));
            }

            ::free(symbols); // backtrace_symbols returns malloc'd buffer
        }

        // Spawn `program arg1 arg2 …` and wait for completion. Returns the exit
        // status on success or -1 if exec failed. Used for zenity / kdialog
        // dispatch on the crash-dialog path.
        int SpawnAndWait(const char* program, const std::vector<std::string>& args)
        {
            const pid_t pid = ::fork();
            if (pid < 0)
            {
                return -1;
            }
            if (pid == 0)
            {
                std::vector<char*> argv;
                argv.reserve(args.size() + 2);
                argv.push_back(const_cast<char*>(program));
                for (const auto& a : args)
                {
                    argv.push_back(const_cast<char*>(a.c_str()));
                }
                argv.push_back(nullptr);
                ::execvp(program, argv.data());
                // exec failed — bail with a recognisable code
                _exit(127);
            }
            int status = 0;
            while (::waitpid(pid, &status, 0) == -1)
            {
                if (errno != EINTR)
                {
                    return -1;
                }
            }
            if (WIFEXITED(status))
            {
                return WEXITSTATUS(status);
            }
            return -1;
        }

        bool ProgramExists(const char* program)
        {
            // PATH-lookup probe: spawn the program with `--version` redirected to
            // /dev/null; success or any zero-ish exit means the binary exists.
            // Done this way (rather than parsing $PATH manually) to handle PATHEXT
            // and any wrapper scripts the same way the shell would.
            const pid_t pid = ::fork();
            if (pid < 0)
            {
                return false;
            }
            if (pid == 0)
            {
                ::close(STDOUT_FILENO);
                ::close(STDERR_FILENO);
                ::execlp(program, program, "--version", static_cast<char*>(nullptr));
                _exit(127);
            }
            int status = 0;
            while (::waitpid(pid, &status, 0) == -1)
            {
                if (errno != EINTR)
                {
                    return false;
                }
            }
            return WIFEXITED(status) && WEXITSTATUS(status) != 127;
        }

    } // namespace

    void InstallHandlers(WriteCrashReportFn callback)
    {
        s_WriteCrashReport.store(callback, std::memory_order_release);

        // Clear the re-entry guard so a fresh InstallHandlers (e.g. after a
        // matching UninstallHandlers in a test) starts with the flag cleared.
        // Without this, a Shutdown / Init sequence would leave the flag stuck
        // and a subsequent real crash would short-circuit straight to default.
        s_HandlerEntered.clear(std::memory_order_release);

        // Install an alt-stack so SIGSEGV from stack overflow can still run our
        // handler. Use sigaltstack() (not the deprecated sigstack()).
        stack_t altStack{};
        altStack.ss_sp = s_AltStack.data();
        altStack.ss_size = s_AltStack.size();
        altStack.ss_flags = 0;
        if (::sigaltstack(&altStack, nullptr) == 0)
        {
            s_AltStackInstalled = true;
        }
        // If alt-stack install failed (rare — typically only when SIGSTKSZ is
        // larger than our buffer), we still install the handlers; we just lose
        // stack-overflow recovery.

        struct sigaction sa{};
        sa.sa_sigaction = &SignalHandler;
        ::sigemptyset(&sa.sa_mask);
        // SA_SIGINFO   → use sa_sigaction (3-arg form) with siginfo_t + ucontext_t.
        // SA_ONSTACK   → run on the alt-stack so we survive stack overflows.
        // SA_NODEFER   → don't block the signal we're handling; combined with the
        //                re-entry guard, this prevents the kernel from suppressing
        //                the second raise() we issue at the end of the handler.
        // SA_RESETHAND → if the handler itself faults, the second hit goes to the
        //                OS default (core dump) instead of looping. We also reset
        //                the handler manually before re-raising, so this is belt-
        //                and-braces.
        sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
        if (s_AltStackInstalled)
        {
            sa.sa_flags |= SA_ONSTACK;
        }

        for (sizet i = 0; i < kFatalSignals.size(); ++i)
        {
            const int sig = kFatalSignals[i];
            s_PreviousActions[i].Signal = sig;
            if (::sigaction(sig, &sa, &s_PreviousActions[i].Action) == 0)
            {
                s_PreviousActions[i].Installed = true;
            }
            else
            {
                OLO_CORE_WARN("[CrashReporter] sigaction({}) failed: {}",
                              SignalToString(sig), std::strerror(errno));
            }
        }

        s_PreviousTerminateHandler = std::set_terminate(TerminateHandler);
    }

    void UninstallHandlers()
    {
        for (auto& prev : s_PreviousActions)
        {
            if (!prev.Installed)
            {
                continue;
            }
            ::sigaction(prev.Signal, &prev.Action, nullptr);
            prev.Installed = false;
        }

        if (s_AltStackInstalled)
        {
            // Disable the alt-stack and let the kernel free our reference to it.
            stack_t disable{};
            disable.ss_flags = SS_DISABLE;
            ::sigaltstack(&disable, nullptr);
            s_AltStackInstalled = false;
        }

        if (s_PreviousTerminateHandler)
        {
            std::set_terminate(s_PreviousTerminateHandler);
            s_PreviousTerminateHandler = nullptr;
        }

        s_WriteCrashReport.store(nullptr, std::memory_order_release);
    }

    bool WriteMiniDump(const std::filesystem::path& dumpPath, void* platformContext)
    {
        // Linux has no native minidump format. We emit a plain-text dump with
        // the equivalent information (signal context + register state + backtrace
        // + a slice of /proc/self/maps) so post-mortem triage works without
        // needing breakpad / lldb.
        std::ofstream dump(dumpPath, std::ios::out | std::ios::trunc);
        if (!dump.is_open())
        {
            return false;
        }

        dump << "OloEngine Linux Text MiniDump\n";
        dump << "PID: " << ::getpid() << "\n";
        dump << "TID: " << ::syscall(SYS_gettid) << "\n";

        if (platformContext != nullptr)
        {
            auto* linuxCtx = static_cast<LinuxSignalContext*>(platformContext);
            if (linuxCtx->Info != nullptr)
            {
                dump << "Signal: " << SignalToString(linuxCtx->Info->si_signo)
                     << " (" << linuxCtx->Info->si_signo << ")\n";
                dump << "Code:   " << SigcodeToString(linuxCtx->Info->si_signo, linuxCtx->Info->si_code)
                     << " (" << linuxCtx->Info->si_code << ")\n";
                if (linuxCtx->Info->si_signo == SIGSEGV || linuxCtx->Info->si_signo == SIGBUS)
                {
                    dump << fmt::format("Fault address: 0x{:016X}\n",
                                        reinterpret_cast<uintptr_t>(linuxCtx->Info->si_addr));
                }
            }
            dump << "\n--- Registers ---\n";
            WriteRegisterDump(dump, linuxCtx->Context);
        }
        else
        {
            dump << "(no signal context — synthetic crash report)\n";
        }

        dump << "\n--- Backtrace ---\n";
        WriteBacktrace(dump);

        dump << "\n--- /proc/self/maps (truncated) ---\n";
        {
            std::ifstream maps("/proc/self/maps");
            if (maps.is_open())
            {
                // Cap at 256 lines; full maps files on heavy processes can be huge
                // and a small head is plenty for triage (identifies loaded modules).
                std::string line;
                int linesEmitted = 0;
                constexpr int kMaxLines = 256;
                while (linesEmitted < kMaxLines && std::getline(maps, line))
                {
                    dump << line << "\n";
                    ++linesEmitted;
                }
                if (linesEmitted == kMaxLines && std::getline(maps, line))
                {
                    dump << "... (truncated after " << kMaxLines << " mappings)\n";
                }
            }
            else
            {
                dump << "(unavailable)\n";
            }
        }

        dump.flush();
        return !dump.fail();
    }

    std::string CollectSystemInfo()
    {
        std::ostringstream info;

        // Distribution + kernel
        std::string osRelease = SlurpFile("/etc/os-release");
        std::string distro;
        if (!osRelease.empty())
        {
            distro = ExtractOsReleaseField(osRelease, "PRETTY_NAME");
            if (distro.empty())
            {
                std::string name = ExtractOsReleaseField(osRelease, "NAME");
                std::string version = ExtractOsReleaseField(osRelease, "VERSION");
                if (!name.empty() || !version.empty())
                {
                    distro = name + (version.empty() ? std::string{} : (" " + version));
                }
            }
        }

        struct utsname uts{};
        if (::uname(&uts) == 0)
        {
            info << "OS: " << (distro.empty() ? "Linux" : distro)
                 << " (kernel " << uts.release << " " << uts.machine << ")\n";
        }
        else
        {
            info << "OS: " << (distro.empty() ? "Linux" : distro) << "\n";
        }

        // CPU model + count
        std::string cpuinfo = SlurpFile("/proc/cpuinfo");
        std::string modelName;
        if (!cpuinfo.empty())
        {
            modelName = FirstCpuinfoField(cpuinfo, "model name");
            if (modelName.empty())
            {
                // aarch64 / older ARM kernels use "Processor" or "Hardware"
                modelName = FirstCpuinfoField(cpuinfo, "Processor");
            }
        }
        const long nproc = ::sysconf(_SC_NPROCESSORS_ONLN);
        info << "CPU: " << (modelName.empty() ? "(unknown)" : modelName);
        if (nproc > 0)
        {
            info << " — " << nproc << " logical processors";
        }
        info << "\n";

        // Memory — prefer sysinfo() (deals in actual bytes) over parsing
        // /proc/meminfo's kB strings.
        struct sysinfo si{};
        if (::sysinfo(&si) == 0)
        {
            const u64 unitBytes = static_cast<u64>(si.mem_unit ? si.mem_unit : 1);
            const u64 totalRam = static_cast<u64>(si.totalram) * unitBytes;
            const u64 freeRam = static_cast<u64>(si.freeram) * unitBytes;
            const u64 totalSwap = static_cast<u64>(si.totalswap) * unitBytes;
            const u64 freeSwap = static_cast<u64>(si.freeswap) * unitBytes;
            constexpr u64 oneMiB = 1024ull * 1024ull;
            info << "Physical Memory: " << (totalRam / oneMiB) << " MB total, "
                 << (freeRam / oneMiB) << " MB free\n";
            info << "Swap:            " << (totalSwap / oneMiB) << " MB total, "
                 << (freeSwap / oneMiB) << " MB free\n";
            info << "Uptime: " << si.uptime << "s, load 1m=" << si.loads[0]
                 << " 5m=" << si.loads[1] << " 15m=" << si.loads[2] << "\n";
        }

        return info.str();
    }

    void ShowCrashDialog(const std::string& title, const std::string& message, bool isHeadless)
    {
        if (isHeadless)
        {
            return;
        }

        // Skip silently when there's no display — typical for OloServer running
        // under systemd or in a container. The crash report already went to disk
        // and to the log; a popup would just be a no-op fork+exec cost.
        const char* display = ::getenv("DISPLAY");
        const char* waylandDisplay = ::getenv("WAYLAND_DISPLAY");
        if ((display == nullptr || display[0] == '\0') &&
            (waylandDisplay == nullptr || waylandDisplay[0] == '\0'))
        {
            return;
        }

        // Try zenity (GNOME / generic) first, then kdialog. Both are common on
        // desktop distros; if neither is installed we accept the no-op.
        if (ProgramExists("zenity"))
        {
            (void)SpawnAndWait("zenity", { "--error",
                                           "--title=" + title,
                                           "--text=" + message,
                                           "--no-wrap" });
            return;
        }
        if (ProgramExists("kdialog"))
        {
            (void)SpawnAndWait("kdialog", { "--title", title,
                                            "--error", message });
            return;
        }
        // No dialog binary available — silent.
    }

    void EmitToDebugOutput(const std::string& message)
    {
        // No OutputDebugStringA equivalent on Linux. Write to stderr so the line
        // is visible in journalctl / terminal output of headless services.
        std::fputs(message.c_str(), stderr);
        // Ensure the message reaches the terminal even if stderr is line-buffered
        // and the caller didn't include a trailing newline.
        std::fflush(stderr);
    }

} // namespace OloEngine::CrashReporterPlatform

#endif // OLO_PLATFORM_LINUX
