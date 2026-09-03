#include "OloEnginePCH.h"
#include "MCP/McpAccessibility.h"
#include "MCP/McpEditorDebugDraw.h"
#include "MCP/McpEditorPanels.h"
#include "MCP/McpLightmapBake.h"
#include "MCP/McpTerrainPick.h"
#include "MCP/McpToolsCommon.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Editor-session MCP tools. These use explicit EditorMcpContext callbacks for
// EditorLayer-owned state; process-global accessibility settings use their
// engine accessor directly. Every operation is marshaled to the main thread.

namespace OloEngine::MCP
{
    namespace
    {
        Schema::Node PanelSchema()
        {
            return Schema::Object()
                .Prop("name", Schema::String())
                .Prop("title", Schema::String())
                .Prop("open", Schema::Bool())
                .Required({ "name", "title", "open" });
        }

        Schema::Node DebugStateSchema()
        {
            auto schema = Schema::Object();
            for (const auto category : EditorDebugDraw::kCategories)
                schema.Prop(std::string(category), Schema::Bool());
            return schema.Required({ "all", "grid", "physics_colliders", "light_gizmos", "world_axis",
                                     "camera_frustums", "bounding_boxes", "selection_outline", "component_gizmos" });
        }

        Schema::Node AccessibilityFieldSchema()
        {
            return Schema::Object()
                .Prop("setting", Schema::String())
                .Prop("group", Schema::String())
                .Prop("description", Schema::String())
                .Prop("type", Schema::String().Enum({ "boolean", "number", "enum" }))
                .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string" }) } }))
                .Prop("min", Schema::Number())
                .Prop("max", Schema::Number())
                .Prop("values", Schema::Array(Schema::Object()
                                                  .Prop("token", Schema::String())
                                                  .Prop("description", Schema::String())
                                                  .Required({ "token", "description" })))
                .Prop("rebuildsRenderGraph", Schema::Bool())
                .Required({ "setting", "group", "description", "type", "value" });
        }

        ToolResult Handle_EditorPanelList(McpServer& server, const Json&)
        {
            if (!server.Context().GetEditorPanels)
                return ToolResult::Error("Editor panel control is not available in this host.");
            const Json result = server.MarshalRead([&server]() -> Json
                                                   {
                Json panels = Json::array();
                for (const auto& state : server.Context().GetEditorPanels())
                    panels.push_back(EditorPanels::ToJson(state));
                return Json{ { "count", panels.size() }, { "panels", std::move(panels) } }; });
            return ToolResult::Structured(result);
        }

        ToolResult Handle_EditorPanelSet(McpServer& server, const Json& args)
        {
            if (!server.Context().SetEditorPanel)
                return ToolResult::Error("Editor panel control is not available in this host.");
            if (!args.contains("panel") || !args["panel"].is_string() ||
                !args.contains("open") || !args["open"].is_boolean())
                return ToolResult::Error("Expected 'panel' (string) and 'open' (boolean).");
            const std::string panel = args["panel"].get<std::string>();
            const bool open = args["open"].get<bool>();
            const Json result = server.MarshalRead([&server, panel, open]() -> Json
                                                   { return EditorPanels::ToJson(server.Context().SetEditorPanel(panel, open)); });
            if (!result.value("ok", false))
                return ToolResult::Error(result.value("message", "Could not change editor panel state."));
            return ToolResult::Structured(result);
        }

        ToolResult Handle_EditorDebugDrawSet(McpServer& server, const Json& args)
        {
            if (!server.Context().SetEditorDebugDraw)
                return ToolResult::Error("Editor debug-draw control is not available in this host.");
            if (!args.contains("category") || !args["category"].is_string() ||
                !args.contains("enabled") || !args["enabled"].is_boolean())
                return ToolResult::Error("Expected 'category' (string) and 'enabled' (boolean).");
            const std::string category = args["category"].get<std::string>();
            const bool enabled = args["enabled"].get<bool>();
            const Json result = server.MarshalRead([&server, category, enabled]() -> Json
                                                   { return EditorDebugDraw::ToJson(server.Context().SetEditorDebugDraw(category, enabled)); });
            if (!result.value("ok", false))
                return ToolResult::Error(result.value("message", "Could not change editor debug-draw state."));
            return ToolResult::Structured(result);
        }

        ToolResult Handle_AccessibilityGet(McpServer& server, const Json& args)
        {
            const AccessibilitySettingsTool::FieldInfo* field = nullptr;
            if (const auto error = AccessibilitySettingsTool::ParseGetArgs(args, field))
                return ToolResult::Error(*error);
            const Json result = server.MarshalRead([field]() -> Json
                                                   {
                if (field != nullptr)
                    return Json{ { "scope", "process" },
                                 { "settings", Json::array({ AccessibilitySettingsTool::DescribeField(
                                                       *field, ::OloEngine::Accessibility::Get()) }) } };
                return AccessibilitySettingsTool::DescribeGlobal(); });
            return ToolResult::Structured(result);
        }

