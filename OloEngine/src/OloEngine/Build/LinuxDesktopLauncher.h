#pragma once

#include <filesystem>
#include <string>

namespace OloEngine
{
    struct LinuxDesktopLauncherArtifacts
    {
        std::filesystem::path DesktopEntryPath;
        std::filesystem::path WrapperScriptPath;
        std::filesystem::path PackagedIconPath;
    };

    /// Stage a portable Linux launcher without embedding authoring-machine paths.
    /// The root desktop entry uses `%k` to locate the packaged wrapper after a
    /// move; that wrapper refreshes the entry's packaged-icon path.
    /// The Linux platform hook adds executable permissions after staging.
    [[nodiscard]] bool StageLinuxDesktopLauncher(
        const std::filesystem::path& executablePath,
        const std::filesystem::path& iconPath,
        const std::string& gameName,
        LinuxDesktopLauncherArtifacts& artifacts,
        std::string& errorMessage);
} // namespace OloEngine
