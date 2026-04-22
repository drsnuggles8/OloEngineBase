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

} // namespace OloEngine::BuildPipelinePlatform