        ToolResult Handle_AccessibilitySet(McpServer& server, const Json& args)
        {
            const AccessibilitySettingsTool::FieldInfo* field = nullptr;
            Json value;
            if (const auto error = AccessibilitySettingsTool::ParseSetArgs(args, field, value))
                return ToolResult::Error(*error);
            const Json result = server.MarshalRead([field, value]() -> Json
                                                   {
                auto applied = AccessibilitySettingsTool::ApplyGlobal(*field, value);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                if (applied.RequiresRenderGraphRebuild)
                    Renderer3D::RequestRenderGraphRebuild();
                return std::move(applied.Data); });
            if (result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        ToolResult Handle_TerrainPick(McpServer& server, const Json& args)
        {
            if (!server.Context().TerrainPick)
                return ToolResult::Error("Terrain GPU picking is not available in this host.");
            TerrainPick::Request request;
            if (const auto error = TerrainPick::ParseRequest(args, request))
                return ToolResult::Error(*error);

            Json result = server.MarshalRead([&server, request]() -> Json
                                             { return TerrainPick::BuildResult(server.Context().TerrainPick(request, true)); });
            if (result.value("status", "") == "pending" && server.Context().GetFrameIndex)
            {
                const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                         { return Json(server.Context().GetFrameIndex()); })
                                          .get<u64>();
                (void)AwaitRenderedFrames(server, baseFrame, 5, std::chrono::seconds(8));
                result = server.MarshalRead([&server, request]() -> Json
                                            { return TerrainPick::BuildResult(server.Context().TerrainPick(request, false)); });
            }
            return ToolResult::Structured(result);
        }

        ToolResult Handle_LightmapBake(McpServer& server, const Json& args)
        {
            if (!server.Context().LightmapBake)
                return ToolResult::Error("Lightmap baking is not available in this host.");
            LightmapBake::Request request;
            if (const auto error = LightmapBake::ParseRequest(args, request))
                return ToolResult::Error(*error);

            const bool starts = request.RequestMode != LightmapBake::Mode::Poll;
            Json response = server.MarshalRead([&server, request, starts]() -> Json
                                               { return LightmapBake::BuildResponse(server.Context().LightmapBake(request, starts)); });
            if (request.RequestMode != LightmapBake::Mode::Blocking ||
                response.value("status", "failed") != "running")
                return ToolResult::Structured(response);

            LightmapBake::Request poll;
            poll.RequestMode = LightmapBake::Mode::Poll;
            poll.OperationId = response.value("operationId", "");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (server.IsCurrentCallCancelled())
                    return ToolResult::Error("Lightmap bake wait was cancelled; the bake continues and can be polled with operationId '" + poll.OperationId + "'.");
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                response = server.MarshalRead([&server, poll]() -> Json
                                              { return LightmapBake::BuildResponse(server.Context().LightmapBake(poll, false)); });
                const std::string status = response.value("status", "failed");
                server.EmitProgress(response.value("progress", 0.0), 1.0, "baking lightmaps");
                if (status == "succeeded" || status == "failed")
                    return ToolResult::Structured(response);
            }
            return ToolResult::Error("Timed out waiting for the lightmap bake; it continues and can be polled with operationId '" + poll.OperationId + "'.");
        }
    } // namespace

