// Linux implementation of CrashReporterPlatform.
// Currently a minimal stub — signal-based handlers / backtrace() can be added later.

#include "OloEnginePCH.h"
#include "OloEngine/Debug/CrashReporterPlatform.h"

#ifdef OLO_PLATFORM_LINUX

namespace OloEngine::CrashReporterPlatform
{
    void InstallHandlers(WriteCrashReportFn /*callback*/)
    {
        // TODO: install sigaction(SIGSEGV/SIGABRT/SIGFPE/...) handlers that funnel
        // into the provided callback via sigaltstack-safe code.
    }

    void UninstallHandlers()
    {
        // No-op — nothing installed yet.
    }

    bool WriteMiniDump(const std::filesystem::path& /*dumpPath*/, void* /*platformContext*/)
    {
        // Linux: could produce a core dump via prctl + abort, or use breakpad.
        // Not implemented yet.
        return false;
    }

    std::string CollectSystemInfo()
    {
        return "OS: Linux (details unavailable in this build)\n";
    }

    void ShowCrashDialog(const std::string& /*title*/, const std::string& /*message*/, bool /*isHeadless*/)
    {
        // No GUI dialog on Linux for now — crash report is written to the log directory.
    }

    void EmitToDebugOutput(const std::string& /*message*/)
    {
        // No Linux equivalent of OutputDebugStringA; logs already go to stderr.
    }

} // namespace OloEngine::CrashReporterPlatform

#endif // OLO_PLATFORM_LINUX
