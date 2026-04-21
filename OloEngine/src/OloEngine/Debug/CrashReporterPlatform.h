// Platform-specific hooks for CrashReporter.
//
// The generic CrashReporter lives in OloEngine/Debug/CrashReporter.cpp and
// calls into this interface for OS-specific behavior:
//   * Installing SEH/signal handlers that funnel into WriteCrashReport.
//   * Writing minidumps / platform-specific crash artifacts.
//   * Collecting OS version, CPU, and memory info.
//   * Showing a message box on crash (interactive builds only).
//
// Implementations live in Platform/<OS>/<OS>CrashReporter.cpp.

#pragma once

#include <filesystem>
#include <string>

namespace OloEngine::CrashReporterPlatform
{
    // Callback signature used by OS-specific handlers to funnel crashes
    // back into the generic CrashReporter::WriteCrashReport.
    // platformContext is opaque: EXCEPTION_POINTERS* on Windows, nullptr elsewhere.
    using WriteCrashReportFn = void (*)(const std::string& exceptionType,
                                        const std::string& exceptionDetail,
                                        void* platformContext);

    /// Install the OS-specific unhandled-exception / fatal-signal handler.
    void InstallHandlers(WriteCrashReportFn callback);

    /// Uninstall the OS-specific handlers installed by InstallHandlers.
    void UninstallHandlers();

    /// Write a minidump (or equivalent) file from an OS-specific context.
    /// platformContext is EXCEPTION_POINTERS* on Windows, ignored elsewhere.
    /// Returns true on success.
    bool WriteMiniDump(const std::filesystem::path& dumpPath, void* platformContext);

    /// Gather OS version, CPU, and memory info into a formatted multi-line string.
    std::string CollectSystemInfo();

    /// Show a modal crash dialog (no-op in headless mode or on platforms without UI).
    void ShowCrashDialog(const std::string& title, const std::string& message, bool isHeadless);

    /// Emit a line to the OS debug console (OutputDebugStringA on Windows, no-op elsewhere).
    void EmitToDebugOutput(const std::string& message);

} // namespace OloEngine::CrashReporterPlatform