    void RegisterEditorTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_editor_panel_list";
            tool.Toolset = "editor";
            tool.Title = "List editor panels";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description = "List every controllable editor panel by stable name and display title, including whether it is open.";
            tool.InputSchema = EditorPanels::ListInputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0))
                                    .Prop("panels", Schema::Array(PanelSchema()))
                                    .Required({ "count", "panels" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_EditorPanelList;
            server.RegisterTool(std::move(tool));
        }
        {
            ToolDef tool;
            tool.Name = "olo_editor_panel_set";
            tool.Toolset = "editor";
            tool.Title = "Open or close editor panel";
            tool.Annotations = MutatingAnnotations(true);
            tool.ProjectWrite = true;
            tool.Description = "Open or close one editor panel by the stable name returned by olo_editor_panel_list. Session UI state only.";
            tool.InputSchema = EditorPanels::SetInputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("available", Schema::Bool())
                                    .Prop("ok", Schema::Bool())
                                    .Prop("changed", Schema::Bool())
                                    .Prop("panel", PanelSchema())
                                    .Prop("message", Schema::String())
                                    .Required({ "available", "ok", "changed", "panel", "message" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_EditorPanelSet;
            server.RegisterTool(std::move(tool));
        }
        {
            ToolDef tool;
            tool.Name = "olo_accessibility_get";
            tool.Toolset = "editor";
            tool.Title = "Get accessibility settings";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description = "Read all nine process-global accessibility settings, or one named setting, with type/range metadata.";
            tool.InputSchema = AccessibilitySettingsTool::GetInputSchema();
            tool.OutputSchema = Schema::Object().Prop("scope", Schema::String()).Prop("settings", Schema::Array(AccessibilityFieldSchema())).Required({ "scope", "settings" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_AccessibilityGet;
            server.RegisterTool(std::move(tool));
        }
        {
            ToolDef tool;
            tool.Name = "olo_accessibility_set";
            tool.Toolset = "editor";
            tool.Title = "Set accessibility setting";
            tool.Annotations = MutatingAnnotations(true);
            tool.ProjectWrite = true;
            tool.Description = "Set one process-global accessibility preference. Returns the prior value as restoreWith; color-blind mode changes rebuild the render graph.";
            tool.InputSchema = AccessibilitySettingsTool::SetInputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("scope", Schema::String())
                                    .Prop("setting", Schema::String())
                                    .Prop("previousValue", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string" }) } }))
                                    .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string" }) } }))
                                    .Prop("changed", Schema::Bool())
                                    .Prop("restoreWith", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string" }) } }))
                                    .Prop("clamped", Schema::Bool())
                                    .Prop("range", Schema::Object().Prop("min", Schema::Number()).Prop("max", Schema::Number()).Required({ "min", "max" }))
                                    .Prop("rebuildsRenderGraph", Schema::Bool())
                                    .Required({ "scope", "setting", "previousValue", "value", "changed", "restoreWith" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_AccessibilitySet;
            server.RegisterTool(std::move(tool));
        }
        {
            ToolDef tool;
            tool.Name = "olo_editor_debug_draw_set";
            tool.Toolset = "editor";
            tool.Title = "Set editor debug draws";
            tool.Annotations = MutatingAnnotations(true);
            tool.ProjectWrite = true;
            tool.Description = "Enable or suppress an editor overlay category in Edit, Play, and Simulate. category:'all', enabled:false produces a clean 3D viewport capture without changing individual preferences.";
            tool.InputSchema = EditorDebugDraw::SetInputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("available", Schema::Bool())
                                    .Prop("ok", Schema::Bool())
                                    .Prop("changed", Schema::Bool())
                                    .Prop("category", Schema::String())
                                    .Prop("enabled", Schema::Bool())
                                    .Prop("state", DebugStateSchema())
                                    .Prop("message", Schema::String())
                                    .Required({ "available", "ok", "changed", "category", "enabled", "state", "message" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_EditorDebugDrawSet;
            server.RegisterTool(std::move(tool));
        }
        {
            ToolDef tool;
            tool.Name = "olo_lightmap_bake";
            tool.Toolset = "editor";
            tool.Title = "Bake scene lightmaps";
            tool.Annotations = DestructiveMutatingAnnotations();
            tool.ProjectWrite = true;
            tool.Description = "Start or poll the editor's real baked-GI lightmap operation, or block until it completes. The optional save flag saves the scene after the generated .olmap asset is attached. Long-running and non-undoable.";
            tool.InputSchema = LightmapBake::InputSchema();
            tool.OutputSchema = LightmapBake::OutputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_LightmapBake;
            server.RegisterTool(std::move(tool));
        }
        {
            ToolDef tool;
            tool.Name = "olo_terrain_pick";
            tool.Toolset = "editor";
            tool.Title = "Pick terrain";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description = "Ray-cast the active terrain through TerrainGPUPicker using a viewport pixel, normalized viewport coordinate, or explicit world ray. Waits for the asynchronous GPU answer and reports pending distinctly from a miss, with ray id, latency, local/world hit, and overflow flags.";
            tool.InputSchema = TerrainPick::InputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("status", Schema::String().Enum({ "unavailable", "pending", "answered" }))
                                    .Prop("rayId", Schema::Int().Min(0))
                                    .Prop("input", Schema::Object().Prop("source", Schema::String()).Prop("coordinate", Schema::Array(Schema::Number())).Prop("viewport", Schema::Object()).Prop("ray", Schema::Object()).Required({ "source" }))
                                    .Prop("worldRay", Schema::Object().Prop("origin", Schema::Array(Schema::Number())).Prop("direction", Schema::Array(Schema::Number())).Prop("maxDistance", Schema::Number()).Required({ "origin", "direction", "maxDistance" }))
                                    .Prop("overflow", Schema::Object().Prop("any", Schema::Bool()).Prop("rawFlags", Schema::Int().Min(0)).Prop("nodes", Schema::Bool()).Prop("candidates", Schema::Bool()).Prop("march", Schema::Bool()).Required({ "any", "rawFlags", "nodes", "candidates", "march" }))
                                    .Prop("reason", Schema::String())
                                    .Prop("hit", Schema::Bool())
                                    .Prop("latencyFrames", Schema::Int().Min(0))
                                    .Prop("worldHit", Schema::Array(Schema::Number()))
                                    .Prop("localHit", Schema::Array(Schema::Number()))
                                    .Required({ "status", "rayId", "input", "overflow" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_TerrainPick;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
