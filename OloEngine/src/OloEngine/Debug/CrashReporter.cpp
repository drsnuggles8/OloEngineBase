#include "OloEnginePCH.h"
#include "OloEngine/Debug/CrashReporter.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Profiler.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

#ifdef OLO_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <DbgHelp.h>
#endif

namespace OloEngine
{
    // -------------------------------------------------------------------
    // Static data — pre-allocated strings so crash-time code doesn't alloc
    // -------------------------------------------------------------------
    static std::filesystem::path s_CrashReportDir;
    static std::string s_AppName = "OloEngine Application";
    static std::string s_AppVersion = "0.0.0";
    static std::string s_GPUInfo;
    static bool s_Initialized = false;
    static bool s_IsHeadless = false;

#ifdef OLO_PLATFORM_WINDOWS
    static LPTOP_LEVEL_EXCEPTION_FILTER s_PreviousFilter = nullptr;
    static std::terminate_handler s_PreviousTerminateHandler = nullptr;

    // -------------------------------------------------------------------
    // SEH exception code → human-readable name
    // -------------------------------------------------------------------
    static const char* ExceptionCodeToString(DWORD code)
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

    // -------------------------------------------------------------------
    // Write a minidump file alongside the crash report
    // -------------------------------------------------------------------
    static void WriteMiniDump(const std::filesystem::path& dumpPath, EXCEPTION_POINTERS* exceptionPointers)
    {
        const HANDLE hFile = CreateFileW(
            dumpPath.c_str(),
            GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            return;
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
    }

    // -------------------------------------------------------------------
    // Windows SEH top-level exception filter
    // -------------------------------------------------------------------
    static LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionPointers)
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

        CrashReporter::WriteCrashReport("Windows SEH Exception", detail, exceptionPointers);

        // Chain to the previous handler if one existed
        if (s_PreviousFilter)
        {
            return s_PreviousFilter(exceptionPointers);
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }

    // -------------------------------------------------------------------
    // std::terminate handler
    // -------------------------------------------------------------------
    static void TerminateHandler()
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

        CrashReporter::WriteCrashReport("std::terminate", detail, nullptr);

        // Call the previous terminate handler or abort
        if (s_PreviousTerminateHandler)
        {
            s_PreviousTerminateHandler();
        }
        else
        {
            std::abort();
        }
    }
