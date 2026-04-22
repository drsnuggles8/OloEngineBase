// Windows implementation of CrashReporterPlatform.
// Owns all SEH / DbgHelp interaction; links against DbgHelp.lib.

#include "OloEnginePCH.h"
#include "OloEngine/Debug/CrashReporterPlatform.h"

#ifdef OLO_PLATFORM_WINDOWS

#include "OloEngine/Core/Log.h"

#include <atomic>
#include <exception>
#include <sstream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>

namespace OloEngine::CrashReporterPlatform
{
    namespace
    {
        // Filter + terminate handlers are installed once from the main thread before
        // any worker threads start, and uninstalled after they've all joined, so the
        // pointers themselves are effectively const during multi-threaded execution.
        // The user callback can be swapped at runtime, so it is stored atomically to
        // avoid torn reads on platforms where pointer stores aren't inherently atomic.
        LPTOP_LEVEL_EXCEPTION_FILTER s_PreviousFilter = nullptr;
        std::terminate_handler s_PreviousTerminateHandler = nullptr;
        std::atomic<WriteCrashReportFn> s_WriteCrashReport{ nullptr };

        const char* ExceptionCodeToString(DWORD code)
        {
            switch (code)
            {
                case EXCEPTION_ACCESS_VIOLATION:
                    return "EXCEPTION_ACCESS_VIOLATION";
                case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
                    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
                case EXCEPTION_BREAKPOINT:
                    return "EXCEPTION_BREAKPOINT";
                case EXCEPTION_DATATYPE_MISALIGNMENT:
                    return "EXCEPTION_DATATYPE_MISALIGNMENT";
                case EXCEPTION_FLT_DENORMAL_OPERAND:
                    return "EXCEPTION_FLT_DENORMAL_OPERAND";
                case EXCEPTION_FLT_DIVIDE_BY_ZERO:
                    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
                case EXCEPTION_FLT_INEXACT_RESULT:
                    return "EXCEPTION_FLT_INEXACT_RESULT";
                case EXCEPTION_FLT_INVALID_OPERATION:
                    return "EXCEPTION_FLT_INVALID_OPERATION";
                case EXCEPTION_FLT_OVERFLOW:
                    return "EXCEPTION_FLT_OVERFLOW";
                case EXCEPTION_FLT_STACK_CHECK:
                    return "EXCEPTION_FLT_STACK_CHECK";
                case EXCEPTION_FLT_UNDERFLOW:
                    return "EXCEPTION_FLT_UNDERFLOW";
                case EXCEPTION_ILLEGAL_INSTRUCTION:
                    return "EXCEPTION_ILLEGAL_INSTRUCTION";
                case EXCEPTION_IN_PAGE_ERROR:
                    return "EXCEPTION_IN_PAGE_ERROR";
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
                case EXCEPTION_INT_OVERFLOW:
                    return "EXCEPTION_INT_OVERFLOW";
                case EXCEPTION_INVALID_DISPOSITION:
                    return "EXCEPTION_INVALID_DISPOSITION";
                case EXCEPTION_NONCONTINUABLE_EXCEPTION:
                    return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
                case EXCEPTION_PRIV_INSTRUCTION:
                    return "EXCEPTION_PRIV_INSTRUCTION";
                case EXCEPTION_SINGLE_STEP:
                    return "EXCEPTION_SINGLE_STEP";
                case EXCEPTION_STACK_OVERFLOW:
                    return "EXCEPTION_STACK_OVERFLOW";
                default:
                    return "UNKNOWN_EXCEPTION";
            }
        }

        LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionPointers)
        {
            const DWORD code = exceptionPointers->ExceptionRecord->ExceptionCode;

            // Skip breakpoints in debug builds
            if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP)
            {
                if (s_PreviousFilter)
                {
                    return s_PreviousFilter(exceptionPointers);
                }
                return EXCEPTION_CONTINUE_SEARCH;
            }

            std::string detail;
            if (code == EXCEPTION_ACCESS_VIOLATION && exceptionPointers->ExceptionRecord->NumberParameters >= 2)
            {
                auto accessType = exceptionPointers->ExceptionRecord->ExceptionInformation[0];
                auto address = exceptionPointers->ExceptionRecord->ExceptionInformation[1];
                detail = fmt::format("{} at address 0x{:016X} ({})",
                                     ExceptionCodeToString(code),
                                     address,
                                     accessType == 0 ? "read" : (accessType == 1 ? "write" : "execute"));
            }
            else
            {
                detail = fmt::format("{} (code 0x{:08X})", ExceptionCodeToString(code), code);
            }

            if (auto Cb = s_WriteCrashReport.load(std::memory_order_acquire))
            {
                Cb("Windows SEH Exception", detail, exceptionPointers);
            }

