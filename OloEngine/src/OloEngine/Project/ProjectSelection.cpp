#include "OloEnginePCH.h"

#include "OloEngine/Project/ProjectSelection.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace OloEngine
{
    namespace
    {
        namespace fs = std::filesystem;

        bool HasExtension(const fs::path& path, std::string_view extension)
        {
            return std::ranges::equal(path.extension().string(), extension, [](unsigned char actual, char expected)
                                      { return std::tolower(actual) == expected; });
        }

        bool IsProjectFile(const fs::path& path)
        {
            std::error_code error;
            return HasExtension(path, ".oloproj") && fs::is_regular_file(path, error);
        }

        fs::path ProjectInDirectory(const fs::path& directory)
        {
            const auto named = directory / (directory.filename().native() + fs::path(".oloproj").native());
            if (IsProjectFile(named))
                return named;

            // A directory with multiple descriptors needs an explicit choice.
            fs::path candidate;
            std::error_code error;
            for (fs::directory_iterator entry(directory, error), end; entry != end && !error; entry.increment(error))
            {
                if (!IsProjectFile(entry->path()))
                    continue;
                if (!candidate.empty())
                    return {};
                candidate = entry->path();
            }
            return error ? fs::path{} : candidate;
        }

        fs::path InferProject(const fs::path& scene, bool allowLegacyDirectory)
        {
            for (auto directory = scene.parent_path(); !directory.empty(); directory = directory.parent_path())
            {
                if (auto descriptor = ProjectInDirectory(directory); !descriptor.empty())
                    return descriptor;
                if (allowLegacyDirectory && directory.filename() == "Assets")
                {
                    auto root = directory.parent_path();
                    if (auto descriptor = ProjectInDirectory(root); !descriptor.empty())
                        return descriptor;
                    std::error_code error;
                    if (fs::is_directory(directory, error))
                        return root;
                }
                if (directory == directory.root_path())
                    break;
            }
            return {};
        }

        fs::path FromWorkingDirectory(const fs::path& path, const fs::path& workingDirectory)
        {
            return (workingDirectory / path).lexically_normal();
        }
    } // namespace

    ProjectLaunchArguments ParseProjectLaunchArguments(int argc, char* const* argv)
    {
        ProjectLaunchArguments args;
        fs::path positionalProject;
        fs::path positionalScene;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view token = argv[i];
            if (token == "--project" || token == "--scene")
            {
                if (i + 1 == argc || std::string_view(argv[i + 1]).empty() ||
                    std::string_view(argv[i + 1]).starts_with('-'))
                {
                    args.MissingValueFor = token == "--project" ? "--project" : "--scene";
                    return args;
                }
                auto& argument = token == "--project" ? args.Project : args.Scene;
                argument = argv[++i];
            }
            else if (token == "--config" || token == "--port" || token == "--max-players" ||
                     token == "--tick-rate" || token == "--snapshot-rate" || token == "--rhi" || token == "--set")
            {
                if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with('-'))
                    ++i;
            }
            else if (!token.starts_with('-'))
            {
                const fs::path path(token);
                if (positionalProject.empty() && HasExtension(path, ".oloproj"))
                    positionalProject = path;
                else if (positionalScene.empty() && HasExtension(path, ".olo"))
                    positionalScene = path;
            }
        }
        if (args.Project.empty())
            args.Project = std::move(positionalProject);
        if (args.Scene.empty())
            args.Scene = std::move(positionalScene);
        return args;
    }

    ProjectSelection ResolveProjectSelection(const ProjectLaunchArguments& args, const std::filesystem::path& workingDirectory,
                                             const std::filesystem::path& recentProject, bool allowLegacyDirectory)
    {
        if (!args.Project.empty())
        {
            auto path = FromWorkingDirectory(args.Project, workingDirectory);
            std::error_code error;
            if (allowLegacyDirectory && fs::is_directory(path, error))
            {
                if (!path.has_filename() && path != path.root_path())
                    path = path.parent_path();
                if (auto descriptor = ProjectInDirectory(path); !descriptor.empty())
                    path = std::move(descriptor);
            }
            return { std::move(path), ProjectSelectionSource::Explicit };
        }
        if (!args.Scene.empty())
        {
            if (auto descriptor = InferProject(FromWorkingDirectory(args.Scene, workingDirectory), allowLegacyDirectory); !descriptor.empty())
                return { std::move(descriptor), ProjectSelectionSource::Scene };
        }
        if (!recentProject.empty())
        {
            auto path = FromWorkingDirectory(recentProject, workingDirectory);
            if (IsProjectFile(path))
                return { std::move(path), ProjectSelectionSource::Recent };
        }
        return { workingDirectory / "SandboxProject" / "Sandbox.oloproj", ProjectSelectionSource::SandboxFallback };
    }
} // namespace OloEngine