#endif // OLO_PLATFORM_WINDOWS

    // ===================================================================
    // Public API
    // ===================================================================

    void CrashReporter::Init(const std::filesystem::path& crashReportDir)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Initialized)
        {
            return;
        }

        s_CrashReportDir = crashReportDir;

        // Ensure the crash report directory exists
        std::error_code ec;
        std::filesystem::create_directories(s_CrashReportDir, ec);
        if (ec)
        {
            OLO_CORE_ERROR("[CrashReporter] Failed to create crash report directory '{}': {} — crash reports will fallback to stderr",
                           s_CrashReportDir.string(), ec.message());
            // Continue in degraded mode — WriteCrashReport will fallback to stderr
        }

        InstallPlatformHandlers();
        s_Initialized = true;

        if (!ec)
        {
            OLO_CORE_INFO("[CrashReporter] Initialized — reports will be written to: {}", s_CrashReportDir.string());
        }
    }

    void CrashReporter::Shutdown()
    {
        if (!s_Initialized)
        {
            return;
        }

        UninstallPlatformHandlers();
        s_Initialized = false;
    }

    void CrashReporter::SetApplicationInfo(const std::string& name, const std::string& version)
    {
        s_AppName = name;
        s_AppVersion = version;
    }

    void CrashReporter::SetHeadless(bool headless)
    {
        s_IsHeadless = headless;
    }

    void CrashReporter::SetGPUInfo(const std::string& info)
    {
        s_GPUInfo = info;
    }

    void CrashReporter::ReportCaughtException(const std::exception& e)
    {
        WriteCrashReport("C++ Exception (caught)", e.what(), nullptr);
    }

    void CrashReporter::ReportFatalError(const std::string& message)
    {
        WriteCrashReport("Fatal Error", message, nullptr);
    }

    // ===================================================================
    // Platform handler installation
    // ===================================================================

    void CrashReporter::InstallPlatformHandlers()
    {
#ifdef OLO_PLATFORM_WINDOWS
        s_PreviousFilter = SetUnhandledExceptionFilter(UnhandledExceptionHandler);
        s_PreviousTerminateHandler = std::set_terminate(TerminateHandler);
#endif
    }

    void CrashReporter::UninstallPlatformHandlers()
    {
#ifdef OLO_PLATFORM_WINDOWS
        SetUnhandledExceptionFilter(s_PreviousFilter);
        s_PreviousFilter = nullptr;
        std::set_terminate(s_PreviousTerminateHandler);
        s_PreviousTerminateHandler = nullptr;
#endif
    }

    // ===================================================================
    // Core crash report writer
    // ===================================================================

    void CrashReporter::WriteCrashReport(
        const std::string& exceptionType,
        const std::string& exceptionDetail,
        void* exceptionPointersOrNull)
    {
        const std::string timestamp = GetTimestamp();

        // Build filenames — use timestamp so multiple crashes don't overwrite
        const std::string baseName = fmt::format("crash_{}", timestamp);
        const auto reportPath = s_CrashReportDir / (baseName + ".txt");

#ifdef OLO_PLATFORM_WINDOWS
        // Write minidump if we have exception pointers
        if (exceptionPointersOrNull)
        {
            const auto dumpPath = s_CrashReportDir / (baseName + ".dmp");
            WriteMiniDump(dumpPath, static_cast<EXCEPTION_POINTERS*>(exceptionPointersOrNull));
        }
#endif

        // Build the crash report text
        std::ostringstream report;
        report << "========================================\n";
        report << " OloEngine Crash Report\n";
        report << "========================================\n\n";

        report << "Timestamp:  " << timestamp << "\n";
        report << "Application: " << s_AppName << "\n";
        report << "Version:    " << s_AppVersion << "\n\n";

        report << "--- Exception ---\n";
        report << "Type:   " << exceptionType << "\n";
        report << "Detail: " << exceptionDetail << "\n\n";

        report << "--- System Info ---\n";
        report << CollectSystemInfo() << "\n";

        // Grab the last N log lines from the ringbuffer
        report << "--- Recent Log Messages (last 200) ---\n";
        auto recentLogs = Log::GetRecentLogMessages();
        if (recentLogs.empty())
        {
            report << "(no log messages captured)\n";
        }
        else
        {
            for (const auto& line : recentLogs)
            {
                report << line;
                // spdlog formatted messages may or may not end with newline
                if (line.empty() || line.back() != '\n')
                {
                    report << '\n';
                }
            }
        }
        report << "\n========================================\n";
        report << " End of Crash Report\n";
        report << "========================================\n";

        // Write the report file
        std::ofstream file(reportPath, std::ios::out | std::ios::trunc);
        if (file.is_open())
        {
            file << report.str();
            file.flush();
            if (file.fail())
            {
                // Partial write — fall back to stderr
                std::cerr << "[CrashReporter] Failed to write crash report to: " << reportPath.string() << "\n";
                std::cerr << report.str() << std::flush;
#ifdef OLO_PLATFORM_WINDOWS
                OutputDebugStringA(report.str().c_str());
#endif
            }
            file.close();
        }
        else
        {
            // Cannot open file — emit to stderr as fallback
            std::cerr << "[CrashReporter] Could not open crash report file: " << reportPath.string() << "\n";
            std::cerr << report.str() << std::flush;
#ifdef OLO_PLATFORM_WINDOWS
            OutputDebugStringA(report.str().c_str());
#endif
        }

        // Also log the crash (this goes to OloEngine.log and console)
        OLO_CORE_FATAL("=== CRASH: {} — {} ===", exceptionType, exceptionDetail);
        if (file.is_open() && !file.fail())
        {
            OLO_CORE_FATAL("Crash report written to: {}", reportPath.string());
        }

#ifdef OLO_PLATFORM_WINDOWS
        // Show a message box so the user knows something went wrong — only in interactive mode
        if (!s_IsHeadless)
        {
            const std::string msg = fmt::format(
                "{} has crashed.\n\n"
                "Error: {}\n\n"
                "A crash report has been saved to:\n{}\n\n"
                "Please send this file (and the .dmp file if present) to the developers.",
                s_AppName, exceptionDetail, reportPath.string());

            MessageBoxA(nullptr, msg.c_str(), "Crash Report", MB_OK | MB_ICONERROR | MB_TASKMODAL);
        }
#endif
    }

    // ===================================================================
    // System information collector
    // ===================================================================

    std::string CrashReporter::CollectSystemInfo()
    {
        std::ostringstream info;

#ifdef OLO_PLATFORM_WINDOWS
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
#else
        info << "OS: Linux (details unavailable in this build)\n";
#endif

        // GPU (set via SetGPUInfo from the renderer)
        if (!s_GPUInfo.empty())
        {
            info << "GPU: " << s_GPUInfo << "\n";
        }

        return info.str();
    }

    // ===================================================================
    // Timestamp helper (filesystem-safe format)
    // ===================================================================

    std::string CrashReporter::GetTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) %
                        1000;

        std::tm localTime{};
#ifdef OLO_PLATFORM_WINDOWS
        localtime_s(&localTime, &time);
#else
        localtime_r(&time, &localTime);
#endif

        // Format: 20250321_143025_123
        return fmt::format("{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}",
                           localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
                           localTime.tm_hour, localTime.tm_min, localTime.tm_sec,
                           ms.count());
    }

} // namespace OloEngine