            // Chain to the previous handler if one existed
            if (s_PreviousFilter)
            {
                return s_PreviousFilter(exceptionPointers);
            }

            return EXCEPTION_EXECUTE_HANDLER;
        }

        [[noreturn]] void TerminateHandler()
        {
            std::string detail = "std::terminate() called";

            // Try to get info from a current exception
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

            // Call the previous terminate handler if one was installed.
            // If it returns (which it should not per the std::terminate contract),
            // we fall through to std::abort() to guarantee [[noreturn]].
            if (s_PreviousTerminateHandler)
            {
                s_PreviousTerminateHandler();
            }
            std::abort();
        }
    } // namespace

    void InstallHandlers(WriteCrashReportFn callback)
    {
        s_WriteCrashReport.store(callback, std::memory_order_release);
        s_PreviousFilter = SetUnhandledExceptionFilter(UnhandledExceptionHandler);
        s_PreviousTerminateHandler = std::set_terminate(TerminateHandler);
    }

    void UninstallHandlers()
    {
        SetUnhandledExceptionFilter(s_PreviousFilter);
        s_PreviousFilter = nullptr;
        std::set_terminate(s_PreviousTerminateHandler);
        s_PreviousTerminateHandler = nullptr;
        s_WriteCrashReport.store(nullptr, std::memory_order_release);
    }

    bool WriteMiniDump(const std::filesystem::path& dumpPath, void* platformContext)
    {
        auto* exceptionPointers = static_cast<EXCEPTION_POINTERS*>(platformContext);

        const HANDLE hFile = CreateFileW(
            dumpPath.c_str(),
            GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        MINIDUMP_EXCEPTION_INFORMATION mdei{};
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = exceptionPointers;
        mdei.ClientPointers = FALSE;

        // MiniDumpWithDataSegs gives us global variables which is useful for debugging
        const auto dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithThreadInfo);

        BOOL dumpResult = MiniDumpWriteDump(
            GetCurrentProcess(),
            GetCurrentProcessId(),
            hFile,
            dumpType,
            exceptionPointers ? &mdei : nullptr,
            nullptr, nullptr);

        if (!dumpResult)
        {
            // Log failure — disk full, permissions, etc.
            DWORD err = GetLastError();
            OutputDebugStringA(("[CrashReporter] MiniDumpWriteDump failed, error: " + std::to_string(err) + "\n").c_str());
        }

        CloseHandle(hFile);
        return dumpResult != FALSE;
    }

    std::string CollectSystemInfo()
    {
        std::ostringstream info;

        // OS version
        OSVERSIONINFOEXW osvi{};
        osvi.dwOSVersionInfoSize = sizeof(osvi);

        // RtlGetVersion is always available and not subject to the manifest-based lies of GetVersionEx
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (auto* const ntdll = GetModuleHandleW(L"ntdll.dll"))
        {
            if (auto const rtlGetVersion = reinterpret_cast<RtlGetVersionFn>( // NOLINT
                    GetProcAddress(ntdll, "RtlGetVersion")))
            {
                rtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osvi)); // NOLINT
            }
        }
        info << "OS: Windows " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion
             << " (Build " << osvi.dwBuildNumber << ")\n";

        // CPU
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        info << "CPU: " << si.dwNumberOfProcessors << " logical processors\n";
        info << "CPU Architecture: ";
        switch (si.wProcessorArchitecture)
        {
            case PROCESSOR_ARCHITECTURE_AMD64:
                info << "x64";
                break;
            case PROCESSOR_ARCHITECTURE_ARM64:
                info << "ARM64";
                break;
            default:
                info << "other (" << si.wProcessorArchitecture << ")";
                break;
        }
        info << "\n";

        // Memory
        MEMORYSTATUSEX memStatus{};
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus))
        {
            info << "Physical Memory: " << (memStatus.ullTotalPhys / (1024 * 1024)) << " MB total, "
                 << (memStatus.ullAvailPhys / (1024 * 1024)) << " MB available\n";
            info << "Virtual Memory:  " << (memStatus.ullTotalVirtual / (1024 * 1024)) << " MB total, "
                 << (memStatus.ullAvailVirtual / (1024 * 1024)) << " MB available\n";
        }

        return info.str();
    }

    void ShowCrashDialog(const std::string& title, const std::string& message, bool isHeadless)
    {
        if (isHeadless)
        {
            return;
        }
        MessageBoxA(nullptr, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_TASKMODAL);
    }

    void EmitToDebugOutput(const std::string& message)
    {
        OutputDebugStringA(message.c_str());
    }

} // namespace OloEngine::CrashReporterPlatform

#endif // OLO_PLATFORM_WINDOWS
