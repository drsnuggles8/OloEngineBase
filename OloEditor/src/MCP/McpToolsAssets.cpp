#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpAssetPackBuild.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Task/NamedThreads.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <fstream>

// Asset MCP tools: olo_assets_list and olo_assets_problems, reading the
// project's asset registry. Split out of the McpTools.cpp monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
        struct PackJob
        {
            std::atomic<float> Progress{ 0 };
            std::atomic<bool> Cancel{ false };
            std::atomic<bool> Done{ false };
            std::string Id;
            Json Result;
        };
        struct PackBuildService
        {
            std::mutex Mutex;
            std::shared_ptr<PackJob> Job;
            std::thread Worker;
            ~PackBuildService()
            {
                if (Job)
                    Job->Cancel.store(true);
                if (Worker.joinable())
                    Worker.join();
            }
        };
        // Called by EditorLayer on its game thread, before any renderer/project teardown.
        void CancelAndWaitPackBuild(PackBuildService& service)
        {
            std::scoped_lock lock(service.Mutex);
            if (service.Job)
            {
                service.Job->Cancel.store(true);
                while (!service.Job->Done.load(std::memory_order_acquire))
                {
                    Tasks::FNamedThreadManager::Get().ProcessTasks(Tasks::ENamedThread::GameThread);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (service.Worker.joinable())
                service.Worker.join();
        }
        Json PackSnapshot(const PackJob& job)
        {
            const bool done = job.Done.load(std::memory_order_acquire);
            Json result{ { "operationId", job.Id }, { "status", done ? (job.Result.value("success", false) ? "succeeded" : "failed") : "running" }, { "progress", job.Progress.load() }, { "cancelRequested", job.Cancel.load() } };
            if (done)
                result["result"] = job.Result;
            return result;
        }
        ToolResult Handle_AssetPackBuild(IAutomationHost& host, const Json& args, const std::shared_ptr<PackBuildService>& serviceOwner)
        {
            auto& service = *serviceOwner;
            std::scoped_lock lock(service.Mutex);
            if (args.contains("operationId"))
            {
                if (args.contains("output") || args.contains("compress"))
                    return ToolResult::Error("Polling/cancelling must not specify output or compress.");
                if (!service.Job || service.Job->Id != args["operationId"].get<std::string>())
                    return ToolResult::Error("Unknown asset-pack operationId (only the latest build is retained).");
                if (args.value("cancel", false))
                    service.Job->Cancel.store(true);
                return ToolResult::Structured(PackSnapshot(*service.Job));
            }
            if (!args.contains("output") || args.contains("cancel"))
                return ToolResult::Error("Start requires output; cancel requires operationId.");
            if (service.Job && !service.Job->Done.load(std::memory_order_acquire))
                return ToolResult::Error("An asset-pack build is already running; poll its operationId.");
            std::filesystem::path output;
            Ref<Project> project;
            std::shared_ptr<void> projectLease;
            const Json prepared = host.MarshalRead([&host, &output, &project, &args, &projectLease, &serviceOwner]() -> Json
                                                   {
                project = Project::GetActive();
                if (!project) return {{"error", "No active project."}};
                if (auto error = AssetPackBuild::ResolveOutput(Project::GetProjectDirectory(), args["output"].get<std::string>(), output))
                    return {{"error", *error}};
                if (host.Context().IsPlaying && host.Context().IsPlaying())
                    return {{"error", "Stop Play/Simulate before building an authored asset pack."}};
                const auto& document = host.Context().SceneDocument;
                auto* history = host.Context().GetCommandHistory ? host.Context().GetCommandHistory() : nullptr;
                if (!document.Capture || !document.Install || !history)
                    return {{"error", "Asset-pack build requires the GUI editor's scene-document save hooks; unavailable in headless attach."}};
                if (!host.Context().AcquireAssetPackBuildLease)
                    return {{"error", "Host lacks the asset-pack project/shutdown lifecycle guard."}};
                projectLease = host.Context().AcquireAssetPackBuildLease([weak = std::weak_ptr<PackBuildService>(serviceOwner)]
                {
                    if (auto owner = weak.lock()) CancelAndWaitPackBuild(*owner);
                });
                if (!projectLease) return {{"error", "Another project build operation holds the project lease."}};
                const auto snapshot = document.Capture();
                if (snapshot.Path.empty()) return {{"error", "Save the scene to a project path before building (no Save As dialog is opened)."}};
                (void)Automation::SaveSceneDocument(document, *history);
                auto manager = Project::HasAssetManager() ? Project::GetAssetManager().As<EditorAssetManager>() : nullptr;
                const auto savedHandle = manager ? manager->ImportAsset(snapshot.Path) : AssetHandle(0);
                // Scene serialization reads the global manager, so refresh its
                // cached scene now rather than waiting for filewatch hot reload.
                if (!manager || static_cast<u64>(savedHandle) == 0 || !manager->ReloadData(savedHandle) || !manager->SerializeAssetRegistry())
                    return {{"error", "Could not persist the asset registry before packing."}};
                return {{"saved", true}}; });
            if (prepared.contains("error"))
                return ToolResult::Error(prepared["error"].get<std::string>());
            if (service.Worker.joinable())
                service.Worker.join();
            auto job = std::make_shared<PackJob>();
            job->Id = "pack-" + std::to_string(NextCaptureSequence());
            AssetPackBuilder::BuildSettings settings;
            settings.m_OutputPath = output;
            settings.m_CompressAssets = args.value("compress", true);
            // This tool writes a pack only; distribution sidecars belong to Build Game.
            settings.m_IncludeLocalizationFiles = false;
            service.Worker = std::thread([job, settings, project, projectLease = std::move(projectLease)]() mutable
                                         {
                try
                {
                    const std::filesystem::path logPath = Log::Get().GetLogFileName();
                    std::error_code ec;
                    const auto logStart = std::filesystem::file_size(logPath, ec);
                    const bool logReadable = !ec;
                    auto built = AssetPackBuilder::BuildFromActiveProject(settings, job->Progress, &job->Cancel);
                    job->Result = AssetPackBuild::ResultJson(built);
                    job->Result["warnings"] = Json::array();
                    job->Result["warningsScope"] = "All warning/error log lines emitted during this build interval, including concurrent editor diagnostics.";
                    std::ifstream log(logPath);
                    if (logReadable && log)
                    {
                        log.seekg(static_cast<std::streamoff>(logStart));
                        const auto logEnd = std::filesystem::file_size(logPath, ec);
                        std::string line;
                        while (!ec && log.tellg() >= 0 && static_cast<u64>(log.tellg()) < logEnd && std::getline(log, line))
                            if (line.find("[warning]") != std::string::npos || line.find("[error]") != std::string::npos || line.find("[critical]") != std::string::npos)
                                job->Result["warnings"].push_back(line);
                        job->Result["warningsAvailable"] = !ec && !log.bad();
                    }
                    else job->Result["warningsAvailable"] = false;
                    job->Result["sceneSaved"] = true;
                }
                catch (const std::exception& e)
                {
                    job->Result = {{"success", false}, {"error", e.what()}};
                }
                projectLease.reset();
                job->Done.store(true, std::memory_order_release); });
            service.Job = job;
            return ToolResult::Structured(PackSnapshot(*job));
        }

        // ---- olo_assets_list (main-marshaled; reads the project asset registry) -
        ToolResult Handle_AssetsList(IAutomationHost& host, const Json& args)
        {
            std::string typeFilter;
            if (args.contains("typeFilter") && args["typeFilter"].is_string())
                typeFilter = args["typeFilter"].get<std::string>();
            // Search by name / path (issue #1128 slice 1). Added here rather than
            // as a second olo_asset_find command: the filters are the only thing
            // that would have differed, and a near-duplicate of a listing command
            // is exactly the surface bloat exposure profiles (#1124) exist to
            // undo. Both are case-insensitive substring matches, not globs --
            // a substring is what an agent that half-remembers a filename needs,
            // and it cannot fail in the surprising ways an unanchored glob does.
            std::string namePattern;
            if (args.contains("namePattern") && args["namePattern"].is_string())
                namePattern = args["namePattern"].get<std::string>();
            std::string pathPattern;
            if (args.contains("pathPattern") && args["pathPattern"].is_string())
                pathPattern = args["pathPattern"].get<std::string>();
            int page = 0;
            int pageSize = 50;
            if (args.contains("page") && args["page"].is_number_integer())
                // CLAMPED, not floored: Pagination() declares `minimum: 0` and no
                // maximum, so a legal 2147483648 narrows to int as -2147483648 on
                // MSVC, `start` goes hugely negative and the loop below indexes
                // the vector far out of bounds. Found by review on #1130, which
                // had copied this shape.
                page = static_cast<int>(std::clamp<long long>(args["page"].get<long long>(), 0, 1'000'000));
            if (args.contains("pageSize") && args["pageSize"].is_number_integer())
                pageSize = static_cast<int>(std::clamp<long long>(args["pageSize"].get<long long>(), 1, 200));

            const Json result = host.MarshalRead([typeFilter, namePattern, pathPattern, page, pageSize]() -> Json
                                                 {
                const Ref<AssetManagerBase> mgr = Project::GetAssetManager();
                if (!mgr)
                    return Json{ { "__error", "No active project / asset manager." } };

                std::vector<AssetHandle> handles;
                if (!typeFilter.empty())
                {
                    const AssetType type = AssetUtils::AssetTypeFromString(typeFilter);
                    if (type == AssetType::None)
                        return Json{ { "__error", "Unknown asset type: " + typeFilter } };
                    for (const AssetHandle h : mgr->GetAllAssetsWithType(type))
                        handles.push_back(h);
                }
                else
                {
                    std::unordered_set<u64> seen;
                    constexpr u16 kMaxType = static_cast<u16>(AssetType::CinematicSequence);
                    for (u16 ti = 1; ti <= kMaxType; ++ti)
                    {
                        for (const AssetHandle h : mgr->GetAllAssetsWithType(static_cast<AssetType>(ti)))
                        {
                            if (seen.insert(static_cast<u64>(h)).second)
                                handles.push_back(h);
                        }
                    }
                }

                // Filter BEFORE paginating, so `total` and `nextPage` describe the
                // filtered set. Filtering the page instead would make a search
                // that matches one asset on page 3 look like it matched nothing.
                if (!namePattern.empty() || !pathPattern.empty())
                {
                    const auto lower = [](std::string text)
                    {
                        std::ranges::transform(text, text.begin(), [](unsigned char ch)
                                               { return static_cast<char>(std::tolower(ch)); });
                        return text;
                    };
                    const std::string wantedName = lower(namePattern);
                    const std::string wantedPath = lower(pathPattern);
                    std::erase_if(handles, [&](AssetHandle handle)
                    {
                        const AssetMetadata meta = mgr->GetAssetMetadata(handle);
                        if (!wantedName.empty() &&
                            lower(meta.FilePath.filename().string()).find(wantedName) == std::string::npos)
                            return true;
                        if (!wantedPath.empty() &&
                            lower(meta.FilePath.generic_string()).find(wantedPath) == std::string::npos)
                            return true;
                        return false;
                    });
                }

                std::sort(handles.begin(), handles.end(),
                          [](AssetHandle a, AssetHandle b) { return static_cast<u64>(a) < static_cast<u64>(b); });

                const auto total = static_cast<int>(handles.size());
                // 64-bit to avoid int overflow when a large page is requested.
                const long long start = static_cast<long long>(page) * pageSize;
                Json assets = Json::array();
                for (long long i = start; i < total && i < start + pageSize; ++i)
                {
                    const AssetMetadata meta = mgr->GetAssetMetadata(handles[static_cast<sizet>(i)]);
                    assets.push_back(Json{ { "handle", std::to_string(static_cast<u64>(handles[static_cast<sizet>(i)])) },
                                           { "type", AssetUtils::AssetTypeToString(meta.Type) },
                                           { "path", meta.FilePath.generic_string() },
                                           { "name", meta.FilePath.filename().string() } });
                }

                Json out;
                out["total"] = total;
                out["page"] = page;
                out["pageSize"] = pageSize;
                out["returned"] = static_cast<int>(assets.size());
                if (start + pageSize < total)
                    out["nextPage"] = page + 1;
                out["assets"] = std::move(assets);
                return out; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_assets_problems (main-marshaled; failed/missing/invalid assets) -
        ToolResult Handle_AssetsProblems(IAutomationHost& host, const Json& /*args*/)
        {
            const Json result = host.MarshalRead([]() -> Json
                                                 {
                const Ref<AssetManagerBase> mgr = Project::GetAssetManager();
                if (!mgr)
                    return Json{ { "__error", "No active project / asset manager." } };

                std::unordered_set<u64> seen;
                Json problems = Json::array();
                constexpr u16 kMaxType = static_cast<u16>(AssetType::CinematicSequence);
                for (u16 ti = 1; ti <= kMaxType; ++ti)
                {
                    for (const AssetHandle h : mgr->GetAllAssetsWithType(static_cast<AssetType>(ti)))
                    {
                        if (!seen.insert(static_cast<u64>(h)).second)
                            continue;
                        const AssetMetadata meta = mgr->GetAssetMetadata(h);
                        if (!AssetStatusUtils::IsStatusError(meta.Status))
                            continue;
                        problems.push_back(Json{ { "handle", std::to_string(static_cast<u64>(h)) },
                                                 { "type", AssetUtils::AssetTypeToString(meta.Type) },
                                                 { "path", meta.FilePath.generic_string() },
                                                 { "status", AssetStatusUtils::AssetStatusToString(meta.Status) } });
                    }
                }
                return Json{ { "count", static_cast<int>(problems.size()) }, { "problems", std::move(problems) } }; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

    } // namespace

    // Composed by olo_project_validate (#1130) — the SAME handler the standalone
    // command registers, so the two cannot disagree about what an asset problem is.
    ToolResult CollectAssetProblems(IAutomationHost& host)
    {
        return Handle_AssetsProblems(host, Json::object());
    }

    void RegisterAssetTools(AutomationRegistry& registry)
    {
        {
            auto service = std::make_shared<PackBuildService>();
            ToolDef tool;
            tool.Name = "olo_asset_pack_build";
            tool.Title = "Build asset pack";
            tool.Toolset = "assets";
            tool.ProjectWrite = true;
            tool.Annotations = DestructiveMutatingAnnotations();
            tool.Description = "Start an asynchronous AssetPackBuilder build with {output,compress}; output is a .olopack path under the active project (absolute/traversal/junction escapes refused). Auto-saves the current scene and registry. GUI Edit mode only; headless attach lacks document-save hooks. Returns operationId/progress immediately; poll with {operationId}, cancel with {operationId,cancel:true}. Completed result lists actual serialized assets, texture cooked format/sRGB and warning/error log lines during the build interval (including concurrent editor diagnostics). Only the latest operation is retained. Project switching is refused while the build holds its editor lease; shutdown cancels and drains the worker before teardown. This writes a pack, not a runnable distribution: no manifest/start-scene/shader/runtime assembly or separate process launch. Use Build Game for distribution; OloRuntime loads Assets/AssetPack.olopack under its game directory.";
            tool.InputSchema = AssetPackBuild::InputSchema();
            tool.OutputSchema = Schema::Object().Prop("operationId", Schema::String()).Prop("status", Schema::String().Enum({ "running", "succeeded", "failed" })).Prop("progress", Schema::Number()).Prop("cancelRequested", Schema::Bool()).Prop("result", Schema::Object()).Required({ "operationId", "status", "progress", "cancelRequested" });
            tool.MainMarshaled = true;
            tool.Handler = [service](IAutomationHost& host, const Json& args)
            { return Handle_AssetPackBuild(host, args, service); };
            registry.Register(std::move(tool));
        }
        {
            ToolDef tool;
            tool.Name = "olo_assets_list";
            tool.Toolset = "assets";
            tool.Title = "List assets";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Search / list the project's registered assets (paginated): handle, type, project-relative path, "
                "and filename. Filter by asset type (e.g. Texture2D, Mesh, Material, Scene, Script), by filename "
                "substring, and by path substring; the filters combine with AND. To find out what REFERENCES an "
                "asset before touching it, use olo_asset_references.";
            tool.InputSchema = Schema::Object()
                                   .Prop("typeFilter", Schema::String().Desc("Asset type name to filter by (e.g. 'Texture2D'). Omit for all types."))
                                   .Prop("namePattern", Schema::String().Desc("Case-insensitive substring of the FILENAME. Omit for any."))
                                   .Prop("pathPattern", Schema::String().Desc("Case-insensitive substring of the project-relative PATH. Omit for any."))
                                   .Pagination("Assets per page (default 50, max 200).")
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("total", Schema::Int().Min(0).Desc("Total registered assets matching every supplied filter."))
                                    .Prop("page", Schema::Int().Min(0))
                                    .Prop("pageSize", Schema::Int().Min(1))
                                    .Prop("returned", Schema::Int().Min(0).Desc("Number of entries in 'assets'."))
                                    .Prop("nextPage", Schema::Int().Min(1).Desc("Next zero-based page index; omitted on the last page."))
                                    .Prop("assets", Schema::Array(Schema::Object()
                                                                      .Prop("handle", Schema::String().Desc("Asset handle (decimal u64)."))
                                                                      .Prop("type", Schema::String())
                                                                      .Prop("path", Schema::String().Desc("Project-relative path."))
                                                                      .Prop("name", Schema::String())))
                                    .Required({ "total", "page", "pageSize", "returned", "assets" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_AssetsList;
            registry.Register(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_assets_problems";
            tool.Toolset = "assets";
            tool.Title = "List asset problems";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List assets that failed to load or are missing/invalid (handle, type, path, status). The "
                "first thing to check when something references an asset that isn't showing up.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0).Desc("Number of problem assets; 0 with an empty list means all assets are healthy."))
                                    .Prop("problems", Schema::Array(Schema::Object()
                                                                        .Prop("handle", Schema::String().Desc("Asset handle (decimal u64)."))
                                                                        .Prop("type", Schema::String())
                                                                        .Prop("path", Schema::String())
                                                                        .Prop("status", Schema::String().Desc("Error status name from the asset registry."))))
                                    .Required({ "count", "problems" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_AssetsProblems;
            registry.Register(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
