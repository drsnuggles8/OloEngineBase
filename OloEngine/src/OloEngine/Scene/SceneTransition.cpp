#include "OloEnginePCH.h"
#include "OloEngine/Scene/SceneTransition.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <vector>

namespace OloEngine::SceneTransition
{
    namespace
    {
        // Lowercased extension compare — scene files authored on Windows are
        // routinely ".OLO" after a copy through a case-insensitive tool.
        [[nodiscard]] std::string LowercaseExtension(const std::filesystem::path& path)
        {
            std::string ext = path.extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        [[nodiscard]] bool IsExistingSceneFile(const std::filesystem::path& candidate)
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(candidate, ec) && IsSceneFileExtension(candidate);
        }

        // A request may omit the extension ("Level2"). Produce the spellings to
        // try, most specific first, without inventing one for a request that
        // already names a scene file.
        [[nodiscard]] std::vector<std::filesystem::path> CandidateNames(const std::filesystem::path& request)
        {
            std::vector<std::filesystem::path> names;
            names.push_back(request);
            if (!IsSceneFileExtension(request))
            {
                std::filesystem::path withExt = request;
                withExt += ".olo";
                names.push_back(withExt);
            }
            return names;
        }
    } // namespace

    bool IsSceneFileExtension(const std::filesystem::path& path)
    {
        const std::string ext = LowercaseExtension(path);
        return ext == ".olo" || ext == ".scene";
    }

    std::filesystem::path ResolveScenePath(std::string_view request, const std::filesystem::path& rootDirectory)
    {
        if (request.empty())
        {
            return {};
        }

        const std::filesystem::path requested(request);

        // Refuse to climb out of the game's data directory. Scene requests come
        // from script source, and no legitimate one needs to leave it.
        //
        // Absolute is checked FIRST and separately from "..": appending an
        // absolute path REPLACES the left operand ([fs.path.append]), so
        // `root / "C:/anywhere/x.olo"` is just `C:/anywhere/x.olo` — an absolute
        // request would sail past the "..' scan below and escape the root
        // anyway, making that scan decorative.
        if (requested.is_absolute() || requested.has_root_name())
        {
            OLO_CORE_WARN("[SceneTransition] Rejecting scene request '{}': absolute paths are not allowed; "
                          "name a scene relative to the game's scene directory.",
                          std::string(request));
            return {};
        }

        for (const auto& part : requested)
        {
            if (part == "..")
            {
                OLO_CORE_WARN("[SceneTransition] Rejecting scene request '{}': parent-directory components are not allowed.",
                              std::string(request));
                return {};
            }
        }

        // An empty root means "the current working directory" — that is where a
        // shipped game's data lives.
        const std::filesystem::path root = rootDirectory.empty() ? std::filesystem::path(".") : rootDirectory;
        const auto names = CandidateNames(requested);

        // 1./2. Under the root, then under the root's Scenes/ subdirectory.
        //       A shipped game keeps its scenes under <game>/Scenes; the editor
        //       keeps them under <project>/Assets/Scenes.
        for (const auto& base : { root, root / "Scenes" })
        {
            for (const auto& name : names)
            {
                if (auto candidate = base / name; IsExistingSceneFile(candidate))
                {
                    return candidate.lexically_normal();
                }
            }
        }

        // 3. As given, relative to the working directory.
        for (const auto& name : names)
        {
            if (IsExistingSceneFile(name))
            {
                return name.lexically_normal();
            }
        }

        // 4. Last resort: find a matching file name anywhere under Scenes/.
        //    Sorted so a project with two same-named scenes in different
        //    subdirectories resolves identically on every machine and run.
        //
        //    Driven by an explicit increment(ec) loop rather than a range-for:
        //    recursive_directory_iterator's operator++ (and directory_entry's
        //    throwing is_regular_file) raise filesystem_error on an unreadable
        //    subdirectory, and this runs on the game thread while servicing a
        //    script's scene switch — an escaping exception there takes the game
        //    down over a permissions quirk in some unrelated folder. A directory
        //    we cannot walk simply ends the search with whatever matched so far.
        const auto sceneRoot = root / "Scenes";
        std::error_code ec;
        if (std::filesystem::is_directory(sceneRoot, ec))
        {
            std::vector<std::filesystem::path> matches;
            std::filesystem::recursive_directory_iterator iter(sceneRoot, ec);
            const std::filesystem::recursive_directory_iterator end;
            if (ec)
            {
                OLO_CORE_WARN("[SceneTransition] Could not scan '{}' for scenes: {}", sceneRoot.string(), ec.message());
            }
            for (; !ec && iter != end; iter.increment(ec))
            {
                if (!iter->is_regular_file(ec) || ec || !IsSceneFileExtension(iter->path()))
                {
                    ec.clear();
                    continue;
                }
                for (const auto& name : names)
                {
                    if (iter->path().filename() == name.filename())
                    {
                        matches.push_back(iter->path());
                        break;
                    }
                }
            }
            if (ec)
            {
                OLO_CORE_WARN("[SceneTransition] Scene scan of '{}' stopped early: {}", sceneRoot.string(), ec.message());
            }
            if (!matches.empty())
            {
                std::ranges::sort(matches);
                return matches.front().lexically_normal();
            }
        }

        return {};
    }

    LoadResult LoadSceneFile(const std::filesystem::path& path, bool requirePrimaryCamera, std::string_view saveSlot)
    {
        LoadResult result;

        if (path.empty())
        {
            result.Error = "no scene path given";
            return result;
        }

        if (!IsSceneFileExtension(path))
        {
            result.Error = "'" + path.string() + "' is not a scene file (.olo/.scene)";
            return result;
        }

        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
        {
            result.Error = "scene file not found: " + path.string();
            return result;
        }

        Ref<Scene> scene = Scene::Create();
        if (SceneSerializer serializer(scene); !serializer.Deserialize(path))
        {
            result.Error = "failed to deserialize scene: " + path.string();
            return result;
        }

        if (requirePrimaryCamera && !scene->GetPrimaryCameraEntity())
        {
            result.Error = "scene '" + path.string() +
                           "' has no entity with a primary CameraComponent — it would render nothing. "
                           "Add one in the editor and rebuild.";
            return result;
        }

        if (!saveSlot.empty())
        {
            if (const auto loadResult = SaveGameManager::Load(*scene, std::string(saveSlot));
                loadResult != SaveLoadResult::Success)
            {
                result.Error = "failed to restore save slot '" + std::string(saveSlot) + "' into scene '" +
                               path.string() + "' (result " + std::to_string(static_cast<int>(loadResult)) + ")";
                return result;
            }
        }

        result.LoadedScene = scene;
        return result;
    }
} // namespace OloEngine::SceneTransition
