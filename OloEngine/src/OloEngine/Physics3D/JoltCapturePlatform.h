// Platform hooks for JoltCaptureManager — provides OS-appropriate default
// capture output paths and the set of directories the user is allowed to
// redirect captures into (user-data locations).

#pragma once

#include <filesystem>
#include <vector>

namespace OloEngine::JoltCapturePlatform
{
    struct CapturePaths
    {
        std::filesystem::path CapturesPath; // Default output directory
        std::filesystem::path ExpectedRoot; // Root the CapturesPath is anchored to
    };

    /// Determine the default directory in which Jolt capture files should be written.
    /// Falls back to `<current_path>/Captures` when platform-specific env vars are absent.
    CapturePaths GetDefaultCapturePaths();

    /// Return platform-specific canonical base directories in which a user-redirected
    /// capture directory is considered safe (in addition to the current working dir).
    std::vector<std::filesystem::path> GetAllowedBasePaths();

} // namespace OloEngine::JoltCapturePlatform
