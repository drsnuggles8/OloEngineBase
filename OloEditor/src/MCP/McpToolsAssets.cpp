#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetMetadata.h"
#include "OloEngine/Asset/AssetTypes.h"
#include "OloEngine/Project/Project.h"

#include <algorithm>
#include <string>
#include <vector>

// Asset MCP tools: olo_assets_list and olo_assets_problems, reading the
// project's asset registry. Split out of the McpTools.cpp monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
        // ---- olo_assets_list (main-marshaled; reads the project asset registry) -
        ToolResult Handle_AssetsList(McpServer& server, const Json& args)
        {
            std::string typeFilter;
            if (args.contains("typeFilter") && args["typeFilter"].is_string())
                typeFilter = args["typeFilter"].get<std::string>();
            int page = 0;
            int pageSize = 50;
            if (args.contains("page") && args["page"].is_number_integer())
                page = static_cast<int>(std::max<long long>(0, args["page"].get<long long>()));
            if (args.contains("pageSize") && args["pageSize"].is_number_integer())
                pageSize = static_cast<int>(std::clamp<long long>(args["pageSize"].get<long long>(), 1, 200));

            const Json result = server.MarshalRead([typeFilter, page, pageSize]() -> Json
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
        ToolResult Handle_AssetsProblems(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([]() -> Json
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

    void RegisterAssetTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_assets_list";
            tool.Toolset = "assets";
            tool.Title = "List assets";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the project's registered assets (paginated): handle, type, project-relative path, and "
                "filename. Optionally filter by asset type (e.g. Texture2D, Mesh, Material, Scene, Script).";
            tool.InputSchema = Schema::Object()
                                   .Prop("typeFilter", Schema::String().Desc("Asset type name to filter by (e.g. 'Texture2D'). Omit for all types."))
                                   .Pagination("Assets per page (default 50, max 200).")
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("total", Schema::Int().Min(0).Desc("Total registered assets after the type filter."))
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
            server.RegisterTool(std::move(tool));
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
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
