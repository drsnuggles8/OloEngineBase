#include "OloEnginePCH.h"
#include "GameBuildPipeline.h"

#include "OloEngine/Asset/AssetPackBuilder.h"
#include "OloEngine/Build/BuildPipelinePlatform.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Core/BuildInfo.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Profiler.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/BackendSelection.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <sstream>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] std::string LowerExtension(const std::filesystem::path& path)
        {
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return extension;
        }
    } // namespace

    bool StageManagedAssembly(
        const std::filesystem::path& source,
        const std::filesystem::path& destination,
        std::string& errorMessage)
    {
        errorMessage.clear();
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (!ec)
        {
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        }
        if (ec)
        {
            errorMessage = "Failed to stage managed assembly " + source.string() + ": " + ec.message();
            return false;
        }

        auto sourcePdb = source;
        sourcePdb.replace_extension(".pdb");
        auto destinationPdb = destination;
        destinationPdb.replace_extension(".pdb");
        const bool hasSymbols = std::filesystem::exists(sourcePdb, ec);
        if (!ec)
        {
            if (hasSymbols)
            {
                std::filesystem::copy_file(sourcePdb, destinationPdb, std::filesystem::copy_options::overwrite_existing, ec);
            }
            else
            {
                std::filesystem::remove(destinationPdb, ec);
            }
        }
        if (ec)
        {
            errorMessage = "Failed to stage managed symbols " + sourcePdb.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    bool StageRuntimeDependencyLibraries(
        BuildTargetPlatform targetPlatform,
        const std::filesystem::path& runtimeBinDir,
        const std::filesystem::path& outputDir,
        sizet& copiedCount,
        std::string& errorMessage)
    {
        copiedCount = 0;
        if (targetPlatform != BuildTargetPlatform::Windows)
        {
            return true;
        }

        std::error_code ec;
        std::vector<std::filesystem::path> dependencies;
        for (std::filesystem::directory_iterator it(runtimeBinDir, ec), end; it != end && !ec; it.increment(ec))
        {
            const bool isRegularFile = it->is_regular_file(ec);
            if (ec)
            {
                break;
            }
            if (!isRegularFile)
            {
                continue;
            }

            const auto extension = LowerExtension(it->path());
            if (extension == ".dll")
            {
                dependencies.push_back(it->path());
            }
        }
        if (ec)
        {
            errorMessage = "Failed to enumerate runtime dependencies in " + runtimeBinDir.string() + ": " + ec.message();
            return false;
        }

        std::ranges::sort(dependencies);
        for (const auto& srcDll : dependencies)
        {
            std::filesystem::copy_file(srcDll, outputDir / srcDll.filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                errorMessage = "Failed to copy runtime dependency " + srcDll.filename().string() + ": " + ec.message();
                return false;
            }
            ++copiedCount;
        }

        return true;
    }

    bool StageLooseRuntimeTextures(
        const std::filesystem::path& projectAssetsDir,
        const std::filesystem::path& outputAssetsDir,
        sizet& copiedCount,
        std::string& errorMessage)
    {
        copiedCount = 0;

        static const std::unordered_set<std::string> textureExtensions = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp"
        };

        std::error_code ec;
        std::vector<std::filesystem::path> textures;
        for (std::filesystem::recursive_directory_iterator it(projectAssetsDir, ec), end;
             it != end && !ec; it.increment(ec))
        {
            const bool isRegularFile = it->is_regular_file(ec);
            if (ec)
            {
                break;
            }
            if (!isRegularFile)
            {
                continue;
            }

            const auto extension = LowerExtension(it->path());
            if (textureExtensions.contains(extension))
            {
                textures.push_back(it->path());
            }
        }
        if (ec)
        {
            errorMessage = "Failed to enumerate loose project textures in " + projectAssetsDir.string() + ": " + ec.message();
            return false;
        }

        std::ranges::sort(textures);
        for (const auto& texture : textures)
        {
            const auto relative = std::filesystem::relative(texture, projectAssetsDir, ec);
            if (ec || relative.empty() || relative.generic_string().starts_with(".."))
            {
                errorMessage = "Failed to make project texture path relative: " + texture.string();
                return false;
            }

            const auto destination = outputAssetsDir / relative;
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (ec)
            {
                errorMessage = "Failed to create runtime texture directory: " + ec.message();
                return false;
            }

            std::filesystem::copy_file(texture, destination,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                errorMessage = "Failed to copy loose runtime texture " + relative.generic_string() + ": " + ec.message();
                return false;
            }
            ++copiedCount;
        }

        return true;
    }

    namespace
    {
        // "%20" -> ' '. glTF URIs are URI-encoded; anything malformed is left as typed.
        [[nodiscard]] std::string DecodeUriPath(const std::string& uri)
        {
            std::string decoded;
            decoded.reserve(uri.size());
            for (sizet i = 0; i < uri.size(); ++i)
            {
                if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(uri[i + 2])))
                {
                    decoded += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else
                {
                    decoded += uri[i];
                }
            }
            return decoded;
        }

        [[nodiscard]] std::string Trim(const std::string& text)
        {
            const auto begin = text.find_first_not_of(" \t\r");
            if (begin == std::string::npos)
                return {};
            const auto end = text.find_last_not_of(" \t\r");
            return text.substr(begin, end - begin + 1);
        }

        struct Token
        {
            std::string Text;
            sizet At = 0;
        };

        [[nodiscard]] std::vector<Token> Tokenise(const std::string& text)
        {
            std::vector<Token> tokens;
            sizet at = 0;
            while ((at = text.find_first_not_of(" \t", at)) != std::string::npos)
            {
                sizet end = text.find_first_of(" \t", at);
                if (end == std::string::npos)
                    end = text.size();
                tokens.push_back({ text.substr(at, end - at), at });
                at = end;
            }
            return tokens;
        }

        [[nodiscard]] bool IsNumber(const std::string& token)
        {
            return !token.empty() && token.find_first_not_of("0123456789+-.eE") == std::string::npos &&
                   token.find_first_of("0123456789") != std::string::npos;
        }

        // The file an .mtl map statement names, once its options are skipped.
        // Options and their argument counts are the MTL spec's. A filename may
        // contain spaces, so the name is the rest of the line; when that names
        // nothing on disk and the last token does, the last token wins, since
        // the directory is the only arbiter of an ambiguous line.
        [[nodiscard]] std::string MapStatementFile(const std::string& rest, const std::filesystem::path& directory)
        {
            static const std::unordered_map<std::string, std::pair<int, int>> kOptionArgs = {
                { "-blendu", { 1, 1 } },
                { "-blendv", { 1, 1 } },
                { "-bm", { 1, 1 } },
                { "-boost", { 1, 1 } },
                { "-cc", { 1, 1 } },
                { "-clamp", { 1, 1 } },
                { "-imfchan", { 1, 1 } },
                { "-mm", { 2, 2 } },
                { "-o", { 1, 3 } },
                { "-s", { 1, 3 } },
                { "-t", { 1, 3 } },
                { "-texres", { 1, 1 } },
                { "-type", { 1, 1 } },
            };
            const std::vector<Token> tokens = Tokenise(rest);
            sizet i = 0;
            while (i < tokens.size() && tokens[i].Text.size() > 1 && tokens[i].Text[0] == '-' && !IsNumber(tokens[i].Text))
            {
                std::string option = tokens[i].Text;
                std::ranges::transform(option, option.begin(), [](unsigned char c)
                                       { return static_cast<char>(std::tolower(c)); });
                ++i;
                if (const auto known = kOptionArgs.find(option); known != kOptionArgs.end())
                {
                    int taken = 0;
                    for (; taken < known->second.first && i < tokens.size(); ++taken)
                        ++i;
                    for (; taken < known->second.second && i < tokens.size() && IsNumber(tokens[i].Text); ++taken)
                        ++i;
                }
                else
                {
                    while (i < tokens.size() && IsNumber(tokens[i].Text))
                        ++i;
                }
            }
            if (i >= tokens.size())
                return {};
            const std::string whole = Trim(rest.substr(tokens[i].At));
            std::error_code ec;
            if (!std::filesystem::exists(directory / whole, ec) && std::filesystem::exists(directory / tokens.back().Text, ec))
                return tokens.back().Text;
            return whole;
        }

        // The files `file` opens by itself, as paths relative to its own directory.
        [[nodiscard]] std::vector<std::string> ReadContentDependencies(const std::filesystem::path& file)
        {
            std::vector<std::string> dependencies;
            const std::string extension = LowerExtension(file);
            const std::filesystem::path directory = file.parent_path();
            if (extension == ".obj" || extension == ".mtl")
            {
                // An .obj names its material libraries with `mtllib`; an .mtl names
                // each texture in a map statement, after its options.
                static const std::unordered_set<std::string> mapStatements = {
                    "map_ka", "map_kd", "map_ks", "map_ke", "map_ns", "map_d", "map_bump", "bump", "norm",
                    "disp", "decal", "refl", "map_pr", "map_pm", "map_ps", "map_rma", "map_orm"
                };
                std::ifstream input(file);
                std::string line;
                while (std::getline(input, line))
                {
                    const std::string trimmed = Trim(line);
                    const auto split = trimmed.find_first_of(" \t");
                    if (split == std::string::npos)
                        continue;
                    std::string statement = trimmed.substr(0, split);
                    std::ranges::transform(statement, statement.begin(), [](unsigned char c)
                                           { return static_cast<char>(std::tolower(c)); });
                    const std::string rest = Trim(trimmed.substr(split + 1));
                    if (extension == ".obj" && statement == "mtllib")
                    {
                        // One library whose name has spaces, or several on one
                        // line (the OBJ spec allows both); the directory decides.
                        std::error_code ec;
                        if (std::filesystem::exists(directory / rest, ec))
                            dependencies.push_back(rest);
                        else
                            for (const auto& token : Tokenise(rest))
                                dependencies.push_back(token.Text);
                    }
                    else if (extension == ".mtl" && mapStatements.contains(statement))
                    {
                        if (std::string named = MapStatementFile(rest, directory); !named.empty())
                            dependencies.push_back(std::move(named));
                    }
                }
            }
            else if (extension == ".gltf")
            {
                std::ifstream input(file);
                const nlohmann::json document = nlohmann::json::parse(input, nullptr, /*allow_exceptions=*/false);
                if (document.is_object())
                {
                    for (const char* array : { "buffers", "images" })
                    {
                        const auto it = document.find(array);
                        if (it == document.end() || !it->is_array())
                            continue;
                        for (const auto& entry : *it)
                        {
                            if (!entry.is_object() || !entry.contains("uri") || !entry["uri"].is_string())
                                continue;
                            const auto& uri = entry["uri"].get_ref<const std::string&>();
                            if (!uri.starts_with("data:"))
                                dependencies.push_back(DecodeUriPath(uri));
                        }
                    }
                }
            }
            return dependencies;
        }

        // True when `path` begins with every component of `prefix`, compared exactly
        // (case-sensitively): ResolveContentPath's own test for "<AssetDirectory>/...",
        // which may be more than one component ("Content/Assets").
        [[nodiscard]] bool StartsWithComponents(const std::filesystem::path& path, const std::filesystem::path& prefix)
        {
            auto it = path.begin();
            bool any = false;
            for (const auto& component : prefix)
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

        // True when `path` is relative, starts with `root` (an empty root is the
        // base directory itself) and has no `..` left after folding, i.e. it
        // cannot leave the directory it is staged into.
        [[nodiscard]] bool IsRootedUnder(const std::filesystem::path& path, const std::filesystem::path& root)
        {
            if (path.empty() || path.is_absolute() || (!root.empty() && !StartsWithComponents(path, root)))
                return false;
            return std::ranges::none_of(path, [](const std::filesystem::path& component)
                                        { return component == ".."; });
        }

        // Could this scalar name a file at all? Cheap, and it spares a stat for
        // each of the tens of thousands of numbers, flags and tags in a scene.
        [[nodiscard]] bool LooksLikeAPath(const std::string& value)
        {
            if (value.find_first_of("/.\\") == std::string::npos)
                return false;
            return value.find_first_not_of("0123456789+-.eE ") != std::string::npos;
        }

        [[nodiscard]] bool SameBytes(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            std::error_code ec;
            if (std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec) || ec)
                return false;
            std::ifstream fa(a, std::ios::binary);
            std::ifstream fb(b, std::ios::binary);
            return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(),
                              std::istreambuf_iterator<char>(fb));
        }
    } // namespace

    bool StageSceneReferencedContent(
        const std::vector<std::filesystem::path>& sceneFiles,
        const std::filesystem::path& projectDir,
        const std::filesystem::path& assetDirectoryName,
        const std::filesystem::path& engineRoot,
        const std::filesystem::path& outputDir,
        sizet& copiedCount,
        std::vector<std::string>& unresolved,
        std::string& errorMessage)
    {
        copiedCount = 0;

        // One stored path to stage: relative to `Base`, rooted at `Root`.
        struct Reference
        {
            std::filesystem::path Stored;
            std::filesystem::path Base;
            std::filesystem::path Root;
            std::string Referrer;
        };

        std::vector<Reference> pending;
        std::unordered_set<std::string> seen;
        const auto enqueue = [&](Reference reference)
        {
            if (seen.insert(reference.Stored.generic_string()).second)
                pending.push_back(std::move(reference));
        };
        // Once per line: a crowd scene names the same model in every entity.
        std::unordered_set<std::string> reported;
        const auto report = [&](std::string line)
        {
            if (reported.insert(line).second)
                unresolved.push_back(std::move(line));
        };

        // ResolveContentPath's spelling rule, applied to one scene scalar, plus
        // the asset-relative spelling some fields use instead.
        const auto consider = [&](const std::string& value, const std::string& sceneName)
        {
            if (value.empty() || value.size() > 1024 || value.find_first_of("\r\n") != std::string::npos)
                return;
            const std::filesystem::path stored = std::filesystem::path(value).lexically_normal();
            if (stored.empty() || stored == ".")
                return;
            const std::string first = stored.begin()->generic_string();
            if (stored.is_absolute())
            {
                std::error_code ec;
                if (std::filesystem::is_regular_file(stored, ec))
                    report(sceneName + ": " + value + " (an absolute path; a packaged game cannot carry it)");
                return;
            }
            if (first == "..")
            {
                // Present or not, a package has nowhere to put it: the runtime
                // would read it from beside the game directory.
                report(sceneName + ": " + value +
                       " (outside the project; a relocatable package has nowhere to put it)");
                return;
            }
            if (StartsWithComponents(stored, assetDirectoryName))
            {
                enqueue({ stored, projectDir, assetDirectoryName, sceneName });
                return;
            }
            if (first == "assets")
            {
                enqueue({ stored, engineRoot, "assets", sceneName });
                return;
            }
            if (!LooksLikeAPath(value))
                return;

            // Two more spellings resolve, and only a value naming a file that is
            // really there counts, so ordinary text is never taken for a path:
            //  - asset-directory-relative ("Audio/Wind.ogg"), how an audio source's
            //    Filepath is stored and read back as <AssetDirectory>/<value>,
            //    which is <game>/Assets/<value> in a packaged game;
            //  - any other relative spelling, which ResolveContentPath reads
            //    against the working directory ("Resources/..."), which is the
            //    game directory in a packaged game.
            std::error_code ec;
            const std::filesystem::path assetRelative = assetDirectoryName / stored;
            if (std::filesystem::exists(projectDir / assetRelative, ec))
                enqueue({ assetRelative, projectDir, assetDirectoryName, sceneName });
            if (std::filesystem::exists(engineRoot / stored, ec))
                enqueue({ stored, engineRoot, {}, sceneName }); // rooted at the working directory itself
        };

        for (const auto& sceneFile : sceneFiles)
        {
            const std::string sceneName = sceneFile.filename().string();
            YAML::Node document;
            try
            {
                document = YAML::LoadFile(sceneFile.string());
            }
            catch (const std::exception& e)
            {
                report(sceneName + ": the scene did not parse, so its references were not staged (" +
                       e.what() + ")");
                continue;
            }

            std::vector<YAML::Node> stack{ document };
            while (!stack.empty())
            {
                const YAML::Node node = stack.back();
                stack.pop_back();
                if (node.IsScalar())
                {
                    consider(node.Scalar(), sceneName);
                }
                else if (node.IsSequence())
                {
                    for (const auto& child : node)
                        stack.push_back(child);
                }
                else if (node.IsMap())
                {
                    for (const auto& entry : node)
                        stack.push_back(entry.second);
                }
            }
        }

        // Where each destination came from, keyed case-insensitively: "Assets/x"
        // and "assets/x" are ONE file in a Windows package, so two different
        // files staged there would overwrite each other without a word.
        std::unordered_map<std::string, std::filesystem::path> stagedFrom;
        const auto copyOne = [&](const std::filesystem::path& source, const std::filesystem::path& stored,
                                 const std::string& referrer) -> bool
        {
            std::string key = stored.generic_string();
            std::ranges::transform(key, key.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            if (const auto earlier = stagedFrom.find(key); earlier != stagedFrom.end())
            {
                if (!SameBytes(earlier->second, source))
                {
                    report(referrer + ": " + stored.generic_string() + " (collides with " +
                           earlier->second.generic_string() +
                           ", a different file at the same path once Assets/ and assets/ share a directory)");
                }
                return true;
            }
            stagedFrom.emplace(std::move(key), source);

            const auto destination = outputDir / stored;
            std::error_code ec;
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (ec)
            {
                errorMessage = "Failed to create the directory for scene-referenced content " + stored.generic_string() +
                               ": " + ec.message();
                return false;
            }
            std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                errorMessage = "Failed to copy scene-referenced content " + stored.generic_string() + ": " + ec.message();
                return false;
            }
            ++copiedCount;
            return true;
        };

        while (!pending.empty())
        {
            const Reference reference = std::move(pending.back());
            pending.pop_back();

            if (!IsRootedUnder(reference.Stored, reference.Root))
            {
                report(reference.Referrer + ": " + reference.Stored.generic_string() + " (leaves " +
                       reference.Root.generic_string() + "/)");
                continue;
            }

            const auto source = reference.Base / reference.Stored;
            std::error_code ec;
            if (std::filesystem::is_directory(source, ec))
            {
                // A folder reference (a cubemap's six faces). Never the root itself,
                // which would copy the whole content tree.
                const auto depth = std::distance(reference.Stored.begin(), reference.Stored.end());
                const auto rootDepth = std::distance(reference.Root.begin(), reference.Root.end());
                if (depth <= std::max<decltype(rootDepth)>(rootDepth, 1))
                    continue;
                std::vector<std::filesystem::path> files;
                for (std::filesystem::directory_iterator it(source, ec), end; it != end && !ec; it.increment(ec))
                {
                    // Its own error code: one unreadable entry must not end the
                    // enumeration or be reported as an iterator failure.
                    std::error_code entryEc;
                    if (it->is_regular_file(entryEc) && !entryEc)
                        files.push_back(it->path().filename());
                }
                if (ec)
                {
                    errorMessage = "Failed to enumerate scene-referenced folder " + reference.Stored.generic_string() +
                                   ": " + ec.message();
                    return false;
                }
                std::ranges::sort(files);
                for (const auto& name : files)
                {
                    if (!copyOne(source / name, reference.Stored / name, reference.Referrer))
                        return false;
                }
                continue;
            }
            if (!std::filesystem::is_regular_file(source, ec))
            {
                report(reference.Referrer + ": " + reference.Stored.generic_string() + " (no such file under " +
                       reference.Base.string() + ")");
                continue;
            }

            if (!copyOne(source, reference.Stored, reference.Referrer))
                return false;

            const std::string referrer = reference.Stored.generic_string();
            const auto enqueueSibling = [&](const std::filesystem::path& stored)
            {
                enqueue({ stored.lexically_normal(), reference.Base, reference.Root, referrer });
            };
            for (const auto& dependency : ReadContentDependencies(source))
                enqueueSibling(reference.Stored.parent_path() / std::filesystem::path(dependency));

            // Import settings travel with the file they configure. Optional, so
            // only a sidecar that exists is staged.
            std::filesystem::path importSettings = reference.Stored;
            importSettings += ".oloimport";
            if (std::filesystem::is_regular_file(reference.Base / importSettings, ec))
                enqueueSibling(importSettings);
        }

        return true;
    }

    bool StageShaderPack(
        const std::filesystem::path& shaderPackSrc,
        const std::filesystem::path& outputAssetsDir,
        bool& packStaged,
        std::string& errorMessage)
    {
        packStaged = false;

        std::error_code ec;
        const bool srcExists = std::filesystem::exists(shaderPackSrc, ec);
        if (ec)
        {
            errorMessage = "Failed to check for a shader pack at " + shaderPackSrc.string() + ": " + ec.message();
            return false;
        }
        if (!srcExists)
        {
            return true;
        }

        std::filesystem::create_directories(outputAssetsDir, ec);
        if (ec)
        {
            errorMessage = "Failed to create asset output directory for the shader pack: " + ec.message();
            return false;
        }

        std::filesystem::copy_file(shaderPackSrc, outputAssetsDir / shaderPackSrc.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMessage = "Failed to copy shader pack: " + ec.message();
            return false;
        }

        packStaged = true;
        return true;
    }

    GameBuildResult GameBuildPipeline::Build(
        const GameBuildSettings& settings,
        std::atomic<f32>& progress,
        const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        auto startTime = std::chrono::high_resolution_clock::now();

        GameBuildResult result;
        progress = 0.0f;

        // Fail loudly, before touching the filesystem, when asked for a target
        // this host cannot produce (#891). OloEngine has no cross-compilation
        // toolchain — every step below copies the HOST's own OloRuntime binary,
        // Mono runtime and script assemblies, so packaging for another platform
        // would produce a folder that looks complete but cannot run.
        if (!IsBuildTargetSupportedOnThisHost(settings.TargetPlatform))
        {
            result.ErrorMessage = "Cannot build for target platform '" + std::string(ToString(settings.TargetPlatform)) +
                                  "' on this host (host platform: " + ToString(GetHostBuildPlatform()) +
                                  "). OloEngine has no cross-compilation toolchain — build on a " +
                                  ToString(settings.TargetPlatform) + " host to produce a " +
                                  ToString(settings.TargetPlatform) + " distribution.";
            return result;
        }

        // Validate and sanitize GameName before using it to build paths
        {
            const auto& gameName = settings.GameName;
            if (gameName.empty())
            {
                result.ErrorMessage = "GameName cannot be empty";
                return result;
            }
            std::filesystem::path nameAsPath(gameName);
            if (nameAsPath.is_absolute() || gameName.find("..") != std::string::npos || gameName.find('/') != std::string::npos || gameName.find('\\') != std::string::npos || nameAsPath.filename().string() != gameName)
            {
                result.ErrorMessage = "GameName contains invalid characters or path separators: " + gameName;
                return result;
            }
        }

        // Step 1: Validate project (5%)
        OLO_CORE_INFO("[GameBuild] Step 1/9: Validating project...");
        if (!ValidateProject(result.ErrorMessage))
        {
            return result;
        }
        progress = 0.05f;

        if (cancelToken && cancelToken->load(std::memory_order_acquire))
        {
            result.ErrorMessage = "Build cancelled by user";
            return result;
        }

        // Create output directory structure (clean staging)
        const std::filesystem::path outputDir = settings.OutputDirectory / settings.GameName;
        std::error_code ec;
        if (std::filesystem::exists(outputDir, ec))
        {
            std::filesystem::remove_all(outputDir, ec);
            if (ec)
            {
                result.ErrorMessage = "Failed to clean existing output directory: " + ec.message();
                return result;
            }
        }
        std::filesystem::create_directories(outputDir, ec);
        if (ec)
        {
            result.ErrorMessage = "Failed to create output directory: " + ec.message();
            return result;
        }
        std::filesystem::create_directories(outputDir / "Assets", ec);
        if (IsScriptingAvailableOnPlatform(settings.TargetPlatform))
        {
            std::filesystem::create_directories(outputDir / "mono" / "lib", ec);
            std::filesystem::create_directories(outputDir / "mono" / "etc", ec);
            std::filesystem::create_directories(outputDir / "Resources" / "Scripts", ec);
        }

        result.OutputPath = outputDir;

        // Ship the renderer-backend config next to the exe (#691): the
        // engine's backend selection reads `config/renderer.yaml` (cwd first,
        // then the executable's directory), so this is what makes a packaged
        // game start on the developer's chosen default without a command-line
        // flag. Written through the same helper the editor dropdown uses — one
        // schema owner, no drift.
        {
            const bool wantVulkan = [&settings]
            {
                std::string lowered = settings.DefaultRendererBackend;
                std::ranges::transform(lowered, lowered.begin(), [](unsigned char c)
                                       { return static_cast<char>(std::tolower(c)); });
                return lowered == "vulkan";
            }();
            const auto rendererConfig = outputDir / "config" / "renderer.yaml";
            if (!WriteRendererConfig(rendererConfig,
                                     wantVulkan ? RendererAPI::API::Vulkan : RendererAPI::API::OpenGL))
            {
                result.ErrorMessage = "Failed to write " + rendererConfig.string();
                return result;
            }
        }

        // Step 2: Build asset pack (5% -> 55%)
        OLO_CORE_INFO("[GameBuild] Step 2/9: Building asset pack...");
        if (!BuildAssetPack(settings, outputDir, result.AssetCount, result.SceneCount, progress, cancelToken))
        {
            if (result.ErrorMessage.empty())
            {
                result.ErrorMessage = "Asset pack build failed";
            }
            return result;
        }
        progress = 0.55f;

        if (cancelToken && cancelToken->load(std::memory_order_acquire))
        {
            result.ErrorMessage = "Build cancelled by user";
            return result;
        }

        // Step 3: Copy runtime executable (55% -> 60%)
        OLO_CORE_INFO("[GameBuild] Step 3/9: Copying runtime executable...");
        if (!CopyRuntimeExecutable(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.60f;

        // Step 3b: Platform-specific executable finishing touches (#891) —
        // icon embedding on Windows, a .desktop launcher entry on Linux.
        // Non-fatal either way.
        {
            const std::filesystem::path destExe = outputDir / GetHostExecutableFileName(settings.GameName, settings.TargetPlatform);
            if (settings.TargetPlatform == BuildTargetPlatform::Windows)
            {
                if (!settings.IconPath.empty())
                {
                    OLO_CORE_INFO("[GameBuild] Embedding custom icon...");
                    std::string iconError;
                    if (!EmbedCustomIcon(destExe, settings.IconPath, iconError))
                    {
                        OLO_CORE_WARN("[GameBuild] Custom icon embedding failed (non-fatal): {}", iconError);
                    }
                }
            }
            else
            {
                OLO_CORE_INFO("[GameBuild] Writing Linux desktop entry...");
                std::string desktopError;
                if (!WriteLinuxDesktopEntry(destExe, settings.IconPath, settings.GameName, desktopError))
                {
                    OLO_CORE_WARN("[GameBuild] Desktop entry generation failed (non-fatal): {}", desktopError);
                }
            }
        }
        progress = 0.62f;

        if (cancelToken && cancelToken->load(std::memory_order_acquire))
        {
            result.ErrorMessage = "Build cancelled by user";
            return result;
        }

        // Step 4: Copy dependency DLLs (62% -> 68%)
        OLO_CORE_INFO("[GameBuild] Step 4/9: Copying dependency DLLs...");
        if (!CopyDependencyDLLs(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.68f;

        // Step 5: Stage every loose file the runtime reads — engine resources,
        // scenes, Lua scripts, loose textures, input actions and the files the
        // scenes reference by path (68% -> 80%)
        OLO_CORE_INFO("[GameBuild] Step 5/9: Staging runtime content...");
        if (!StageRuntimeContent(outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.80f;

        // Step 6: Copy Mono runtime (80% -> 88%)
        OLO_CORE_INFO("[GameBuild] Step 6/9: Copying Mono runtime...");
        if (!CopyMonoRuntime(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.88f;

        // Step 7: Copy ScriptCore assembly (88% -> 90%)
        OLO_CORE_INFO("[GameBuild] Step 7/9: Copying ScriptCore assembly...");
        if (!CopyScriptCoreAssembly(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }
        progress = 0.95f;

        // Step 8 (scenes, scripts, runtime config) is part of step 5's staging.

        // Step 9: Write game manifest (95% -> 100%)
        OLO_CORE_INFO("[GameBuild] Step 9/9: Writing game manifest...");
        if (!WriteGameManifest(settings, outputDir, result.ErrorMessage))
        {
            return result;
        }

        // Calculate total size
        result.TotalSizeBytes = CalculateDirectorySize(outputDir);

        auto endTime = std::chrono::high_resolution_clock::now();
        result.BuildTimeSeconds = std::chrono::duration<f64>(endTime - startTime).count();
        result.Success = true;
        progress = 1.0f;

        OLO_CORE_INFO("[GameBuild] Build completed successfully in {:.1f}s", result.BuildTimeSeconds);
        OLO_CORE_INFO("[GameBuild]   Output: {}", outputDir.string());
        OLO_CORE_INFO("[GameBuild]   Assets: {}, Scenes: {}", result.AssetCount, result.SceneCount);
        OLO_CORE_INFO("[GameBuild]   Total size: {:.1f} MB", static_cast<f64>(result.TotalSizeBytes) / (1024.0 * 1024.0));

        return result;
    }

    bool GameBuildPipeline::StageRuntimeContent(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        if (!CopyEngineResources(outputDir, errorMessage))
        {
            return false;
        }

        if (!CopySceneFiles(outputDir, errorMessage))
        {
            return false;
        }

        // Non-fatal — a game with no Lua scripts is perfectly normal, and a
        // failure here shouldn't sink an otherwise-complete build.
        {
            std::string scriptError;
            if (!CopyScriptFiles(outputDir, scriptError))
            {
                OLO_CORE_WARN("[GameBuild] Lua script copy failed (non-fatal): {}", scriptError);
            }
        }

        // Writable project runtime configuration and loose textures. Input
        // actions stay loose (rather than inside the immutable asset pack)
        // because the in-game rebind panel persists back to this same path.
        if (!StageProjectRuntimeFiles(outputDir, errorMessage))
        {
            return false;
        }

        return StageProjectSceneReferences(outputDir, errorMessage);
    }

    bool GameBuildPipeline::ValidateProject(std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        if (auto project = Project::GetActive(); !project)
        {
            errorMessage = "No active project. Open a project in the editor first.";
            return false;
        }

        if (auto assetManager = Project::GetAssetManager(); !assetManager)
        {
            errorMessage = "No asset manager available.";
            return false;
        }

        return true;
    }

    bool GameBuildPipeline::BuildAssetPack(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        sizet& assetCount,
        sizet& sceneCount,
        std::atomic<f32>& progress,
        const std::atomic<bool>* cancelToken)
    {
        OLO_PROFILE_FUNCTION();

        AssetPackBuilder::BuildSettings packSettings;
        packSettings.m_OutputPath = outputDir / "Assets" / "AssetPack.olopack";
        packSettings.m_CompressAssets = settings.CompressAssets;
        packSettings.m_IncludeScriptModule = settings.IncludeScriptModule;
        packSettings.m_ValidateAssets = settings.ValidateAssets;

        // The pack builder reports 0.0-1.0 progress; we map it to 0.05-0.60
        std::atomic<f32> packProgress = 0.0f;

        auto buildResult = AssetPackBuilder::BuildFromActiveProject(packSettings, packProgress, cancelToken);

        // Map final pack progress to our overall progress
        progress = 0.05f + (packProgress.load() * 0.55f);

        if (!buildResult.m_Success)
        {
            OLO_CORE_ERROR("[GameBuild] Asset pack build failed: {}", buildResult.m_ErrorMessage);
            return false;
        }

        assetCount = buildResult.m_AssetCount;
        sceneCount = buildResult.m_SceneCount;

        OLO_CORE_INFO("[GameBuild] Asset pack created: {} assets, {} scenes",
                      assetCount, sceneCount);
        if (buildResult.m_FailedAssetCount > 0)
        {
            OLO_CORE_WARN("[GameBuild] {} registered asset(s) did not load and are NOT in the pack — the "
                          "AssetPackBuilder warnings above name each one",
                          buildResult.m_FailedAssetCount);
        }
        return true;
    }

    bool GameBuildPipeline::CopyRuntimeExecutable(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        // Locate the OloRuntime executable based on build configuration and
        // target platform. The binary is at: bin/{Config}/OloRuntime/<name>
        // — <name> carries the host's native extension (.exe on Windows,
        // none on Linux); Build() already refused any target that isn't this
        // host's own platform, so that's the only convention that applies.
        const auto& startupDir = Application::GetStartupWorkingDirectory();
        std::filesystem::path engineRoot = startupDir.parent_path();

        const std::string runtimeExeName = GetHostExecutableFileName("OloRuntime", settings.TargetPlatform);
        std::filesystem::path runtimeExe = engineRoot / "bin" / settings.BuildConfiguration / "OloRuntime" / runtimeExeName;

        if (!std::filesystem::exists(runtimeExe))
        {
            // Try relative to the workspace root (common in development)
            // The editor typically runs from OloEditor/, so engine root is one level up
            runtimeExe = engineRoot / ".." / "bin" / settings.BuildConfiguration / "OloRuntime" / runtimeExeName;

            if (!std::filesystem::exists(runtimeExe))
            {
                errorMessage = runtimeExeName + " not found. Build OloRuntime in " + settings.BuildConfiguration +
                               " configuration first. Expected at: " + runtimeExe.string();
                return false;
            }
        }

        // Warn if the runtime binary appears to be stale relative to the
        // editor binary. The CMake build ensures they stay in sync, but this
        // catches manual or partial builds where only one target was rebuilt.
        {
            const std::string editorExeName = GetHostExecutableFileName("OloEditor", settings.TargetPlatform);
            std::filesystem::path editorExe = engineRoot / "bin" / settings.BuildConfiguration / "OloEditor" / editorExeName;
            if (std::filesystem::exists(editorExe))
            {
                std::error_code tsEc;
                auto runtimeTime = std::filesystem::last_write_time(runtimeExe, tsEc);
                auto editorTime = std::filesystem::last_write_time(editorExe, tsEc);
                if (!tsEc && runtimeTime < editorTime)
                {
                    OLO_CORE_WARN("[GameBuild] {} is older than {} — "
                                  "it may be missing recent engine changes. "
                                  "Rebuild the OloRuntime target to ensure the game binary is up-to-date.",
                                  runtimeExeName, editorExeName);
                }
            }
        }

        // Copy and rename to the game name
        const std::filesystem::path destExe = outputDir / GetHostExecutableFileName(settings.GameName, settings.TargetPlatform);
        std::error_code ec;
        std::filesystem::copy_file(runtimeExe, destExe,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMessage = "Failed to copy runtime executable: " + ec.message();
            return false;
        }

        // Linux does not use a file extension to mark a file runnable —
        // copy_file does not reliably preserve the executable bit across
        // filesystems, so set it explicitly. Unlike the DLL/Mono/icon steps,
        // this one is fatal: an unrunnable binary is exactly the "folder that
        // looks fine and does not run" acceptance criterion #4 exists to catch.
        if (settings.TargetPlatform == BuildTargetPlatform::Linux)
        {
            std::filesystem::permissions(destExe,
                                         std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
                                         std::filesystem::perm_options::add, ec);
            if (ec)
            {
                errorMessage = "Failed to mark " + destExe.string() + " executable: " + ec.message();
                return false;
            }

            constexpr std::filesystem::perms execBits =
                std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
            const auto status = std::filesystem::status(destExe, ec);
            if (ec || (status.permissions() & execBits) == std::filesystem::perms::none)
            {
                errorMessage = "Copied runtime executable " + destExe.string() + " is not runnable — "
                                                                                 "no execute permission bit is set after copy.";
                return false;
            }
        }

        OLO_CORE_INFO("[GameBuild] Runtime executable copied: {}", destExe.string());
        return true;
    }

    bool GameBuildPipeline::CopyDependencyDLLs(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        [[maybe_unused]] std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        // Every dependency the Linux runtime needs today is either statically
        // linked or resolved via the system's shared-library search path —
        // this step is Windows-shaped by name and function (#891). Kept as an
        // explicit early-return rather than falling through an empty Windows
        // DLL list so a future Linux runtime dependency has an obvious home.
        if (settings.TargetPlatform != BuildTargetPlatform::Windows)
        {
            OLO_CORE_INFO("[GameBuild] No dependency libraries to stage for {}", ToString(settings.TargetPlatform));
            return true;
        }

        // Resolve the runtime binary directory using the same logic as CopyRuntimeExecutable
        const auto& startupDir = Application::GetStartupWorkingDirectory();
        std::filesystem::path engineRoot = startupDir.parent_path();

        std::filesystem::path runtimeBinDir = engineRoot / "bin" / settings.BuildConfiguration / "OloRuntime";
        if (!std::filesystem::exists(runtimeBinDir))
        {
            // Fallback: editor runs from OloEditor/, engine root is one level up
            runtimeBinDir = engineRoot / ".." / "bin" / settings.BuildConfiguration / "OloRuntime";
        }

        sizet copiedCount = 0;
        if (!StageRuntimeDependencyLibraries(settings.TargetPlatform, runtimeBinDir, outputDir, copiedCount, errorMessage))
        {
            return false;
        }

        OLO_CORE_INFO("[GameBuild] Copied {} dependency DLLs", copiedCount);
        return true;
    }

    bool GameBuildPipeline::CopyEngineResources(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        // Engine resources are located relative to the editor working directory.
        // The build pipeline runs from OloEditor/ cwd.
        const auto copyOpts = std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive;
        std::error_code ec;

        // --- Shaders (required — renderer will fail without them) ---
        const std::filesystem::path shaderSrc = "assets/shaders";
        const std::filesystem::path shaderDst = outputDir / "assets" / "shaders";
        if (!std::filesystem::exists(shaderSrc))
        {
            errorMessage = "Engine shaders not found at: " + std::filesystem::absolute(shaderSrc).string();
            return false;
        }

        std::filesystem::create_directories(shaderDst, ec);
        std::filesystem::copy(shaderSrc, shaderDst, copyOpts, ec);
        if (ec)
        {
            errorMessage = "Failed to copy engine shaders: " + ec.message();
            return false;
        }

        sizet shaderCount = 0;
        for ([[maybe_unused]] const auto& entry : std::filesystem::recursive_directory_iterator(shaderDst))
        {
            if (entry.is_regular_file())
            {
                ++shaderCount;
            }
        }
        OLO_CORE_INFO("[GameBuild] Copied {} shader files", shaderCount);

        // --- Shader pack (issue #908, optional) ---
        bool packStaged = false;
        if (!StageShaderPack("assets/ShaderPack.osp", shaderDst.parent_path(), packStaged, errorMessage))
        {
            return false;
        }
        if (packStaged)
        {
            // Staging alone doesn't guarantee a hit: the runtime still
            // content-hash-validates every entry against the shipped
            // `assets/shaders` source (ShaderLibrary::TryReadPackEntry) before
            // serving it, and falls back to compiling from source for
            // anything that doesn't validate — a pack staged here from a
            // stale bake, or a script-loaded custom shader (ShaderLibrary.Load
            // exposed to Lua) outside the pack's fixed enumeration, still
            // compiles on first launch. See docs/agent-rules/shader-pack-bake.md.
            OLO_CORE_INFO("[GameBuild] Staged shader pack (ShaderPack.osp) — the packaged runtime will "
                          "try it first and fall back to compiling from source for any shader that "
                          "doesn't validate");
        }
        else
        {
            OLO_CORE_INFO("[GameBuild] No shader pack found — packaged runtime will compile shaders "
                          "from source on first launch");
        }

        // --- Textures (skybox cubemaps, IBL, etc.) ---
        const std::filesystem::path textureSrc = "assets/textures";
        const std::filesystem::path textureDst = outputDir / "assets" / "textures";
        if (std::filesystem::exists(textureSrc))
        {
            std::filesystem::create_directories(textureDst, ec);
            std::filesystem::copy(textureSrc, textureDst, copyOpts, ec);
            if (ec)
            {
                OLO_CORE_WARN("[GameBuild] Failed to copy textures: {}", ec.message());
                ec.clear();
            }
            else
            {
                OLO_CORE_INFO("[GameBuild] Textures copied");
            }
        }

        // --- Fonts (required for text rendering) ---
        const std::filesystem::path fontSrc = "assets/fonts";
        const std::filesystem::path fontDst = outputDir / "assets" / "fonts";
        if (std::filesystem::exists(fontSrc))
        {
            std::filesystem::create_directories(fontDst, ec);
            std::filesystem::copy(fontSrc, fontDst, copyOpts, ec);
            if (ec)
            {
                OLO_CORE_WARN("[GameBuild] Failed to copy fonts: {}", ec.message());
                // Non-fatal: text rendering falls back to built-in font
            }
            else
            {
                OLO_CORE_INFO("[GameBuild] Fonts copied");
            }
        }

        return true;
    }

    bool GameBuildPipeline::CopyMonoRuntime(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        // C# scripting is Windows-only — the engine links the Mono runtime
        // only on Windows (#891) — so a non-Windows target has no Mono
        // runtime to ship. This is the honest answer, not a
        // silently incomplete build: Lua scripting is unaffected.
        if (!IsScriptingAvailableOnPlatform(settings.TargetPlatform))
        {
            OLO_CORE_INFO("[GameBuild] Skipping Mono runtime copy — C# scripting is not available on {}",
                          ToString(settings.TargetPlatform));
            return true;
        }

        // Mono runtime is expected relative to the working directory
        // In development: OloEditor/mono/
        const std::filesystem::path monoSrcDir = "mono";

        if (!std::filesystem::exists(monoSrcDir))
        {
            OLO_CORE_WARN("[GameBuild] Mono runtime directory not found at: {}", monoSrcDir.string());
            return true; // Non-fatal — game might not use C# scripts
        }

        // Copy mono/lib/
        const std::filesystem::path monoLibSrc = monoSrcDir / "lib";
        const std::filesystem::path monoLibDst = outputDir / "mono" / "lib";
        if (std::filesystem::exists(monoLibSrc))
        {
            std::error_code ec;
            std::filesystem::copy(monoLibSrc, monoLibDst,
                                  std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive, ec);
            if (ec)
            {
                errorMessage = "Failed to copy Mono lib directory: " + ec.message();
                return false;
            }
        }

        // Copy mono/etc/
        const std::filesystem::path monoEtcSrc = monoSrcDir / "etc";
        const std::filesystem::path monoEtcDst = outputDir / "mono" / "etc";
        if (std::filesystem::exists(monoEtcSrc))
        {
            std::error_code ec;
            std::filesystem::copy(monoEtcSrc, monoEtcDst,
                                  std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive, ec);
            if (ec)
            {
                errorMessage = "Failed to copy Mono etc directory: " + ec.message();
                return false;
            }
        }

        OLO_CORE_INFO("[GameBuild] Mono runtime copied");
        return true;
    }

    bool GameBuildPipeline::CopyScriptCoreAssembly(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        // Same reasoning as CopyMonoRuntime: no ScriptCore assembly exists to
        // copy on a platform C# scripting doesn't run on (#891).
        if (!IsScriptingAvailableOnPlatform(settings.TargetPlatform))
        {
            OLO_CORE_INFO("[GameBuild] Skipping ScriptCore assembly copy — C# scripting is not available on {}",
                          ToString(settings.TargetPlatform));
            return true;
        }

        // The ScriptCore DLL is at Resources/Scripts/OloEngine-ScriptCore.dll
        const std::filesystem::path scriptCoreSrc = "Resources/Scripts/OloEngine-ScriptCore.dll";

        if (!std::filesystem::exists(scriptCoreSrc))
        {
            if (!Project::GetActive()->GetConfig().ScriptModulePath.empty())
            {
                errorMessage = "ScriptCore assembly required by the configured script module was not found: " + scriptCoreSrc.string();
                return false;
            }
            // A game without C# scripts may intentionally omit ScriptCore.
            OLO_CORE_WARN("[GameBuild] ScriptCore assembly absent; skipping managed assemblies: {}", scriptCoreSrc.string());
            return true;
        }

        const std::filesystem::path scriptCoreDst = outputDir / "Resources" / "Scripts" / "OloEngine-ScriptCore.dll";

        if (!StageManagedAssembly(scriptCoreSrc, scriptCoreDst, errorMessage))
        {
            return false;
        }

        // A configured app-specific script assembly is required.
        if (const auto& projectConfig = Project::GetActive()->GetConfig(); !projectConfig.ScriptModulePath.empty())
        {
            const std::filesystem::path appScriptSrc = projectConfig.ScriptModulePath;
            // Runtime looks for the assembly at Resources/Scripts/<filename>
            const std::filesystem::path appScriptDst = outputDir / "Resources" / "Scripts" / appScriptSrc.filename();
            if (!StageManagedAssembly(appScriptSrc, appScriptDst, errorMessage))
            {
                return false;
            }
            OLO_CORE_INFO("[GameBuild] App script assembly copied: {}", appScriptDst.string());
        }

        OLO_CORE_INFO("[GameBuild] ScriptCore assembly copied");
        return true;
    }

    bool GameBuildPipeline::CopySceneFiles(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        auto project = Project::GetActive();
        if (!project)
        {
            errorMessage = "No active project";
            return false;
        }

        // Scenes live under the project asset directory as .olo files
        const auto assetDir = Project::GetAssetDirectory();

        // Create Scenes/ directory in the output
        const auto sceneOutputDir = outputDir / "Scenes";
        std::error_code ec;
        std::filesystem::create_directories(sceneOutputDir, ec);

        // Find all .olo scene files recursively in the asset directory
        u32 copiedCount = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(assetDir, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            if (entry.path().extension() != ".olo")
            {
                continue;
            }

            // Preserve relative path from asset directory
            auto relativePath = std::filesystem::relative(entry.path(), assetDir, ec);
            auto destPath = sceneOutputDir / relativePath;

            std::filesystem::create_directories(destPath.parent_path(), ec);
            std::filesystem::copy_file(entry.path(), destPath,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec)
            {
                OLO_CORE_WARN("[GameBuild] Failed to copy scene file {}: {}", relativePath.string(), ec.message());
                ec.clear();
                continue;
            }

            ++copiedCount;
        }

        if (copiedCount == 0)
        {
            errorMessage = "No scene files (.olo) found in project asset directory";
            return false;
        }

        // Write the start scene path into the manifest
        // The project config may specify a start scene; otherwise fall back to the first found
        if (const auto& startScene = project->GetConfig().StartScene; !startScene.empty())
        {
            OLO_CORE_INFO("[GameBuild] Start scene from project config: {}", startScene.string());
        }

        OLO_CORE_INFO("[GameBuild] Copied {} scene file(s) to output", copiedCount);
        return true;
    }

    bool GameBuildPipeline::CopyScriptFiles(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        auto project = Project::GetActive();
        if (!project)
        {
            errorMessage = "No active project";
            return false;
        }

        const auto assetDir = Project::GetAssetDirectory();

        // Scripts land under <game>/Assets/<asset-relative path> — NOT a
        // Scripts/ sibling like the scene copy uses. The runtime resolves a
        // LuaScriptComponent's project-relative ScriptFile through
        // Project::GetAssetFileSystemPath, so the relative layout under the
        // asset root has to survive the build byte for byte.
        const auto scriptOutputRoot = outputDir / "Assets";
        std::error_code ec;
        std::filesystem::create_directories(scriptOutputRoot, ec);

        // The traversal is wrapped because recursive_directory_iterator's
        // operator++ (and directory_entry::is_regular_file) throw
        // filesystem_error on an unreadable entry — an asset tree with one
        // permission-denied subfolder would otherwise throw straight out of
        // here. Build() runs on a detached FThread with no handler above it, so
        // that would std::terminate the whole editor rather than fail a build
        // step this caller deliberately treats as non-fatal.
        u32 copiedCount = 0;
        try
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(assetDir, ec))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".lua")
                {
                    continue;
                }

                auto relativePath = std::filesystem::relative(entry.path(), assetDir, ec);
                auto destPath = scriptOutputRoot / relativePath;

                std::filesystem::create_directories(destPath.parent_path(), ec);
                std::filesystem::copy_file(entry.path(), destPath,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec)
                {
                    OLO_CORE_WARN("[GameBuild] Failed to copy script file {}: {}", relativePath.string(), ec.message());
                    ec.clear();
                    continue;
                }

                ++copiedCount;
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            // Loud, not silent: the shipped game is missing scripts it should
            // have had, and "the level does nothing" is a miserable way to find
            // that out later.
            OLO_CORE_ERROR("[GameBuild] Lua script scan of '{}' aborted after {} file(s): {}",
                           assetDir.string(), copiedCount, e.what());
            return true;
        }

        if (copiedCount == 0)
        {
            OLO_CORE_INFO("[GameBuild] No Lua script files found in project asset directory");
        }
        else
        {
            OLO_CORE_INFO("[GameBuild] Copied {} Lua script file(s) to output", copiedCount);
        }
        return true;
    }

    bool GameBuildPipeline::StageProjectRuntimeFiles(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        sizet copiedTextureCount = 0;
        if (!StageLooseRuntimeTextures(Project::GetAssetDirectory(), outputDir / "Assets",
                                       copiedTextureCount, errorMessage))
        {
            return false;
        }
        OLO_CORE_INFO("[GameBuild] Copied {} loose runtime texture file(s)", copiedTextureCount);

        const std::filesystem::path inputActionsSrc = Project::GetInputActionMapPath();
        std::error_code existsEc;
        const bool hasInputActions = std::filesystem::exists(inputActionsSrc, existsEc);
        if (existsEc)
        {
            errorMessage = "Failed to query project input-action config: " + existsEc.message();
            return false;
        }
        if (!hasInputActions)
        {
            OLO_CORE_INFO("[GameBuild] No project input-action config to copy");
            return true;
        }

        const std::filesystem::path inputActionsDst = outputDir / "Config" / "InputActions.yaml";
        std::error_code ec;
        std::filesystem::create_directories(inputActionsDst.parent_path(), ec);
        if (ec)
        {
            errorMessage = "Failed to create runtime Config directory: " + ec.message();
            return false;
        }

        std::filesystem::copy_file(inputActionsSrc, inputActionsDst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            errorMessage = "Failed to copy project input actions: " + ec.message();
            return false;
        }

        OLO_CORE_INFO("[GameBuild] Copied writable input actions to {}", inputActionsDst.string());
        return true;
    }

    bool GameBuildPipeline::StageProjectSceneReferences(
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        const auto project = Project::GetActive();
        if (!project)
        {
            errorMessage = "No active project";
            return false;
        }

        // The asset directory as scenes spell it ("Assets"): ProjectSerializer
        // stores it canonicalised to an absolute path. Same derivation as
        // ResolveContentPath, so the two agree on what counts as project content.
        std::filesystem::path assetDirectoryName = project->GetConfig().AssetDirectory;
        if (assetDirectoryName.is_absolute())
        {
            std::error_code relativeError;
            assetDirectoryName = std::filesystem::relative(assetDirectoryName, project->GetDirectory(), relativeError);
            if (relativeError || assetDirectoryName.empty())
            {
                errorMessage = "Cannot spell the asset directory relative to the project directory '" +
                               project->GetDirectory().string() + "'";
                return false;
            }
        }

        std::vector<std::filesystem::path> scenes;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(Project::GetAssetDirectory(), ec), end;
             it != end && !ec; it.increment(ec))
        {
            std::error_code entryEc; // per entry, apart from the iterator's
            if (it->is_regular_file(entryEc) && !entryEc && it->path().extension() == ".olo")
            {
                scenes.push_back(it->path());
            }
        }
        if (ec)
        {
            errorMessage = "Failed to enumerate the project's scenes: " + ec.message();
            return false;
        }
        std::ranges::sort(scenes);

        const std::filesystem::path engineRoot = std::filesystem::current_path(ec);
        if (ec)
        {
            errorMessage = "Failed to read the editor working directory: " + ec.message();
            return false;
        }

        sizet copiedCount = 0;
        std::vector<std::string> unresolved;
        if (!StageSceneReferencedContent(scenes, project->GetDirectory(), assetDirectoryName, engineRoot, outputDir,
                                         copiedCount, unresolved, errorMessage))
        {
            return false;
        }

        for (const auto& reference : unresolved)
        {
            OLO_CORE_WARN("[GameBuild] The packaged game will not find {}", reference);
        }
        OLO_CORE_INFO("[GameBuild] Staged {} scene-referenced file(s) from {} scene(s); {} reference(s) could not be "
                      "staged",
                      copiedCount, scenes.size(), unresolved.size());
        return true;
    }

    bool GameBuildPipeline::WriteGameManifest(
        const GameBuildSettings& settings,
        const std::filesystem::path& outputDir,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();

        const std::filesystem::path manifestPath = outputDir / "game.manifest";

        YAML::Emitter out;
        out << YAML::BeginMap;
        out << YAML::Key << "Game" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Name" << YAML::Value << settings.GameName;
        out << YAML::Key << "EngineVersion" << YAML::Value << BuildInfo::GetEngineVersion();
        // Build identity (#894) — the version + short git hash the depot was
        // packaged from, so a bug report can name its exact build. See
        // BuildInfo.h and docs/ops/shipping.md.
        out << YAML::Key << "BuildId" << YAML::Value << BuildInfo::GetBuildId();
        out << YAML::EndMap;

        out << YAML::Key << "Assets" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "PackFile" << YAML::Value << "Assets/AssetPack.olopack";
        out << YAML::Key << "SceneDirectory" << YAML::Value << "Scenes";
        out << YAML::EndMap;

        out << YAML::Key << "Rendering" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Is3DMode" << YAML::Value << settings.Is3DMode;
        out << YAML::EndMap;

        // Record the target explicitly, including whether C# scripting is
        // available on it (#891) — the honest answer, rather than a runtime
        // that silently discovers scripting is missing.
        out << YAML::Key << "Platform" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "Target" << YAML::Value << ToString(settings.TargetPlatform);
        out << YAML::Key << "CSharpScriptingAvailable" << YAML::Value << IsScriptingAvailableOnPlatform(settings.TargetPlatform);
        out << YAML::EndMap;

        // Write start scene from build settings (asset-relative path)
        if (!settings.StartScene.empty())
        {
            // The StartScene in settings is relative to the project asset dir (e.g. "Scenes/MyScene.olo").
            // The runtime expects a path relative to the game output dir (e.g. "Scenes/MyScene.olo").
            out << YAML::Key << "StartScene" << YAML::Value << settings.StartScene.generic_string();
        }
        else
        {
            // Fallback: try project config, converting absolute to relative
            auto project = Project::GetActive();
            if (project && !project->GetConfig().StartScene.empty())
            {
                std::error_code ec;
                auto relative = std::filesystem::relative(
                    project->GetConfig().StartScene, Project::GetAssetDirectory(), ec);
                if (!ec && !relative.empty())
                {
                    out << YAML::Key << "StartScene" << YAML::Value << relative.generic_string();
                }
                else
                {
                    // Last resort: just the filename under Scenes/
                    out << YAML::Key << "StartScene" << YAML::Value
                        << ("Scenes/" + project->GetConfig().StartScene.filename().string());
                }
            }
        }

        out << YAML::EndMap;

        std::ofstream fout(manifestPath);
        if (!fout.is_open())
        {
            errorMessage = "Failed to create game manifest file: " + manifestPath.string();
            return false;
        }
        fout << out.c_str();
        fout.close();

        OLO_CORE_INFO("[GameBuild] Game manifest written: {}", manifestPath.string());
        return true;
    }

    sizet GameBuildPipeline::CalculateDirectorySize(const std::filesystem::path& directory)
    {
        OLO_PROFILE_FUNCTION();

        sizet totalSize = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ec))
        {
            if (entry.is_regular_file())
            {
                totalSize += entry.file_size(ec);
            }
        }
        return totalSize;
    }
    bool GameBuildPipeline::EmbedCustomIcon(
        const std::filesystem::path& exePath,
        const std::filesystem::path& iconPath,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();
        return BuildPipelinePlatform::EmbedCustomIcon(exePath, iconPath, errorMessage);
    }

    bool GameBuildPipeline::WriteLinuxDesktopEntry(
        const std::filesystem::path& exePath,
        const std::filesystem::path& iconPath,
        const std::string& gameName,
        std::string& errorMessage)
    {
        OLO_PROFILE_FUNCTION();
        return BuildPipelinePlatform::WriteDesktopEntry(exePath, iconPath, gameName, errorMessage);
    }

} // namespace OloEngine
