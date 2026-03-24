#pragma once

#include <exception>
#include <filesystem>
#include <string>

namespace OloEngine
{
    class CrashReporter
    {
      public:
        /// Install crash handlers and set the directory for crash report output.
        /// Call after Log::Init() so that the ringbuffer sink is available.
        static void Init(const std::filesystem::path& crashReportDir = "CrashReports");

        /// Uninstall crash handlers. Call during shutdown.
        static void Shutdown();

        /// Set identifiers shown in crash reports (e.g. from game.manifest).
        static void SetApplicationInfo(const std::string& name, const std::string& version);

        /// Cache whether the application runs in headless mode (no message boxes on crash).
        static void SetHeadless(bool headless);

        /// Store the GPU renderer string for inclusion in crash reports.
        static void SetGPUInfo(const std::string& info);

        /// Manually generate a crash report for a caught C++ exception.
        /// The process continues after this call.
        static void ReportCaughtException(const std::exception& e);

        /// Manually generate a crash report for an unknown fatal error.
        /// The process continues after this call.
        static void ReportFatalError(const std::string& message);

        /// Write crash report (also used by platform-specific handlers).
        static void WriteCrashReport(const std::string& exceptionType,
                                     const std::string& exceptionDetail,
                                     void* exceptionPointersOrNull);

      private:
        static void InstallPlatformHandlers();
        static void UninstallPlatformHandlers();
        static std::string CollectSystemInfo();
        static std::string GetTimestamp();
    };
} // namespace OloEngine
