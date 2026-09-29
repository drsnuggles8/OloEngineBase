#include "OloEnginePCH.h"

#include "OloEngine/Project/ContentPath.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Project/ProjectSerializer.h"

#include <system_error>

namespace OloEngine
{
    namespace
    {
        // True when `path` begins with every component of `prefix`, compared
        // exactly (case-sensitively) on every platform. The spelling of the first
        // component is what picks the base, so "assets/..." must not match an
        // "Assets" asset directory even on a case-insensitive filesystem.
        bool StartsWithComponents(const std::filesystem::path& path, const std::filesystem::path& prefix)
        {
            auto it = path.begin();
            bool any = false;
            for (const std::filesystem::path& component : prefix)
            {
                if (component.empty() || component == ".")
                    continue;
                if (it == path.end() || it->generic_string() != component.generic_string())
                    return false;
                ++it;
                any = true;
            }
            return any;
        }
    } // namespace

    std::filesystem::path ResolveContentPath(std::string_view storedPath)
    {
        if (storedPath.empty())
            return {};

        const std::filesystem::path path(storedPath);
        if (!path.is_relative())
            return path;

        // The path's own spelling picks exactly ONE base; nothing is probed. Probing
        // both bases was ambiguous: on a case-insensitive filesystem the engine path
        // "assets/models/CesiumMan/CesiumMan.gltf" (OloEditor/assets/...) also names
        // SandboxProject/Assets/Models/CesiumMan/..., and the base tried first won.
        //   - "<AssetDirectory>/..." (exact case, e.g. "Assets/...") -> the project directory;
        //   - "../..." -> the project directory: the asset registry keys content outside
        //     the project that way ("../assets/textures/pbr/wall/albedo.png");
        //   - anything else ("assets/...", "Resources/...") -> the working directory.
        const Ref<Project> project = Project::GetActive();
        // ProjectSerializer stores AssetDirectory canonicalised to an absolute path, so
        // compare the stored path against its spelling RELATIVE to the project ("Assets").
        // std::filesystem::relative canonicalises both sides, so a drive-letter or case
        // difference between the two absolute spellings does not break the match.
        std::filesystem::path assetDirectory;
        if (project)
        {
            assetDirectory = project->GetConfig().AssetDirectory;
            if (assetDirectory.is_absolute())
            {
                std::error_code relativeError;
                assetDirectory = std::filesystem::relative(assetDirectory, project->GetDirectory(), relativeError);
                if (relativeError)
                    assetDirectory.clear();
            }
        }
        const bool projectRooted =
            project && (StartsWithComponents(path, assetDirectory) || *path.begin() == "..");

        std::filesystem::path resolved;
        std::string base;
        if (projectRooted)
        {
            resolved = project->GetDirectory() / path;
            base = "the active project directory '" + project->GetDirectory().string() + "'";
        }
        else
        {
            resolved = path;
            std::error_code cwdError;
            base = "the working directory '" + std::filesystem::current_path(cwdError).string() + "'";
            if (!project && *path.begin() == "Assets")
                base += " (no project is open to resolve an 'Assets/...' path against)";
        }

        std::error_code ec;
        if (std::filesystem::exists(resolved, ec) && !ec)
            return resolved;

        // Not under the one base the path names. Say so and refuse: handing the
        // relative path to a loader anyway would read it against whatever the
        // working directory is, the silent failure this helper exists to remove
        // (#1067, #1496).
        OLO_CORE_ERROR("ResolveContentPath: '{}' does not exist under {}; refusing to open it. Project content is "
                       "spelled '<AssetDirectory>/...' (e.g. 'Assets/...'), engine content 'assets/...' relative to "
                       "the working directory",
                       storedPath, base);
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
