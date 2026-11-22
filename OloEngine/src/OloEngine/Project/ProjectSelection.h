#pragma once

#include <filesystem>
#include <string_view>

namespace OloEngine
{
    struct ProjectLaunchArguments
    {
        std::filesystem::path Project;
        std::filesystem::path Scene;
        std::string_view MissingValueFor;
    };

    enum class ProjectSelectionSource
    {
        Explicit,
        Scene,
        Recent,
        SandboxFallback,
        Ambiguous
    };

    struct ProjectSelection
    {
        std::filesystem::path Path;
        ProjectSelectionSource Source;
    };

    [[nodiscard]] ProjectLaunchArguments ParseProjectLaunchArguments(int argc, char* const* argv);

    // A selected path is either a descriptor or, for legacy server launches,
    // an Assets/ project directory without a descriptor. An ambiguous lookup logs
    // the conflict and leaves Path empty.
    [[nodiscard]] ProjectSelection ResolveProjectSelection(
        const ProjectLaunchArguments& args, const std::filesystem::path& workingDirectory,
        const std::filesystem::path& recentProject = {}, bool allowLegacyDirectory = false);
} // namespace OloEngine
