#include "OloEnginePCH.h"
#include "OloEngine/Debug/CrashReporter.h"
#include "OloEngine/Debug/CrashReporterPlatform.h"

#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Debug/Profiler.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

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
            OLO_CORE_INFO("[CrashReporter] Initialized. Reports will be written to: {}", s_CrashReportDir.string());
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
        CrashReporterPlatform::InstallHandlers(&CrashReporter::WriteCrashReport);
    }

    void CrashReporter::UninstallPlatformHandlers()
    {
        CrashReporterPlatform::UninstallHandlers();
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

        // Write minidump (OS-specific) if we have an exception context
        if (exceptionPointersOrNull)
        {
            const auto dumpPath = s_CrashReportDir / (baseName + ".dmp");
            CrashReporterPlatform::WriteMiniDump(dumpPath, exceptionPointersOrNull);
        }

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
        const auto* logInstance = Log::GetIfInitialized();
        if (auto recentLogs = logInstance ? logInstance->GetRecentLogMessages() : std::vector<std::string>{}; recentLogs.empty())
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
                CrashReporterPlatform::EmitToDebugOutput(report.str());
            }
            file.close();
        }
        else
        {
            // Cannot open file — emit to stderr as fallback
            std::cerr << "[CrashReporter] Could not open crash report file: " << reportPath.string() << "\n";
            std::cerr << report.str() << std::flush;
            CrashReporterPlatform::EmitToDebugOutput(report.str());
        }

        // Also log the crash (this goes to OloEngine.log and console)
        OLO_CORE_FATAL("=== CRASH: {} — {} ===", exceptionType, exceptionDetail);
        if (file.is_open() && !file.fail())
        {
            OLO_CORE_FATAL("Crash report written to: {}", reportPath.string());
        }

        // Show a crash dialog (no-op in headless / non-GUI environments)
        const std::string msg = fmt::format(
            "{} has crashed.\n\n"
            "Error: {}\n\n"
            "A crash report has been saved to:\n{}\n\n"
            "Please send this file (and the .dmp file if present) to the developers.",
            s_AppName, exceptionDetail, reportPath.string());
        CrashReporterPlatform::ShowCrashDialog("Crash Report", msg, s_IsHeadless);
    }

    // ===================================================================
    // System information collector
    // ===================================================================

    std::string CrashReporter::CollectSystemInfo()
    {
        std::ostringstream info;
        info << CrashReporterPlatform::CollectSystemInfo();

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
