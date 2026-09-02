// Linux implementation of BuildPipelinePlatform.
// Icon embedding in ELF binaries is not currently supported; Linux desktop
// environments pick up an application's icon and launch command from a
// freedesktop.org .desktop entry instead (#891), which this file writes.

#include "OloEnginePCH.h"
#include "OloEngine/Build/BuildPipelinePlatform.h"
#include "OloEngine/Build/LinuxDesktopLauncher.h"

#ifdef OLO_PLATFORM_LINUX

#include "OloEngine/Core/Log.h"

namespace OloEngine::BuildPipelinePlatform
{
    bool EmbedCustomIcon(const std::filesystem::path& /*exePath*/,
                         const std::filesystem::path& /*iconPath*/,
                         std::string& outError)
    {
        outError = "Icon embedding is not supported on Linux";
        return false;
    }

    bool WriteDesktopEntry(const std::filesystem::path& exePath,
                           const std::filesystem::path& iconPath,
                           const std::string& gameName,
                           std::string& outError)
    {
        LinuxDesktopLauncherArtifacts artifacts;
        if (!StageLinuxDesktopLauncher(exePath, iconPath, gameName, artifacts, outError))
        {
            return false;
        }

        // Many desktop environments only treat a .desktop file as "trusted"
        // (rather than prompting "Untrusted application launcher") once it is
        // itself marked executable.
        std::error_code desktopPermEc;
        std::filesystem::permissions(artifacts.DesktopEntryPath,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, desktopPermEc);
        if (desktopPermEc)
        {
            OLO_CORE_WARN("[GameBuild] Failed to mark {} executable: {}", artifacts.DesktopEntryPath.string(), desktopPermEc.message());
        }

        desktopPermEc.clear();
        std::filesystem::permissions(artifacts.WrapperScriptPath,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, desktopPermEc);
        if (desktopPermEc)
        {
            OLO_CORE_WARN("[GameBuild] Failed to mark {} executable: {}", artifacts.WrapperScriptPath.string(), desktopPermEc.message());
        }

        OLO_CORE_INFO("[GameBuild] Linux desktop entry written: {}", artifacts.DesktopEntryPath.string());
        return true;
    }

} // namespace OloEngine::BuildPipelinePlatform

#endif // OLO_PLATFORM_LINUX
