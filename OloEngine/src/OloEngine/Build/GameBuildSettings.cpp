#include "OloEnginePCH.h"
#include "GameBuildSettings.h"

namespace OloEngine
{
    const char* ToString(BuildTargetPlatform platform)
    {
        switch (platform)
        {
            case BuildTargetPlatform::Windows:
                return "Windows";
            case BuildTargetPlatform::Linux:
                return "Linux";
        }
        return "Unknown";
    }

    BuildTargetPlatform GetHostBuildPlatform()
    {
#if defined(OLO_PLATFORM_WINDOWS)
        return BuildTargetPlatform::Windows;
#elif defined(OLO_PLATFORM_LINUX)
        return BuildTargetPlatform::Linux;
#else
#error "GetHostBuildPlatform: unhandled platform — add a BuildTargetPlatform arm before porting the build pipeline here."
#endif
    }

    bool IsBuildTargetSupportedOnThisHost(BuildTargetPlatform platform)
    {
        // No cross-compilation toolchain: the pipeline copies the host's own
        // OloRuntime binary, Mono runtime and script assemblies, so it can
        // only ever produce a build for the platform it is running on.
        return platform == GetHostBuildPlatform();
    }

    std::string GetHostExecutableFileName(const std::string& baseName, BuildTargetPlatform platform)
    {
        return platform == BuildTargetPlatform::Windows ? baseName + ".exe" : baseName;
    }

    bool IsScriptingAvailableOnPlatform(BuildTargetPlatform platform)
    {
        return platform == BuildTargetPlatform::Windows;
    }

} // namespace OloEngine
