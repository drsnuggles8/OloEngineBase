#include "OloEnginePCH.h"
#include "Automation/AutomationAssetCommands.h"

#include "Automation/AutomationAssetIndex.h"
#include "Automation/AutomationFileWrite.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Asset/InstancePlacementAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "UndoRedo/EditorCommand.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
        namespace Schema = MCP::Schema;

        // How many referrers one result will name before it stops. A destructive
        // command that hits this REFUSES rather than acting on a partial list --
        // see RefusalForReferrers.
        constexpr sizet kMaxReportedReferrers = 500;

        Json Failure(std::string message)
        {
            return Json{ { "__error", std::move(message) } };
        }

        [[nodiscard]] bool IsFailure(const Json& value)
        {
            return value.is_object() && value.contains("__error");
        }

        // ---- the project view a command needs --------------------------------

        // Everything about the project a command reads, captured under one
        // main-thread marshal so the SCAN can then run on the handler thread. The
        // scan walks the whole project; doing it inside MarshalRead would hold the
        // game thread for its duration and blow the 5s marshal timeout on any
        // project bigger than the sandbox.
        struct ProjectView
        {
            std::filesystem::path Root;
            std::filesystem::path AssetDirectory;
            std::filesystem::path WorkingDirectory;
        };

        AssetIndexScope MakeScope(const ProjectView& project)
        {
            AssetIndexScope scope;
            scope.ProjectRoot = project.Root;
            scope.AssetDirectory = project.AssetDirectory;
            // Mirrors EditorAssetManager::ImportAsset's final fallback, which is
            // plain std::filesystem::absolute() -- i.e. relative to the process
            // working directory. For the editor that is OloEditor/, which is how
            // the "assets/textures/..." spelling in checked-in scenes resolves at
            // all. Passed in rather than read here so the index stays a pure
            // function of its scope and a test can pin it.
            if (!project.WorkingDirectory.empty())
                scope.BaseDirectories.push_back(project.WorkingDirectory);
            return scope;
        }

        Json CaptureProject(ProjectView& project)
        {
            if (!Project::GetActive())
                return Failure("No active project.");
            if (!Project::HasAssetManager())
                return Failure("The active project has no asset manager; asset commands are unavailable here.");
            project.Root = Project::GetProjectDirectory();
            project.AssetDirectory = Project::GetAssetDirectory();
            std::error_code ec;
            project.WorkingDirectory = std::filesystem::current_path(ec);
            if (ec)
                project.WorkingDirectory.clear();
            return Json::object();
        }

        // Project::GetAssetManager() ASSERTS when none is set -- it is not a
        // nullable accessor -- so a host with a project but no asset manager would
        // take an assert here rather than getting the "not available on this host"
        // refusal every other command in the surface returns. HasAssetManager is
        // the guard that exists for exactly this.
        Ref<EditorAssetManager> EditorManager()
        {
            if (!Project::HasAssetManager())
                return {};
            return Project::GetAssetManager().As<EditorAssetManager>();
        }

        // ---- resolving the asset a command was pointed at ---------------------

        struct AssetTarget
        {
            AssetHandle Handle{ 0 };
            AssetType Type = AssetType::None;
            std::filesystem::path RegistryKey; // as stored in the registry.
            std::filesystem::path AbsolutePath;
            AssetStatus Status = AssetStatus::None;
            bool ExistsOnDisk = false;
            bool Registered = false;
        };

        std::optional<u64> ParseHandle(const Json& value)
        {
            if (value.is_number_unsigned())
                return value.get<u64>();
            if (!value.is_string())
                return std::nullopt;
            const auto& text = value.get_ref<const std::string&>();
            if (text.empty() || !std::ranges::all_of(text, [](char ch)
                                                     { return ch >= '0' && ch <= '9'; }))
                return std::nullopt;
            u64 parsed = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (error != std::errc{} || end != text.data() + text.size())
                return std::nullopt;
            return parsed;
        }

        std::filesystem::path Canonicalize(const std::filesystem::path& path)
        {
            std::error_code ec;
            std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
            return ec ? path.lexically_normal() : canonical;
        }

        // Resolve `path` the way EditorAssetManager::ImportAsset would, so a
        // command and the engine agree on which file a project-relative string
        // names. Absolute input is taken as given.
        std::filesystem::path ResolveProjectPath(const ProjectView& project, const std::filesystem::path& path)
        {
            if (path.empty())
                return {};
            if (path.is_absolute())
                return Canonicalize(path);
            std::error_code ec;
            if (const std::filesystem::path projectRelative = project.Root / path;
                std::filesystem::exists(projectRelative, ec) && !ec)
            {
                return Canonicalize(projectRelative);
            }
            // Not on disk yet (a create or move destination). Project-relative is
            // the documented spelling, so that is what a new path means.
            return Canonicalize(project.Root / path);
        }

        // Main-thread. Fills `target` from either a handle or a path argument.
        Json ResolveTarget(const ProjectView& project, const Json& args, AssetTarget& target)
        {
            auto manager = EditorManager();
            if (!manager)
                return Failure("Asset commands require an editor asset manager (no active project).");

            const bool hasHandle = args.contains("handle");
            const bool hasPath = args.contains("path");
            if (hasHandle == hasPath)
                return Failure("Provide exactly one of 'handle' or 'path'.");

            if (hasHandle)
            {
                const auto handle = ParseHandle(args.at("handle"));
                if (!handle || *handle == 0)
                    return Failure("handle must be a nonzero decimal asset handle.");
                target.Handle = AssetHandle(*handle);
                const AssetMetadata metadata = manager->GetMetadata(target.Handle);
                if (!metadata.IsValid())
                    return Failure("No asset is registered under handle " + std::to_string(*handle) + ".");
                target.Registered = true;
                target.Type = metadata.Type;
                target.Status = metadata.Status;
                target.RegistryKey = metadata.FilePath;
                target.AbsolutePath = Canonicalize(manager->GetFileSystemPath(metadata));
            }
            else
            {
                if (!args.at("path").is_string())
                    return Failure("path must be a string.");
                const std::string raw = args.at("path").get<std::string>();
                if (raw.empty())
                    return Failure("path must not be empty.");
                target.AbsolutePath = ResolveProjectPath(project, std::filesystem::path(raw));
                const std::filesystem::path key =
                    EditorAssetManager::MakeRegistryKey(target.AbsolutePath, project.Root);
                if (const AssetHandle handle = manager->GetAssetHandleFromFilePath(key); handle != 0)
                {
                    target.Handle = handle;
                    const AssetMetadata metadata = manager->GetMetadata(handle);
                    target.Registered = metadata.IsValid();
                    target.Type = metadata.Type;
                    target.Status = metadata.Status;
                    target.RegistryKey = metadata.FilePath;
                }
                else
                {
                    // An unregistered file is still a legitimate query target --
                    // "what references this .png I am about to delete" must work
                    // whether or not anything imported it yet.
                    target.RegistryKey = key;
                    target.Type = AssetExtensions::GetAssetTypeFromPath(target.AbsolutePath.string());
                }
            }
            std::error_code ec;
            target.ExistsOnDisk = std::filesystem::is_regular_file(target.AbsolutePath, ec) && !ec;
            return Json::object();
        }

        Json DescribeTarget(const AssetTarget& target, const ProjectView& project)
        {
            Json described{ { "handle", std::to_string(static_cast<u64>(target.Handle)) },
                            { "registered", target.Registered },
                            { "type", AssetUtils::AssetTypeToString(target.Type) },
                            { "path", target.RegistryKey.generic_string() },
                            { "absolutePath", target.AbsolutePath.generic_string() },
                            { "name", target.AbsolutePath.filename().string() },
                            { "existsOnDisk", target.ExistsOnDisk } };
            if (target.Registered)
                described["status"] = AssetStatusUtils::AssetStatusToString(target.Status);
            std::error_code ec;
            if (target.ExistsOnDisk)
            {
                if (const auto size = std::filesystem::file_size(target.AbsolutePath, ec); !ec)
                    described["sizeBytes"] = static_cast<u64>(size);
            }
            // By path COMPONENT, not string prefix: "/work/proj-backup/A.png"
            // starts_with("/work/proj") and is not in the project at all.
            std::error_code relativeEc;
            const std::filesystem::path relativeToRoot =
                std::filesystem::relative(target.AbsolutePath, project.Root, relativeEc);
            described["inProject"] =
                !relativeEc && !relativeToRoot.empty() && *relativeToRoot.begin() != "..";
            return described;
        }

        // Per-asset import settings live in a sidecar beside the asset (see
        // olo_asset_import_settings for why the extension is what it is). Move and
        // delete have to carry it: a sidecar left behind by a move stops applying
        // and litters the tree, and one left behind by a delete silently reattaches
        // itself to whatever is created at that path next.
        std::filesystem::path SidecarPath(const std::filesystem::path& assetPath)
        {
            std::filesystem::path sidecar = assetPath;
            sidecar += ".oloimport";
            return sidecar;
        }

        // ---- the reference index ----------------------------------------------

        // Built fresh on every call, deliberately. A cached index answers "who
        // references this" from a snapshot of the project as it was, and the whole
        // value of the answer is that it is true RIGHT NOW -- a delete authorised
        // against a stale index is exactly the silent data loss this command set
        // exists to prevent. The walk is bounded by MaxFiles and reports its own
        // cost, so a project where the rescan stops being cheap says so rather
        // than quietly getting slower.
        AssetIndex ScanProject(IAutomationHost& host, const AssetIndexScope& scope)
        {
            host.EmitProgress(0.0, 2.0, "Scanning project asset files for references");
            AssetIndex index = BuildAssetIndex(scope);
            host.EmitProgress(1.0, 2.0, "Resolving references");
            return index;
        }

        Json DescribeCoverage(const AssetIndex& index, const AssetIndexScope& scope)
        {
            Json extensions = Json::array();
            for (const std::string& extension : ScannableExtensions())
                extensions.push_back(extension);
            Json skipped = Json::array();
            for (const std::string& extension : index.Coverage.BinaryExtensionsSkipped)
                skipped.push_back(extension);
            Json unreadable = Json::array();
            for (const std::string& file : index.Coverage.UnreadableFiles)
                unreadable.push_back(file);
            Json bases = Json::array();
            for (const std::filesystem::path& base : scope.BaseDirectories)
                bases.push_back(base.generic_string());
            return Json{
                { "filesScanned", index.Coverage.FilesScanned },
                { "referencesFound", index.Coverage.ReferencesFound },
                { "scannedExtensions", std::move(extensions) },
                { "binaryFilesSkipped", index.Coverage.BinaryFilesSkipped },
                { "binaryExtensionsSkipped", std::move(skipped) },
                { "unreadableFiles", std::move(unreadable) },
                { "unresolvedReferences", index.Coverage.UnresolvedReferences },
                { "truncated", index.Coverage.Truncated },
                { "complete", index.Coverage.Complete },
                { "incompleteReason", index.Coverage.IncompleteReason },
                { "scanCompleted", index.Coverage.ScanCompleted() },
                { "projectRoot", scope.ProjectRoot.generic_string() },
                { "assetDirectory", scope.AssetDirectory.generic_string() },
                { "extraBaseDirectories", std::move(bases) },
                { "handleKeyFiltered", true },
                { "note",
                  "Path references are found by value (any scalar ending in a known asset extension) and resolved "
                  "against the project root, the asset directory, the legacy project-prefixed spelling and the "
                  "working directory, in that order -- the same anchors the engine itself uses. A value with no "
                  "directory separator counts only when it resolves, because a bare filename is ambiguous with a "
                  "name that happens to end in an asset extension. Handle references are found only under "
                  "handle-shaped keys, so an asset referenced by a handle under an unrecognised key would be missed. "
                  "Binary formats listed under binaryExtensionsSkipped are not searched at all. An empty referrer "
                  "list means nothing was found in this set, not that nothing references the asset." }
            };
        }

        Json DescribeReference(const AssetReference& reference, const ProjectView& project)
        {
            Json described{ { "file", reference.SourceFile.generic_string() },
                            { "line", reference.Line },
                            { "key", reference.Key },
                            { "value", reference.RawValue },
                            { "kind", reference.Kind == AssetReferenceKind::Handle ? "handle" : "path" },
                            { "resolved", reference.IsResolved() } };
            const std::filesystem::path relative =
                std::filesystem::relative(reference.SourceFile, project.Root);
            if (!relative.empty() && *relative.begin() != "..")
                described["projectPath"] = relative.generic_string();
            if (reference.Kind == AssetReferenceKind::Path && !reference.ResolvedFile.empty())
                described["resolvedFile"] = reference.ResolvedFile.generic_string();
            return described;
        }

        // ---- olo_asset_get -----------------------------------------------------

        AutomationResult AssetGet(IAutomationHost& host, const Json& args)
        {
            ProjectView project;
            AssetTarget target;
            const Json prepared = host.MarshalRead([&project, &target, &args]() -> Json
                                                   {
                if (Json captured = CaptureProject(project); IsFailure(captured))
                    return captured;
                return ResolveTarget(project, args, target); });
            if (IsFailure(prepared))
                return AutomationResult::Error(prepared.at("__error").get<std::string>());
            return AutomationResult::Structured(DescribeTarget(target, project));
        }

        // ---- olo_asset_references ---------------------------------------------

        AutomationResult AssetReferences(IAutomationHost& host, const Json& args)
        {
            ProjectView project;
            AssetTarget target;
            const Json prepared = host.MarshalRead([&project, &target, &args]() -> Json
                                                   {
                if (Json captured = CaptureProject(project); IsFailure(captured))
                    return captured;
                return ResolveTarget(project, args, target); });
            if (IsFailure(prepared))
                return AutomationResult::Error(prepared.at("__error").get<std::string>());

            const std::string direction = args.value("direction", std::string("referrers"));
            const AssetIndexScope scope = MakeScope(project);
            const AssetIndex index = ScanProject(host, scope);
            if (host.IsCurrentCallCancelled())
                return AutomationResult::Error("Cancelled while scanning the project.");

            std::vector<AssetReference> found =
                direction == "dependencies"
                    ? FindDependencies(index, target.AbsolutePath)
                    : FindReferrers(index, target.AbsolutePath, static_cast<u64>(target.Handle));

            Json entries = Json::array();
            const sizet reported = std::min(found.size(), kMaxReportedReferrers);
            for (sizet i = 0; i < reported; ++i)
                entries.push_back(DescribeReference(found[i], project));

            Json result{ { "asset", DescribeTarget(target, project) },
                         { "direction", direction },
                         { "count", static_cast<u32>(found.size()) },
                         { "returned", static_cast<u32>(reported) },
                         { "listTruncated", found.size() > reported },
                         { "references", std::move(entries) },
                         { "coverage", DescribeCoverage(index, scope) } };
            return AutomationResult::Structured(result);
        }

        // ---- shared refusal shape for a referenced asset -----------------------

        // The one place a destructive command decides it must not proceed. Both
        // conditions are refusals, and they are DIFFERENT refusals: a truncated
        // index means the referrer list itself cannot be trusted, which is worse
        // than a known non-empty one and must never be waivable by the same flag.
        std::optional<Json> RefusalForReferrers(const AssetIndex& index, const std::vector<AssetReference>& referrers,
                                                const AssetTarget& target, const ProjectView& project,
                                                const AssetIndexScope& scope, bool force, std::string_view verb)
        {
            if (!index.Coverage.ScanCompleted())
            {
                Json refusal{ { "refused", true },
                              { "reason", index.Coverage.Truncated ? "index-truncated" : "scan-incomplete" },
                              { "message", std::string("Refusing to ") + std::string(verb) +
                                               " this asset: the reference scan did not see the whole project, so "
                                               "the referrer list is incomplete and cannot be trusted. " +
                                               (index.Coverage.Truncated
                                                    ? std::string("It hit its file limit.")
                                                    : index.Coverage.IncompleteReason) +
                                               " 'force' deliberately does NOT waive this -- it is a claim about "
                                               "the quality of the list, not about the risk you are accepting." },
                              { "asset", DescribeTarget(target, project) },
                              { "coverage", DescribeCoverage(index, scope) } };
                return refusal;
            }
            if (referrers.empty() || force)
                return std::nullopt;
            Json entries = Json::array();
            const sizet reported = std::min(referrers.size(), kMaxReportedReferrers);
            for (sizet i = 0; i < reported; ++i)
                entries.push_back(DescribeReference(referrers[i], project));
            Json refusal{ { "refused", true },
                          { "reason", "referenced" },
                          { "message", std::string("Refusing to ") + std::string(verb) + " an asset that " +
                                           std::to_string(referrers.size()) +
                                           " reference(s) point at. Pass force:true to proceed and break them." },
                          { "asset", DescribeTarget(target, project) },
                          { "referrerCount", static_cast<u32>(referrers.size()) },
                          { "referrers", std::move(entries) },
                          { "coverage", DescribeCoverage(index, scope) } };
            return refusal;
        }

        // ---- destination validation (create / move) ---------------------------

        // Is `candidate` inside `directory`? By path COMPONENT: a bare string
        // prefix says yes for "/work/proj-backup" against "/work/proj".
        bool IsInside(const std::filesystem::path& candidate, const std::filesystem::path& directory)
        {
            if (directory.empty())
                return false;
            std::error_code ec;
            const std::filesystem::path relative =
                std::filesystem::relative(candidate, Canonicalize(directory), ec);
            return !ec && !relative.empty() && *relative.begin() != "..";
        }

        // Every command that WRITES has to check this, not just the ones that take
        // a destination. ResolveTarget accepts an absolute path, so without it
        // olo_asset_delete would std::filesystem::remove any file on the machine
        // and olo_asset_import_settings would drop a sidecar next to it -- write
        // consent is a gate on touching the PROJECT, never a licence for the whole
        // disk. Read-only commands are deliberately not gated: answering "what is
        // this file, and does anything point at it" about a path outside the
        // project is useful and harmless.
        Json RequireWritableTarget(const ProjectView& project, const AssetTarget& target, std::string_view verb)
        {
            if (IsInside(target.AbsolutePath, project.AssetDirectory))
                return Json::object();
            return Failure("Refusing to " + std::string(verb) + " a path outside the project asset directory (" +
                           Canonicalize(project.AssetDirectory).generic_string() +
                           "). Got: " + target.AbsolutePath.generic_string());
        }

        // A destination must land INSIDE the project asset directory. Writing an
        // engine asset anywhere else produces a file the registry will not track
        // and the watcher will not see -- which looks like it worked and is not
        // an asset.
        Json ValidateDestination(const ProjectView& project, const std::string& raw, bool createDirectories,
                                 std::filesystem::path& out)
        {
            if (raw.empty())
                return Failure("destination must not be empty.");
            if (raw.find('\0') != std::string::npos)
                return Failure("destination contains a null character.");
            std::filesystem::path destination = ResolveProjectPath(project, std::filesystem::path(raw));
            const std::string assetDir = Canonicalize(project.AssetDirectory).generic_string();
            const std::string candidate = destination.generic_string();
            if (!IsInside(destination, project.AssetDirectory))
            {
                return Failure("destination must be inside the project asset directory (" + assetDir +
                               "). Got: " + candidate);
            }
            std::error_code ec;
            if (!std::filesystem::is_directory(destination.parent_path(), ec) || ec)
            {
                // Reorganising assets into a NEW folder is an ordinary thing to
                // want, and this surface has no directory command, so refusing
                // outright would make it impossible rather than merely two-step.
                // But creating a tree for a typo'd path silently is exactly the
                // helpfulness that loses files, so it is an explicit opt-in and
                // the refusal names the flag that grants it.
                if (!createDirectories)
                {
                    return Failure("destination directory does not exist: " +
                                   destination.parent_path().generic_string() +
                                   ". Pass createDirectories:true to create it.");
                }
                std::error_code createEc;
                std::filesystem::create_directories(destination.parent_path(), createEc);
                if (createEc)
                {
                    return Failure("Cannot create destination directory " +
                                   destination.parent_path().generic_string() + ": " + createEc.message());
                }
            }
            if (std::filesystem::exists(destination, ec) && !ec)
                return Failure("destination already exists: " + candidate);
            out = std::move(destination);
            return Json::object();
        }

        // ---- olo_asset_move ----------------------------------------------------

        // One referring file's pending edit, computed before anything is written.
        struct PendingEdit
        {
            std::filesystem::path File;
            FileContents Before;
            std::string After;
            u32 ReferencesRewritten = 0;
        };

        // Group the referrers by file and produce one before/after pair per file.
        // Computing every edit up front is what makes the move all-or-nothing: a
        // reference that cannot be re-spelled aborts before the first byte is
        // written, rather than leaving half the project pointing at a file that
        // has already moved.
        Json PlanReferenceEdits(const std::vector<AssetReference>& referrers, const std::filesystem::path& oldTarget,
                                const std::filesystem::path& newTarget, const AssetIndexScope& scope,
                                std::vector<PendingEdit>& edits)
        {
            std::vector<AssetReference> ordered = referrers;
            // Descending by (file, line, column) so an edit never shifts the offset
            // of one not yet applied.
            std::ranges::sort(ordered, [](const AssetReference& a, const AssetReference& b)
                              {
                if (a.SourceFile != b.SourceFile)
                    return a.SourceFile < b.SourceFile;
                if (a.Line != b.Line)
                    return a.Line > b.Line;
                return a.ValueColumn > b.ValueColumn; });

            sizet i = 0;
            while (i < ordered.size())
            {
                const std::filesystem::path& file = ordered[i].SourceFile;
                PendingEdit edit;
                edit.File = file;
                try
                {
                    edit.Before = ReadFileContents(file);
                }
                catch (const std::exception& error)
                {
                    return Failure(std::string("Cannot read referring file ") + file.generic_string() + ": " +
                                   error.what());
                }
                if (!edit.Before)
                    return Failure("Referring file vanished during the move: " + file.generic_string());
                std::string text = *edit.Before;
                for (; i < ordered.size() && ordered[i].SourceFile == file; ++i)
                {
                    const AssetReference& reference = ordered[i];
                    if (reference.Kind == AssetReferenceKind::Handle)
                        continue; // the handle is the identity; a move does not change it.
                    const std::string replacement = RespellReference(reference, oldTarget, newTarget, scope);
                    if (replacement.empty())
                    {
                        return Failure("Cannot rewrite the reference at " + file.generic_string() + ":" +
                                       std::to_string(reference.Line) + " (key '" + reference.Key + "', value '" +
                                       reference.RawValue + "'). Refusing the whole move rather than breaking it.");
                    }
                    if (!ApplyReferenceEdit(text, reference, replacement))
                    {
                        return Failure("The reference at " + file.generic_string() + ":" +
                                       std::to_string(reference.Line) +
                                       " no longer matches what the scan recorded; the file changed underneath. "
                                       "Refusing the move.");
                    }
                    ++edit.ReferencesRewritten;
                }
                if (edit.ReferencesRewritten > 0)
                {
                    edit.After = std::move(text);
                    edits.push_back(std::move(edit));
                }
            }
            return Json::object();
        }

        // Write every planned edit, then move the asset file. On any failure, roll
        // back everything already written -- a partially rewritten project is
        // strictly worse than a refused move.
        Json CommitMove(const std::vector<PendingEdit>& edits, const std::filesystem::path& source,
                        const std::filesystem::path& destination)
        {
            sizet written = 0;
            const auto rollback = [&edits, &written]()
            {
                for (sizet i = written; i-- > 0;)
                {
                    try
                    {
                        ReplaceFileContents(edits[i].File, edits[i].After, edits[i].Before);
                    }
                    catch (const std::exception& error)
                    {
                        // Report loudly: the tree is now inconsistent and only a
                        // human can settle it. Swallowing this is how a rollback
                        // failure becomes a mystery next week.
                        OLO_CORE_ERROR("Asset move rollback failed for {}: {}", edits[i].File.string(), error.what());
                    }
                }
            };

            try
            {
                for (; written < edits.size(); ++written)
                    ReplaceFileContents(edits[written].File, edits[written].Before, edits[written].After);
            }
            catch (const std::exception& error)
            {
                rollback();
                return Failure(std::string("Failed to rewrite a referring file: ") + error.what());
            }

            std::error_code ec;
            std::filesystem::rename(source, destination, ec);
            if (ec)
            {
                rollback();
                return Failure("Failed to move the asset file: " + ec.message());
            }

            // The import-settings sidecar is part of the asset. Move it last, and
            // put the asset back if it will not follow -- an asset that arrives at
            // its destination having quietly shed its import settings is exactly
            // the delayed-symptom failure this command set exists to avoid.
            const std::filesystem::path sourceSidecar = SidecarPath(source);
            if (std::filesystem::exists(sourceSidecar, ec) && !ec)
            {
                std::error_code sidecarEc;
                std::filesystem::rename(sourceSidecar, SidecarPath(destination), sidecarEc);
                if (sidecarEc)
                {
                    std::error_code undoEc;
                    std::filesystem::rename(destination, source, undoEc);
                    rollback();
                    if (undoEc)
                    {
                        // The asset is at the destination and the references point
                        // at the source. Saying "rolled the move back" here would
                        // be false, and a false all-clear on a half-applied move is
                        // worse than the failure itself.
                        OLO_CORE_ERROR("Asset move rollback failed: {} still sits at {} ({})", source.string(),
                                       destination.string(), undoEc.message());
                        return Failure("Moved the asset but could not move its import-settings sidecar (" +
                                       sidecarEc.message() + "), AND could not move the asset back (" +
                                       undoEc.message() + "). The asset file is now at " +
                                       destination.generic_string() + " while every reference points at " +
                                       source.generic_string() + ". This needs fixing by hand.");
                    }
                    return Failure("Moved the asset but could not move its import-settings sidecar (" +
                                   sidecarEc.message() + "); rolled the move back.");
                }
            }
            return Json::object();
        }

        // Everything one move did, in a form that replays either way. A move IS
        // undoable -- unlike an import, every effect it has is a file write and a
        // registry key, and both are recoverable -- so the issue's "undo where the
        // operation is undoable" applies and this is what makes it true.
        //
        // Every write goes back through the guarded ReplaceFileContents, so an undo
        // after somebody edited one of the referring files by hand REFUSES instead
        // of clobbering their edit. That is the whole reason the guard takes the
        // expected bytes rather than just writing.
        struct MoveRecord
        {
            std::filesystem::path Source;
            std::filesystem::path Destination;
            AssetHandle Handle{ 0 };
            std::filesystem::path SourceKey;      // registry key before the move.
            std::filesystem::path DestinationKey; // ...and after.
            bool Registered = false;
            bool HadSidecar = false;
            std::vector<PendingEdit> Edits;
        };

        class AssetMoveCommand final : public EditorCommand
        {
          public:
            explicit AssetMoveCommand(MoveRecord record) : m_Record(std::move(record)) {}

            // Pushed with PushAlreadyExecuted, so this runs only on REDO.
            void Execute() override
            {
                Apply(true);
            }
            void Undo() override
            {
                Apply(false);
            }
            [[nodiscard]] std::string GetDescription() const override
            {
                return "Move Asset";
            }

          private:
            // Put the first `count` rewrites back the way they were. Used by both
            // failure paths below, because a half-rewritten project is strictly
            // worse than a refused undo.
            void RevertRewrites(bool forward, sizet count) const
            {
                for (sizet i = count; i-- > 0;)
                {
                    const PendingEdit& edit = m_Record.Edits[i];
                    const FileContents after(edit.After);
                    try
                    {
                        ReplaceFileContents(edit.File, forward ? after : edit.Before,
                                            forward ? edit.Before : after);
                    }
                    catch (const std::exception& error)
                    {
                        OLO_CORE_ERROR("Asset move undo rollback failed for {}: {}", edit.File.string(),
                                       error.what());
                    }
                }
            }

            void Apply(bool forward)
            {
                const std::filesystem::path& from = forward ? m_Record.Source : m_Record.Destination;
                const std::filesystem::path& to = forward ? m_Record.Destination : m_Record.Source;

                // Rewrite the referring files first, exactly as the forward move
                // did, so a refusal happens before anything has moved.
                //
                // THE REFUSAL IS RETHROWN AFTER THE HANDLER, NOT INSIDE IT. This used
                // to read `catch (...) { RevertRewrites(...); throw; }`, and that
                // shape is the one build-trees-and-windows-asan.md §4b forbids: under
                // clang-cl + ASan, a throw executed lexically inside a catch handler
                // makes __CxxFrameHandler3 read the handler funclet's parent frame as
                // NULL and fault. It presented as
                //   SEH exception with code 0xc0000005 thrown in the test body
                // from UndoRefusesWhenAReferringFileChangedUnderneath on every Windows
                // ASan shard — no ASan report, no stack, because gtest's SEH catcher
                // gets there first — while the same test passes in every other
                // configuration. A 50-line standalone TU of exactly this frame shape
                // (destructor-bearing locals in the try, a helper with its own
                // try/catch in the handler, then the rethrow) reproduces the fault on
                // clang 23.1.0; capturing the exception and rethrowing once the
                // handler has been left passes. The rollback itself is unchanged and
                // still runs before the exception reaches the caller.
                sizet written = 0;
                std::exception_ptr refusal;
                try
                {
                    for (; written < m_Record.Edits.size(); ++written)
                    {
                        const PendingEdit& edit = m_Record.Edits[written];
                        const FileContents after(edit.After);
                        ReplaceFileContents(edit.File, forward ? edit.Before : after,
                                            forward ? after : edit.Before);
                    }
                }
                catch (...)
                {
                    refusal = std::current_exception();
                }
                if (refusal)
                {
                    RevertRewrites(forward, written);
                    std::rethrow_exception(refusal);
                }

                std::error_code ec;
                std::filesystem::rename(from, to, ec);
                if (ec)
                {
                    // The rewrites already landed. Without putting them back, every
                    // referring file now points at a location the asset is not at --
                    // the exact silent breakage this command set exists to prevent,
                    // and reached by an ordinary Ctrl-Z. CommitMove guards the
                    // forward move this way; the undo path has to as well.
                    RevertRewrites(forward, m_Record.Edits.size());
                    throw std::runtime_error("Cannot move the asset file back to " + to.generic_string() + ": " +
                                             ec.message() + ". The reference rewrites were rolled back, so the "
                                                            "project still points at " +
                                             from.generic_string() + ".");
                }
                if (m_Record.HadSidecar)
                {
                    std::error_code sidecarEc;
                    std::filesystem::rename(SidecarPath(from), SidecarPath(to), sidecarEc);
                    if (sidecarEc)
                        OLO_CORE_ERROR("Asset move left its import-settings sidecar behind at {}",
                                       SidecarPath(from).string());
                }

                if (!m_Record.Registered || !Project::HasAssetManager())
                    return;
                auto manager = Project::GetAssetManager().As<EditorAssetManager>();
                if (!manager)
                    return;
                AssetMetadata metadata = manager->GetMetadata(m_Record.Handle);
                if (!metadata.IsValid())
                    return;
                metadata.FilePath = forward ? m_Record.DestinationKey : m_Record.SourceKey;
                std::error_code stampEc;
                if (const auto stamp = std::filesystem::last_write_time(to, stampEc); !stampEc)
                    metadata.LastWriteTime = stamp;
                manager->SetMetadata(m_Record.Handle, metadata);
                (void)manager->SerializeAssetRegistry();
            }

            MoveRecord m_Record;
        };

        AutomationResult AssetMove(IAutomationHost& host, const Json& args)
        {
            ProjectView project;
            AssetTarget target;
            std::filesystem::path destination;
            const Json prepared = host.MarshalRead([&project, &target, &destination, &args]() -> Json
                                                   {
                if (Json captured = CaptureProject(project); IsFailure(captured))
                    return captured;
                if (Json resolved = ResolveTarget(project, args, target); IsFailure(resolved))
                    return resolved;
                if (!target.ExistsOnDisk)
                    return Failure("The asset file does not exist on disk: " + target.AbsolutePath.generic_string());
                if (Json contained = RequireWritableTarget(project, target, "move"); IsFailure(contained))
                    return contained;
                if (!args.at("destination").is_string())
                    return Failure("destination must be a string.");
                return ValidateDestination(project, args.at("destination").get<std::string>(),
                                           args.value("createDirectories", false), destination); });
            if (IsFailure(prepared))
                return AutomationResult::Error(prepared.at("__error").get<std::string>());

            const AssetIndexScope scope = MakeScope(project);
            const AssetIndex index = ScanProject(host, scope);
            if (host.IsCurrentCallCancelled())
                return AutomationResult::Error("Cancelled while scanning the project.");
            if (!index.Coverage.ScanCompleted())
            {
                return AutomationResult::Structured(
                    Json{ { "moved", false },
                          { "refused", true },
                          { "reason", index.Coverage.Truncated ? "index-truncated" : "scan-incomplete" },
                          { "message", std::string("Refusing to move: the reference scan did not see the whole "
                                                   "project, so the set of references to rewrite is incomplete. ") +
                                           (index.Coverage.Truncated ? std::string("It hit its file limit.")
                                                                     : index.Coverage.IncompleteReason) },
                          { "coverage", DescribeCoverage(index, scope) } });
            }

            const std::vector<AssetReference> referrers =
                FindReferrers(index, target.AbsolutePath, static_cast<u64>(target.Handle));
            std::vector<PendingEdit> edits;
            if (Json planned = PlanReferenceEdits(referrers, target.AbsolutePath, destination, scope, edits);
                IsFailure(planned))
            {
                return AutomationResult::Error(planned.at("__error").get<std::string>());
            }

            if (Json committed = CommitMove(edits, target.AbsolutePath, destination); IsFailure(committed))
                return AutomationResult::Error(committed.at("__error").get<std::string>());

            // Re-key the registry LAST, and in place: the handle is the asset's
            // identity, and every handle-shaped reference in the project depends on
            // it surviving the move. Removing and re-importing would mint a fresh
            // handle and silently orphan all of them.
            //
            // The undo entry is pushed in the same marshaled job, because both the
            // registry and CommandHistory are main-thread-only and a move that was
            // half-recorded would be worse than one that was not recorded at all.
            u32 registryUpdated = 0;
            bool undoable = false;
            std::error_code sidecarCheck;
            const bool hadSidecar = std::filesystem::exists(SidecarPath(destination), sidecarCheck) && !sidecarCheck;
            const Json registry = host.MarshalRead(
                [&host, &project, &target, &destination, &edits, &registryUpdated, &undoable, hadSidecar]() -> Json
                {
                auto manager = EditorManager();
                if (!manager)
                    return Failure("Asset manager disappeared mid-move.");
                const std::filesystem::path destinationKey =
                    EditorAssetManager::MakeRegistryKey(destination, project.Root);
                if (target.Registered)
                {
                    AssetMetadata metadata = manager->GetMetadata(target.Handle);
                    metadata.FilePath = destinationKey;
                    std::error_code ec;
                    if (const auto stamp = std::filesystem::last_write_time(destination, ec); !ec)
                        metadata.LastWriteTime = stamp;
                    manager->SetMetadata(target.Handle, metadata);
                    registryUpdated = 1;
                }
                if (!manager->SerializeAssetRegistry())
                    return Failure("Moved the files, but failed to persist the asset registry.");

                // Only when the host actually has an editor history. A headless
                // caller (oloctl, a test) has none, and the result says
                // undoable:false rather than implying a Ctrl-Z that does not exist.
                const auto getHistory = host.Context().GetCommandHistory;
                if (CommandHistory* history = getHistory ? getHistory() : nullptr; history != nullptr)
                {
                    MoveRecord record;
                    record.Source = target.AbsolutePath;
                    record.Destination = destination;
                    record.Handle = target.Handle;
                    record.SourceKey = target.RegistryKey;
                    record.DestinationKey = destinationKey;
                    record.Registered = target.Registered;
                    record.HadSidecar = hadSidecar;
                    record.Edits = edits;
                    history->PushAlreadyExecuted(std::make_unique<AssetMoveCommand>(std::move(record)));
                    undoable = true;
                }
                return Json::object(); });
            if (IsFailure(registry))
                return AutomationResult::Error(registry.at("__error").get<std::string>());

            Json rewritten = Json::array();
            u32 total = 0;
            for (const PendingEdit& edit : edits)
            {
                rewritten.push_back(Json{ { "file", edit.File.generic_string() },
                                          { "referencesRewritten", edit.ReferencesRewritten } });
                total += edit.ReferencesRewritten;
            }
            const u32 handleReferrers = static_cast<u32>(std::ranges::count_if(
                referrers, [](const AssetReference& reference)
                { return reference.Kind == AssetReferenceKind::Handle; }));

            return AutomationResult::Structured(
                Json{ { "moved", true },
                      { "refused", false },
                      { "handle", std::to_string(static_cast<u64>(target.Handle)) },
                      { "from", target.AbsolutePath.generic_string() },
                      { "to", destination.generic_string() },
                      { "referrerCount", static_cast<u32>(referrers.size()) },
                      { "referencesRewritten", total },
                      { "handleReferencesUnchanged", handleReferrers },
                      { "filesRewritten", std::move(rewritten) },
                      { "registryUpdated", registryUpdated == 1 },
                      { "unscannableFiles", index.Coverage.BinaryFilesSkipped },
                      { "undoable", undoable },
                      { "coverage", DescribeCoverage(index, scope) } });
        }

        // ---- olo_asset_delete --------------------------------------------------

        AutomationResult AssetDelete(IAutomationHost& host, const Json& args)
        {
            ProjectView project;
            AssetTarget target;
            const Json prepared = host.MarshalRead([&project, &target, &args]() -> Json
                                                   {
                if (Json captured = CaptureProject(project); IsFailure(captured))
                    return captured;
                if (Json resolved = ResolveTarget(project, args, target); IsFailure(resolved))
                    return resolved;
                if (!target.ExistsOnDisk)
                    return Failure("The asset file does not exist on disk: " + target.AbsolutePath.generic_string());
                return RequireWritableTarget(project, target, "delete"); });
            if (IsFailure(prepared))
                return AutomationResult::Error(prepared.at("__error").get<std::string>());

            const bool force = args.value("force", false);
            const AssetIndexScope scope = MakeScope(project);
            const AssetIndex index = ScanProject(host, scope);
            if (host.IsCurrentCallCancelled())
                return AutomationResult::Error("Cancelled while scanning the project.");

            const std::vector<AssetReference> referrers =
                FindReferrers(index, target.AbsolutePath, static_cast<u64>(target.Handle));
            if (auto refusal = RefusalForReferrers(index, referrers, target, project, scope, force, "delete"))
            {
                (*refusal)["deleted"] = false;
                return AutomationResult::Structured(*refusal);
            }

            std::error_code ec;
            if (!std::filesystem::remove(target.AbsolutePath, ec) || ec)
            {
                return AutomationResult::Error("Failed to delete " + target.AbsolutePath.generic_string() +
                                               (ec ? ": " + ec.message() : ""));
            }
            // An orphaned sidecar would silently reattach to whatever is created
            // at this path next, which is a stranger failure than losing it.
            std::error_code sidecarEc;
            const bool sidecarRemoved = std::filesystem::remove(SidecarPath(target.AbsolutePath), sidecarEc);

            const Json registry = host.MarshalRead([&target]() -> Json
                                                   {
                auto manager = EditorManager();
                if (!manager)
                    return Failure("Asset manager disappeared mid-delete.");
                if (target.Registered)
                {
                    manager->DeregisterDependencies(target.Handle);
                    manager->RemoveAsset(target.Handle);
                }
                if (!manager->SerializeAssetRegistry())
                    return Failure("Deleted the file, but failed to persist the asset registry.");
                return Json::object(); });
            if (IsFailure(registry))
                return AutomationResult::Error(registry.at("__error").get<std::string>());

            Json broken = Json::array();
            const sizet reported = std::min(referrers.size(), kMaxReportedReferrers);
            for (sizet i = 0; i < reported; ++i)
                broken.push_back(DescribeReference(referrers[i], project));

            return AutomationResult::Structured(
                Json{ { "deleted", true },
                      { "refused", false },
                      { "forced", force && !referrers.empty() },
                      { "handle", std::to_string(static_cast<u64>(target.Handle)) },
                      { "path", target.AbsolutePath.generic_string() },
                      // Named, not just counted: a forced delete's whole cost is
                      // these references, and the caller has to be able to go fix
                      // them.
                      { "referencesBroken", static_cast<u32>(referrers.size()) },
                      { "brokenReferences", std::move(broken) },
                      { "importSettingsRemoved", sidecarRemoved && !sidecarEc },
                      // At the DECISION POINT, not only inside coverage: the scan
                      // cannot read these formats at all, so "0 referrers" is a
                      // statement about the files it could read. A caller deleting
                      // on the strength of an empty list should see this without
                      // having to go digging.
                      { "unscannableFiles", index.Coverage.BinaryFilesSkipped },
                      { "undoable", false },
                      { "coverage", DescribeCoverage(index, scope) } });
        }

        // ---- olo_asset_create --------------------------------------------------

        // AssetType -> "make a default one and serialize it to this path".
        //
        // Every entry is CreateOrReplaceAsset<T>, which writes the file through the
        // type's registered serializer and registers the handle in one step. The
        // set is the types with a default-constructible asset class AND a
        // registered serializer; EditorAssetManager::CreateAsset(type, path) would
        // be the obvious alternative, but it is DECLARED AND NEVER DEFINED (see the
        // PR that added this file), so calling it is a link error.
        using AssetFactory = AssetHandle (*)(EditorAssetManager&, const std::filesystem::path&);
        // Why a type needs its own availability check: MaterialAsset's constructor
        // resolves a shader out of Renderer3D::GetShaderLibrary() and ASSERTS when
        // it finds none, so on a host with no shaders loaded (oloctl, a headless
        // test) calling the factory aborts the process instead of returning an
        // error. Checking first turns that into the refusal every other
        // unavailable path on this surface returns.
        using AssetFactoryAvailable = bool (*)();

        struct AssetFactoryEntry
        {
            AssetType Type;
            AssetFactory Make;
            AssetFactoryAvailable Available; // null => always available.
            const char* Unavailable;         // why not, when Available says no.
        };

        bool ShaderLibraryHasAMaterialShader()
        {
            const ShaderLibrary& library = Renderer3D::GetShaderLibrary();
            return library.Exists("DefaultPBR") || library.Exists("DefaultPBR_Transparent") ||
                   library.Exists("Basic3D");
        }

        template<typename T>
        AssetHandle MakeDefault(EditorAssetManager& manager, const std::filesystem::path& path)
        {
            Ref<T> created = manager.CreateOrReplaceAsset<T>(path);
            return created ? created->GetHandle() : AssetHandle(0);
        }

        // SoundGraph is deliberately absent. SoundGraphAsset holds a
        // Ref<SoundGraph::Prototype> whose type is only forward-declared in its
        // header, so instantiating the asset needs the complete audio-graph
        // internals -- a real dependency of that type, not something a command
        // should route around by pulling the audio subsystem into this TU. It can
        // join the table when its header is self-contained; until then the refusal
        // names what IS supported rather than half-working.
        const std::vector<AssetFactoryEntry>& AssetFactories()
        {
            static const std::vector<AssetFactoryEntry> factories = {
                { AssetType::Material, &MakeDefault<MaterialAsset>, &ShaderLibraryHasAMaterialShader,
                  "creating a Material needs the renderer's shader library (DefaultPBR or Basic3D), and this host "
                  "has none loaded -- run it against the editor" },
                { AssetType::InstancePlacement, &MakeDefault<InstancePlacementAsset>, nullptr, nullptr },
            };
            return factories;
        }

        std::vector<std::string> CreatableTypeNames()
        {
            std::vector<std::string> names;
            for (const AssetFactoryEntry& entry : AssetFactories())
                names.emplace_back(AssetUtils::AssetTypeToString(entry.Type));
            std::ranges::sort(names);
            return names;
        }

        AutomationResult AssetCreate(IAutomationHost& host, const Json& args)
        {
            ProjectView project;
            AssetTarget created;
            const Json result = host.MarshalRead([&project, &created, &args]() -> Json
                                                 {
                if (Json captured = CaptureProject(project); IsFailure(captured))
                    return captured;
                auto manager = EditorManager();
                if (!manager)
                    return Failure("Asset creation requires an editor asset manager.");

                const std::string typeName = args.value("type", std::string{});
                const AssetType type = AssetUtils::AssetTypeFromString(typeName);
                const AssetFactoryEntry* entry = nullptr;
                for (const AssetFactoryEntry& candidate : AssetFactories())
                {
                    if (candidate.Type == type)
                        entry = &candidate;
                }
                if (entry != nullptr && entry->Available != nullptr && !entry->Available())
                    return Failure("Cannot create an asset of type '" + typeName + "': " +
                                   std::string(entry->Unavailable) + ".");
                if (entry == nullptr)
                {
                    std::string supported;
                    for (const std::string& name : CreatableTypeNames())
                        supported += (supported.empty() ? "" : ", ") + name;
                    return Failure("Cannot create an asset of type '" + typeName +
                                   "'. Creating one needs a default-constructible asset class with a registered "
                                   "serializer; this build has: " + supported + ".");
                }

                std::filesystem::path destination;
                if (!args.at("destination").is_string())
                    return Failure("destination must be a string.");
                if (Json validated = ValidateDestination(project, args.at("destination").get<std::string>(),
                                                        args.value("createDirectories", false), destination);
                    IsFailure(validated))
                {
                    return validated;
                }

                // The extension must map back to the SAME type. CreateOrReplaceAsset
                // stamps the type from T and never looks at the extension, so a
                // Material written to "foo.png" serializes happily and is then
                // re-registered as a Texture2D by the next directory scan -- an
                // asset that changes type behind your back, with nothing in the log.
                if (const AssetType fromExtension = AssetExtensions::GetAssetTypeFromPath(destination.string());
                    fromExtension != type)
                {
                    std::string expected;
                    for (const std::string& extension : AssetExtensions::GetExtensionsForAssetType(type))
                        expected += (expected.empty() ? "" : ", ") + extension;
                    return Failure("destination extension '" + destination.extension().string() + "' maps to " +
                                   std::string(AssetUtils::AssetTypeToString(fromExtension)) + ", not " + typeName +
                                   ". Use one of: " + (expected.empty() ? std::string("(none registered)") : expected) +
                                   ".");
                }

                const AssetHandle handle = entry->Make(*manager, destination);
                if (handle == 0)
                    return Failure("The asset was not created: the serializer produced no handle.");
                if (!manager->SerializeAssetRegistry())
                    return Failure("Created the asset, but failed to persist the asset registry.");
                created.Handle = handle;
                created.Registered = true;
                created.Type = type;
                const AssetMetadata metadata = manager->GetMetadata(handle);
                created.RegistryKey = metadata.FilePath;
                created.Status = metadata.Status;
                created.AbsolutePath = Canonicalize(destination);
                std::error_code ec;
                created.ExistsOnDisk = std::filesystem::is_regular_file(created.AbsolutePath, ec) && !ec;
                return Json::object(); });
            if (IsFailure(result))
                return AutomationResult::Error(result.at("__error").get<std::string>());

            Json described = DescribeTarget(created, project);
            described["created"] = true;
            described["undoable"] = false;
            return AutomationResult::Structured(described);
        }

        // ---- olo_asset_import / olo_asset_reimport -----------------------------

        AutomationResult AssetImport(IAutomationHost& host, const Json& args)
        {
            ProjectView project;
            AssetTarget imported;
            bool wasAlreadyRegistered = false;
            const Json result = host.MarshalRead([&project, &imported, &wasAlreadyRegistered, &args]() -> Json
                                                 {
                if (Json captured = CaptureProject(project); IsFailure(captured))
                    return captured;
                auto manager = EditorManager();
                if (!manager)
                    return Failure("Importing requires an editor asset manager.");
                if (!args.at("path").is_string())
                    return Failure("path must be a string.");
                const std::filesystem::path absolute =
                    ResolveProjectPath(project, std::filesystem::path(args.at("path").get<std::string>()));
                std::error_code ec;
                if (!std::filesystem::is_regular_file(absolute, ec) || ec)
                    return Failure("No file to import at: " + absolute.generic_string());
                const std::filesystem::path key = EditorAssetManager::MakeRegistryKey(absolute, project.Root);
                wasAlreadyRegistered = manager->GetAssetHandleFromFilePath(key) != 0;

                const AssetHandle handle = manager->ImportAsset(absolute);
                if (handle == 0)
                {
                    return Failure("Import failed for " + absolute.generic_string() +
                                   ". The extension may not map to a registered asset type "
                                   "(AssetExtensions), or the file is outside the project.");
                }
                if (!manager->SerializeAssetRegistry())
                    return Failure("Imported the asset, but failed to persist the asset registry.");
                imported.Handle = handle;
                imported.Registered = true;
                const AssetMetadata metadata = manager->GetMetadata(handle);
                imported.Type = metadata.Type;
                imported.Status = metadata.Status;
                imported.RegistryKey = metadata.FilePath;
                imported.AbsolutePath = Canonicalize(absolute);
                imported.ExistsOnDisk = true;
                return Json::object(); });
            if (IsFailure(result))
                return AutomationResult::Error(result.at("__error").get<std::string>());

            Json described = DescribeTarget(imported, project);
            described["imported"] = true;
            described["alreadyRegistered"] = wasAlreadyRegistered;
            // Stated in the payload as well as in the command's declared Undo, so
            // an agent reading only the result still learns it cannot take this
            // back. The issue is explicit that an import is not undoable, and
            // pretending otherwise is worse than saying so.
            described["undoable"] = false;
            return AutomationResult::Structured(described);
        }

        AutomationResult AssetReimport(IAutomationHost& host, const Json& args)
        {
            ProjectView project;
            AssetTarget target;
            bool reloaded = false;
            const Json result = host.MarshalRead([&project, &target, &reloaded, &args]() -> Json
                                                 {
                if (Json captured = CaptureProject(project); IsFailure(captured))
                    return captured;
                if (Json resolved = ResolveTarget(project, args, target); IsFailure(resolved))
                    return resolved;
                if (!target.Registered)
                    return Failure("That asset is not in the registry. Use olo_asset_import first.");
                if (!target.ExistsOnDisk)
                    return Failure("The asset file does not exist on disk: " + target.AbsolutePath.generic_string());
                auto manager = EditorManager();
                if (!manager)
                    return Failure("Reimport requires an editor asset manager.");
                reloaded = manager->ReloadData(target.Handle);
                const AssetMetadata metadata = manager->GetMetadata(target.Handle);
                target.Status = metadata.Status;
                return Json::object(); });
            if (IsFailure(result))
                return AutomationResult::Error(result.at("__error").get<std::string>());

            Json described = DescribeTarget(target, project);
            described["reimported"] = reloaded;
            described["undoable"] = false;
            if (!reloaded)
            {
                // ReloadData returning false is a real failure, not a no-op, and it
                // is invisible in the editor: the old bytes stay live. Say so.
                described["message"] =
                    "ReloadData returned false: the asset was NOT reloaded and the previously loaded data is still "
                    "in use. Check olo_assets_problems and the log for the underlying loader error.";
            }
            return AutomationResult::Structured(described);
        }

        // ---- olo_asset_import_settings ----------------------------------------

        // Per-asset import settings have no store in the engine: AssetMetadata is
        // handle/type/path/status/mtime, and the one import option that exists
        // (MeshImportOptions::FlipUV) is passed per call and never persisted. So
        // this command owns a sidecar.
        //
        // ".oloimport" is deliberately NOT in the asset extension map. That is what
        // makes writing one safe: DecideFileWatchAction ignores any path whose
        // extension resolves to AssetType::None, so a sidecar landing next to an
        // asset cannot trigger the auto-import of a stray file -- the exact hazard
        // this epic was warned about.
        AutomationResult AssetImportSettings(IAutomationHost& host, const Json& args)
        {
            ProjectView project;
            AssetTarget target;
            const Json prepared = host.MarshalRead([&project, &target, &args]() -> Json
                                                   {
                if (Json captured = CaptureProject(project); IsFailure(captured))
                    return captured;
                return ResolveTarget(project, args, target); });
            if (IsFailure(prepared))
                return AutomationResult::Error(prepared.at("__error").get<std::string>());

            const std::filesystem::path sidecar = SidecarPath(target.AbsolutePath);
            FileContents existing;
            try
            {
                existing = ReadFileContents(sidecar);
            }
            catch (const std::exception& error)
            {
                return AutomationResult::Error(std::string("Cannot read import settings: ") + error.what());
            }

            Json settings = Json::object();
            if (existing)
            {
                settings = Json::parse(*existing, nullptr, false);
                if (settings.is_discarded() || !settings.is_object())
                {
                    // Refused on a READ too, not just a write: reporting an empty
                    // settings object for a file that plainly holds something would
                    // be a silent fallback, and the caller would then write over it.
                    return AutomationResult::Error("The import-settings sidecar is not a JSON object: " +
                                                   sidecar.generic_string() +
                                                   ". Refusing to read or overwrite it; fix or remove it by hand.");
                }
            }

            const bool writing = args.contains("settings");
            if (!writing)
            {
                return AutomationResult::Structured(Json{ { "asset", DescribeTarget(target, project) },
                                                          { "sidecar", sidecar.generic_string() },
                                                          { "exists", existing.has_value() },
                                                          { "settings", std::move(settings) },
                                                          { "changed", false },
                                                          { "undoable", false } });
            }
            if (!args.at("settings").is_object())
                return AutomationResult::Error("settings must be a JSON object.");
            // Reading settings for any path is harmless; WRITING a sidecar next to
            // an arbitrary file on the machine is not.
            if (Json contained = RequireWritableTarget(project, target, "write import settings for");
                IsFailure(contained))
            {
                return AutomationResult::Error(contained.at("__error").get<std::string>());
            }

            // Merge rather than replace, so setting one key does not silently drop
            // every other. A null VALUE removes its key -- the only way to unset
            // one, and explicit rather than inferred from absence.
            for (const auto& [key, value] : args.at("settings").items())
            {
                if (value.is_null())
                    settings.erase(key);
                else
                    settings[key] = value;
            }
            const std::string replacement = settings.dump(2) + "\n";
            const bool changed = !existing || *existing != replacement;
            if (changed)
            {
                try
                {
                    ReplaceFileContents(sidecar, existing, replacement);
                }
                catch (const std::exception& error)
                {
                    return AutomationResult::Error(std::string("Cannot write import settings: ") + error.what());
                }
            }
            return AutomationResult::Structured(Json{ { "asset", DescribeTarget(target, project) },
                                                      { "sidecar", sidecar.generic_string() },
                                                      { "exists", true },
                                                      { "settings", std::move(settings) },
                                                      { "changed", changed },
                                                      // Nothing reads these yet: the engine has no per-asset import
                                                      // settings pipeline, so this command stores and returns them
                                                      // and no importer consults them. Said here rather than
                                                      // implied, because a settings write that looks like it took
                                                      // effect and did not is exactly a silent fallback.
                                                      { "appliedByImporter", false },
                                                      { "undoable", false } });
        }

        // ---- schemas + registration -------------------------------------------

        Schema::Node TargetSelector()
        {
            return Schema::Object()
                .Prop("handle", Schema::String().Desc("Decimal asset handle. Provide this or 'path', not both."))
                .Prop("path", Schema::String().Desc(
                                  "Asset path, project-relative (e.g. 'Assets/Textures/Foo.png') or absolute. "
                                  "Provide this or 'handle', not both."));
        }

        Schema::Node AssetDescriptionSchema()
        {
            return Schema::Object()
                .Prop("handle", Schema::String())
                .Prop("registered", Schema::Bool().Desc("Whether the asset registry knows this file."))
                .Prop("type", Schema::String())
                .Prop("path", Schema::String().Desc("Registry key (project-relative)."))
                .Prop("absolutePath", Schema::String())
                .Prop("name", Schema::String())
                .Prop("existsOnDisk", Schema::Bool())
                .Prop("status", Schema::String())
                .Prop("sizeBytes", Schema::Int().Min(0))
                .Prop("inProject", Schema::Bool());
        }

        Schema::Node CoverageSchema()
        {
            return Schema::Object()
                .Prop("filesScanned", Schema::Int().Min(0))
                .Prop("referencesFound", Schema::Int().Min(0))
                .Prop("scannedExtensions", Schema::Array(Schema::String()))
                .Prop("binaryFilesSkipped", Schema::Int().Min(0).Desc(
                                                "Files under the project with an importable but non-text format. "
                                                "These are NOT searched; a reference inside one is invisible here."))
                .Prop("binaryExtensionsSkipped", Schema::Array(Schema::String()))
                .Prop("unreadableFiles", Schema::Array(Schema::String()))
                .Prop("unresolvedReferences", Schema::Int().Min(0).Desc(
                                                  "Path values that resolve to no file on disk -- broken today."))
                .Prop("truncated", Schema::Bool().Desc(
                                       "The scan hit its file limit. The reference set is INCOMPLETE."))
                .Prop("complete", Schema::Bool().Desc(
                                      "False when the scan could not see the whole project for any other reason "
                                      "-- an unusable root, a walk that could not finish, an unreadable file."))
                .Prop("incompleteReason", Schema::String())
                .Prop("scanCompleted", Schema::Bool().Desc(
                                           "complete AND not truncated: the scan finished over the formats it can "
                                           "read. Every destructive command refuses when this is false, and "
                                           "'force' does not waive it. It is NOT a promise that nothing references "
                                           "the asset -- see binaryFilesSkipped for the formats no scan here can "
                                           "read."))
                .Prop("projectRoot", Schema::String())
                .Prop("assetDirectory", Schema::String().Desc("The second resolution anchor; see 'note'."))
                .Prop("extraBaseDirectories", Schema::Array(Schema::String()))
                .Prop("handleKeyFiltered", Schema::Bool())
                .Prop("note", Schema::String());
        }

        Schema::Node ReferenceSchema()
        {
            return Schema::Object()
                .Prop("file", Schema::String().Desc("Absolute path of the file holding the reference."))
                .Prop("projectPath", Schema::String())
                .Prop("line", Schema::Int().Min(1))
                .Prop("key", Schema::String().Desc("YAML key; 'Materials/0' for an indexed map entry."))
                .Prop("value", Schema::String().Desc("The reference text exactly as the file spells it."))
                .Prop("kind", Schema::String().Enum({ "path", "handle" }))
                .Prop("resolved", Schema::Bool())
                .Prop("resolvedFile", Schema::String());
        }

        // `idempotent` defaults to "read-only commands are, writes are not", which
        // is true of every command here except olo_asset_import: importing a file
        // that is already registered returns its existing handle and changes
        // nothing, so advertising false there would talk a client out of a retry
        // that is perfectly safe.
        void Register(AutomationRegistry& registry, std::string name, std::string title, std::string description,
                      Schema::Node input, Schema::Node output, AutomationHandler handler, bool projectWrite,
                      AutomationUndo undo, bool destructive, std::optional<bool> idempotent = std::nullopt)
        {
            AutomationCommand command;
            command.Name = std::move(name);
            command.Title = std::move(title);
            command.Description = std::move(description);
            command.Toolset = "assets";
            command.InputSchema = input.NoAdditional();
            command.OutputSchema = output;
            command.Annotations = Json{ { "readOnlyHint", !projectWrite },
                                        { "destructiveHint", destructive },
                                        { "idempotentHint", idempotent.value_or(!projectWrite) },
                                        { "openWorldHint", false } };
            command.ProjectWrite = projectWrite;
            // NOT MainMarshaled. These commands marshal the registry reads and
            // writes themselves and run the project scan in between, on the handler
            // thread -- a whole-project walk inside one marshaled job would hold the
            // game thread for its duration and trip the 5s marshal timeout.
            command.MainMarshaled = false;
            command.Undo = undo;
            command.Handler = std::move(handler);
            registry.Register(std::move(command));
        }
    } // namespace

    void RegisterAssetAuthoringCommands(AutomationRegistry& registry)
    {
        Register(registry, "olo_asset_get", "Get asset metadata",
                 "Read one asset's registry metadata: handle, type, project-relative path, on-disk state and size. "
                 "Accepts either a handle or a path, and answers for an unregistered file too.",
                 TargetSelector(), AssetDescriptionSchema(), AssetGet, false, AutomationUndo::None, false);

        Register(registry, "olo_asset_references", "Find asset references",
                 "Answer 'what references this asset' (direction=referrers, the default) or 'what does this asset "
                 "need' (direction=dependencies), WITHOUT opening the editor. Derived by scanning the project's text "
                 "asset files -- scenes, materials, prefabs -- because AssetRegistry.oar is binary and the "
                 "AssetManager dependency graph covers neither scenes nor unloaded assets. Always read 'coverage': "
                 "an empty list means nothing was found in the scanned set, not that nothing references the asset.",
                 TargetSelector().Prop("direction", Schema::String()
                                                        .Enum({ "referrers", "dependencies" })
                                                        .Desc("Default 'referrers'.")),
                 Schema::Object()
                     .Prop("asset", AssetDescriptionSchema())
                     .Prop("direction", Schema::String())
                     .Prop("count", Schema::Int().Min(0))
                     .Prop("returned", Schema::Int().Min(0))
                     .Prop("listTruncated", Schema::Bool())
                     .Prop("references", Schema::Array(ReferenceSchema()))
                     .Prop("coverage", CoverageSchema())
                     .Required({ "asset", "direction", "count", "references", "coverage" }),
                 AssetReferences, false, AutomationUndo::None, false);

        Register(registry, "olo_asset_create", "Create asset",
                 "Create a new, default-valued asset at a project path and register it. The destination must be "
                 "inside the project asset directory and must not already exist. Only types with a "
                 "default-constructible asset class and a registered serializer can be created; the error names "
                 "them. NOT undoable -- the file is written through the asset serializer, outside the editor's "
                 "undo stack.",
                 Schema::Object()
                     .Prop("type", Schema::String().Desc("Asset type name, e.g. 'Material'."))
                     .Prop("destination", Schema::String().Desc(
                                              "Project-relative or absolute destination path inside the asset "
                                              "directory."))
                     .Prop("createDirectories", Schema::Bool().Desc("Create the destination directory when it does not exist. Default false: a typo would otherwise silently create a tree."))
                     .Required({ "type", "destination" }),
                 AssetDescriptionSchema().Prop("created", Schema::Bool()).Prop("undoable", Schema::Bool()),
                 AssetCreate, true, AutomationUndo::Irreversible, false);

        Register(registry, "olo_asset_move", "Move or rename asset",
                 "Move or rename an asset AND rewrite every reference to it, so referring scenes and materials keep "
                 "resolving. A rename is a move within the same directory. Each reference is re-spelled in the style "
                 "its own file used; handle references need no change and are reported separately. All-or-nothing: "
                 "if any reference cannot be rewritten, nothing is written. The asset keeps its handle. ONE editor "
                 "undo step when the host has an editor history (result says undoable); undo refuses if a referring "
                 "file changed in the meantime rather than clobbering the change. A headless host has no history, "
                 "and then undoable is false -- rerun the command in reverse to move it back.",
                 TargetSelector().Prop("destination",
                                       Schema::String().Desc("New project-relative or absolute path, inside the "
                                                             "project asset directory. Must not already exist."))
                     .Prop("createDirectories", Schema::Bool().Desc("Create the destination directory when it does not exist. Default false: a typo would otherwise silently create a tree."))
                     .Required({ "destination" }),
                 Schema::Object()
                     .Prop("moved", Schema::Bool())
                     .Prop("refused", Schema::Bool())
                     .Prop("reason", Schema::String())
                     .Prop("handle", Schema::String())
                     .Prop("from", Schema::String())
                     .Prop("to", Schema::String())
                     .Prop("referrerCount", Schema::Int().Min(0))
                     .Prop("referencesRewritten", Schema::Int().Min(0))
                     .Prop("handleReferencesUnchanged", Schema::Int().Min(0))
                     .Prop("filesRewritten", Schema::Array(Schema::Object()
                                                               .Prop("file", Schema::String())
                                                               .Prop("referencesRewritten", Schema::Int().Min(0))))
                     .Prop("registryUpdated", Schema::Bool())
                     .Prop("unscannableFiles", Schema::Int().Min(0).Desc(
                                                   "Files in formats this scan cannot read; a reference from one "
                                                   "of them was not rewritten because it was never seen."))
                     .Prop("undoable", Schema::Bool())
                     .Prop("coverage", CoverageSchema())
                     .Required({ "moved", "refused", "coverage" }),
                 AssetMove, true, AutomationUndo::EditorUndoStack, true);

        Register(registry, "olo_asset_delete", "Delete asset",
                 "Delete an asset file and drop it from the registry. REFUSED BY DEFAULT when anything references "
                 "it, and the refusal names every referrer -- deleting a referenced asset is data loss with a "
                 "delayed symptom (the scene still loads, the mesh renders untextured, nothing says why). "
                 "force:true proceeds and reports exactly which references it broke. A truncated reference scan is "
                 "refused regardless of force. NOT undoable.",
                 TargetSelector().Prop("force", Schema::Bool().Desc(
                                                    "Delete even though references exist, and report what broke. "
                                                    "Default false.")),
                 Schema::Object()
                     .Prop("deleted", Schema::Bool())
                     .Prop("refused", Schema::Bool())
                     .Prop("reason", Schema::String().Enum({ "referenced", "index-truncated", "scan-incomplete" }))
                     .Prop("message", Schema::String())
                     .Prop("forced", Schema::Bool())
                     .Prop("handle", Schema::String())
                     .Prop("path", Schema::String())
                     .Prop("asset", AssetDescriptionSchema())
                     .Prop("referrerCount", Schema::Int().Min(0))
                     .Prop("referrers", Schema::Array(ReferenceSchema()))
                     .Prop("referencesBroken", Schema::Int().Min(0))
                     .Prop("brokenReferences", Schema::Array(ReferenceSchema()))
                     .Prop("importSettingsRemoved", Schema::Bool().Desc(
                                                        "Whether an .oloimport sidecar was removed alongside."))
                     .Prop("unscannableFiles", Schema::Int().Min(0).Desc(
                                                   "Files in formats this scan cannot read (see "
                                                   "coverage.binaryExtensionsSkipped). A reference from one of "
                                                   "them is invisible here, so an empty referrer list is a "
                                                   "statement about the files that COULD be read."))
                     .Prop("undoable", Schema::Bool())
                     .Prop("coverage", CoverageSchema())
                     .Required({ "deleted", "refused", "coverage" }),
                 AssetDelete, true, AutomationUndo::Irreversible, true);

        Register(registry, "olo_asset_import", "Import asset",
                 "Register a file on disk as a project asset, minting its handle. Idempotent: importing an "
                 "already-registered file returns its existing handle and reports alreadyRegistered. NOT undoable, "
                 "and deliberately not pretending to be -- an import mutates the persisted registry.",
                 Schema::Object()
                     .Prop("path", Schema::String().Desc("Project-relative or absolute path of the file to import."))
                     .Required({ "path" }),
                 AssetDescriptionSchema()
                     .Prop("imported", Schema::Bool())
                     .Prop("alreadyRegistered", Schema::Bool())
                     .Prop("undoable", Schema::Bool()),
                 AssetImport, true, AutomationUndo::Irreversible, false, /*idempotent*/ true);

        Register(registry, "olo_asset_reimport", "Reimport asset",
                 "Reload a registered asset's data from disk (the hot-reload path). Reports reimported:false with "
                 "an explanation when the reload fails -- a failed reload leaves the previously loaded data live, "
                 "which is invisible in the editor. NOT undoable.",
                 TargetSelector(),
                 AssetDescriptionSchema()
                     .Prop("reimported", Schema::Bool())
                     .Prop("message", Schema::String())
                     .Prop("undoable", Schema::Bool()),
                 AssetReimport, true, AutomationUndo::Irreversible, false);

        Register(registry, "olo_asset_import_settings", "Get or set import settings",
                 "Read (omit 'settings') or merge (provide 'settings') an asset's per-asset import settings, stored "
                 "in a '<asset>.oloimport' JSON sidecar. A null value removes its key. NOTE appliedByImporter is "
                 "false: the engine has no per-asset import-settings pipeline yet, so these are stored and returned "
                 "but no importer consults them. NOT undoable.",
                 TargetSelector().Prop("settings", Schema::Object().Desc(
                                                       "Keys to merge into the sidecar. Omit to read. A null value "
                                                       "removes that key.")),
                 Schema::Object()
                     .Prop("asset", AssetDescriptionSchema())
                     .Prop("sidecar", Schema::String())
                     .Prop("exists", Schema::Bool())
                     .Prop("settings", Schema::Object())
                     .Prop("changed", Schema::Bool())
                     .Prop("appliedByImporter", Schema::Bool())
                     .Prop("undoable", Schema::Bool())
                     .Required({ "asset", "sidecar", "exists", "settings", "changed" }),
                 AssetImportSettings, true, AutomationUndo::Irreversible, false);
    }
} // namespace OloEngine::Automation
