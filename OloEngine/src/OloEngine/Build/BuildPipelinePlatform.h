// Platform hooks for GameBuildPipeline — executable post-processing that
// requires OS-specific APIs (PE resource updates on Windows, objcopy/similar
// on Linux in the future).

#pragma once

#include <filesystem>
#include <string>

namespace OloEngine::BuildPipelinePlatform
{
    /// Embed a .ico / platform-native icon resource into an executable file.
    /// Returns true on success, populates `outError` on failure.
    /// On platforms without an implementation, returns false and sets an error.
    bool EmbedCustomIcon(const std::filesystem::path& exePath,
                         const std::filesystem::path& iconPath,
                         std::string& outError);

    /// Write a freedesktop.org `.desktop` launcher entry next to `exePath`
    /// (#891) — the Linux arm of icon handling. Linux has no PE-resource slot
    /// to embed an icon into, so instead of the Windows-only nicety silently
    /// vanishing on Linux, the pipeline emits a relocatable launcher and stages
    /// the optional Linux-native icon inside the package.
    /// Returns true on success, populates `outError` on failure.
    /// On platforms without an implementation, returns false and sets an error.
    bool WriteDesktopEntry(const std::filesystem::path& exePath,
                           const std::filesystem::path& iconPath,
                           const std::string& gameName,
                           std::string& outError);

} // namespace OloEngine::BuildPipelinePlatform
