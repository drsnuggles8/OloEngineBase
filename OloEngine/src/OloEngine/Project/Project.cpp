#include "OloEnginePCH.h"

#include "OloEngine/Project/ContentPath.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Project/ProjectSerializer.h"

#include <system_error>

namespace OloEngine
{
    std::filesystem::path ResolveContentPath(std::string_view storedPath)
    {
        if (storedPath.empty())
            return {};

        std::filesystem::path path(storedPath);
        if (!path.is_relative())
            return path;

        // Project first: content authored inside a project is spelled relative
        // to it. Then the working directory, which is where engine content
        // ("assets/...", under OloEditor/) lives. Only a file that is really
        // there wins, so neither base can shadow a real file in the other.
        std::error_code ec;
        const bool haveProject = Project::GetActive() != nullptr;
        if (haveProject)
        {
            if (std::filesystem::path projectRooted = Project::GetProjectDirectory() / path;
                std::filesystem::exists(projectRooted, ec) && !ec)
            {
                return projectRooted;
            }
        }

        ec.clear();
        if (std::filesystem::exists(path, ec) && !ec)
            return path;

        // Neither base has the file. Say so and refuse — handing the relative path
        // to the loader anyway would read it against the working directory, which
        // is the silent failure this helper exists to remove (#1067, #1496).
        OLO_CORE_ERROR("ResolveContentPath: cannot resolve relative content path '{}' ({}, nor under the working "
                       "directory); refusing to open it",
                       storedPath,
                       haveProject ? "not found under the active project directory" : "no active project");
        return {};
    }

    Ref<Project> Project::New()
    {
        s_ActiveProject = Ref<Project>::Create();
        return s_ActiveProject;
    }

    Ref<Project> Project::NewInMemory(const std::filesystem::path& directory, const ProjectConfig& config)
    {
        Ref<Project> project = Ref<Project>::Create();
        project->m_Config = config;
        project->m_ProjectDirectory = directory;
        s_ActiveProject = project;
        return s_ActiveProject;
    }

    Ref<Project> Project::Load(const std::filesystem::path& path)
    {
        Ref<Project> project = Ref<Project>::Create();

        if (ProjectSerializer serializer(project); serializer.Deserialize(path))
        {
            project->m_ProjectDirectory = path.parent_path();
            s_ActiveProject = project;
            return s_ActiveProject;
        }

        return nullptr;
    }

    bool Project::SaveActive(const std::filesystem::path& path)
    {
        if (ProjectSerializer serializer(s_ActiveProject); serializer.Serialize(path))
        {
            s_ActiveProject->m_ProjectDirectory = path.parent_path();
            return true;
        }

        return false;
    }
} // namespace OloEngine
