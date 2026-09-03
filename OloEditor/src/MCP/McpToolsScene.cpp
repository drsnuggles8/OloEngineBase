#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpGenericFieldWrite.h"
#include "MCP/McpReflectionProbeBake.h"
#include "MCP/McpSceneControl.h"
#include "MCP/McpSchedulerGraph.h"
#include "MCP/McpSelectEntity.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scene/SystemScheduler.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

// Scene / ECS MCP tools: the scene readers (summary, entity list/get), the
// generic consented field write (olo_entity_list_fields / olo_entity_set_field),
// and the scene-control writes (olo_scene_open / olo_scene_play / olo_scene_stop).
// Split out of the McpTools.cpp monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
        // ---- olo_scene_summary (main-marshaled) --------------------------------
        // Reads the active Scene + EnTT registry, which are NOT thread-safe, so the
        // read is marshaled onto the game thread and returns a consistent snapshot.
        ToolResult Handle_SceneSummary(McpServer& server, const Json& /*arguments*/)
        {
            Json summary = server.MarshalRead([&server]() -> Json
                                              {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                const bool isPlaying = server.Context().IsPlaying && server.Context().IsPlaying();

                j["hasActiveScene"] = static_cast<bool>(scene);
                j["isPlaying"] = isPlaying;
                if (scene)
                {
                    j["name"] = scene->GetName();
                    j["isPaused"] = scene->IsPaused();
                    // Every entity carries an IDComponent, so this view's size is the
                    // entity count without walking individual archetypes.
                    j["entityCount"] = static_cast<std::uint64_t>(scene->GetAllEntitiesWith<IDComponent>().size());
                }
                return j; });

            return ToolResult::Structured(summary);
        }

        // ---- olo_scene_get_entity (main-marshaled) -----------------------------
        // Reuses SceneSerializer::SerializeEntity to dump every component of one
        // entity. Returns the component data as YAML text (the serializer already
        // exists and is authoritative) plus structured id/name/hierarchy fields.
        ToolResult Handle_SceneGetEntity(McpServer& server, const Json& args)
        {
            if (!args.contains("id"))
                return ToolResult::Error("Missing required argument 'id' (entity UUID).");
            u64 idValue = 0;
            if (!ParseUuid(args["id"], idValue))
                return ToolResult::Error("Invalid 'id': expected a UUID as a string or number.");

            Json result = server.MarshalRead([&server, idValue]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                {
                    j["error"] = "No active scene";
                    return j;
                }
                const auto entityOpt = scene->TryGetEntityWithUUID(UUID(idValue));
                if (!entityOpt)
                {
                    j["found"] = false;
                    return j;
                }
                Entity entity = *entityOpt;
                j["found"] = true;
                j["id"] = UuidToString(entity.GetUUID());
                j["name"] = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string{};
                if (const UUID parent = entity.GetParentUUID(); static_cast<u64>(parent) != 0)
                    j["parent"] = UuidToString(parent);
                Json children = Json::array();
                for (const UUID child : entity.Children())
                    children.push_back(UuidToString(child));
                j["children"] = std::move(children);

                YAML::Emitter out;
                SceneSerializer::SerializeEntity(out, entity);
                j["componentsYaml"] = std::string(out.c_str());
                return j; });

            if (result.contains("error"))
                return ToolResult::Error(result["error"].get<std::string>());
            if (!result.value("found", false))
                return ToolResult::Error("No entity with that UUID in the active scene.");
            return ToolResult::Structured(result);
        }

        // ---- olo_scene_list_entities (main-marshaled) --------------------------
        // Paginated registry walk (every entity has an IDComponent). Optional
        // substring name filter. Lean entries; drill into olo_scene_get_entity for
        // full component data.
        ToolResult Handle_SceneListEntities(McpServer& server, const Json& args)
        {
            std::string namePattern;
            if (args.contains("namePattern") && args["namePattern"].is_string())
                namePattern = args["namePattern"].get<std::string>();
            int page = 0;
            int pageSize = 50;
            if (args.contains("page") && args["page"].is_number_integer())
                page = static_cast<int>(std::max<long long>(0, args["page"].get<long long>()));
            if (args.contains("pageSize") && args["pageSize"].is_number_integer())
                pageSize = static_cast<int>(std::clamp<long long>(args["pageSize"].get<long long>(), 1, 200));

            Json result = server.MarshalRead([&server, namePattern, page, pageSize]() -> Json
                                             {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                {
                    j["error"] = "No active scene";
                    return j;
                }

                std::vector<Entity> matches;
                for (const auto handle : scene->GetAllEntitiesWith<IDComponent>())
                {
                    Entity entity{ handle, scene.get() };
                    if (!namePattern.empty())
                    {
                        const std::string name = entity.HasComponent<TagComponent>()
                                                     ? entity.GetComponent<TagComponent>().Tag
                                                     : std::string{};
                        if (name.find(namePattern) == std::string::npos)
                            continue;
                    }
                    matches.push_back(entity);
                }

                const auto total = static_cast<int>(matches.size());
                // 64-bit to avoid int overflow when a large page is requested.
                const long long start = static_cast<long long>(page) * pageSize;
                Json entities = Json::array();
                for (long long i = start; i < total && i < start + pageSize; ++i)
                {
                    Entity entity = matches[static_cast<sizet>(i)];
                    Json e;
                    e["id"] = UuidToString(entity.GetUUID());
                    e["name"] = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string{};
                    if (const UUID parent = entity.GetParentUUID(); static_cast<u64>(parent) != 0)
                        e["parent"] = UuidToString(parent);
                    e["childCount"] = static_cast<int>(entity.Children().size());
                    entities.push_back(std::move(e));
                }

                j["total"] = total;
                j["page"] = page;
                j["pageSize"] = pageSize;
                j["returned"] = static_cast<int>(entities.size());
                if (start + pageSize < total)
                    j["nextPage"] = page + 1;
                j["entities"] = std::move(entities);
                return j; });

            if (result.contains("error"))
                return ToolResult::Error(result["error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_entity_set_field (main-marshaled; PROJECT WRITE) --------------
        // The GENERIC consented, undoable write tool (#306):
        // set ANY registered component field by (component, field, value) through the
        // editor's undo stack — the catch-all successor to olo_set_collision_layer's
        // one-tool-per-field shape. Gated at dispatch by the "Allow writes" session
        // toggle (ToolDef::ProjectWrite); the shared reflect+coerce+apply core lives
        // in McpGenericFieldWrite.h so it is unit-tested at the dispatch seam without
        // this TU. The command is built + executed inside the MarshalRead job, i.e.
        // on the main thread, since it touches the EnTT registry and command stack.
        //
        // Play-mode branch (issue #607's runtime field-write slice): CommandHistory
        // is deliberately nulled while the scene is playing, so a Play-mode call used
        // to hard-fail with "No editor command history available." — blocking an
        // agent from driving/verifying live gameplay state without stopping Play,
        // editing, and restarting. When EditorMcpContext.IsPlaying() reports the
        // scene is running, skip the history requirement entirely and write straight
        // into the live m_ActiveScene component via GenericFieldWrite::ApplyDirect —
        // a real mutation with no undo and no persistence back to the edit-mode
        // scene (mirrors the non-undoable framing of SelectEntityInEditor /
        // olo_renderer_settings_set). The result's `undoable: false` tells the caller
        // this write won't survive Stop and can't be Ctrl-Z'd.
        ToolResult Handle_EntitySetField(McpServer& server, const Json& args)
        {
            if (!server.Context().GetActiveScene || !server.Context().GetCommandHistory)
                return ToolResult::Error("Project writes are not available in this editor build.");

            u64 entityUuid = 0;
            std::string component;
            std::string field;
            Json value;
            if (const auto error = GenericFieldWrite::ParseArgs(args, entityUuid, component, field, value))
                return ToolResult::Error(*error);

            const Json result = server.MarshalRead([&server, entityUuid, component, field, value]() -> Json
                                                   {
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };

                const bool isPlaying = server.Context().IsPlaying && server.Context().IsPlaying();
                if (isPlaying)
                {
                    const GenericFieldWrite::ApplyResult applied =
                        GenericFieldWrite::ApplyDirect(scene, entityUuid, component, field, value);
                    if (!applied.Ok)
                        return Json{ { "__error", applied.Error } };
                    return applied.Data;
                }

                CommandHistory* history = server.Context().GetCommandHistory
                                              ? server.Context().GetCommandHistory()
                                              : nullptr;
                if (!history)
                    return Json{ { "__error", "No editor command history available." } };

                const GenericFieldWrite::ApplyResult applied =
                    GenericFieldWrite::Apply(scene, *history, entityUuid, component, field, value);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                return applied.Data; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_entity_list_fields (main-marshaled; read-only) ----------------
        // The discovery half of olo_entity_set_field: for one entity, list the
        // writable component fields (those the entity actually has) with their type
        // and current value, so an agent learns the exact (component, field) names +
        // value shapes before issuing a write. Read-only (not ProjectWrite).
        ToolResult Handle_EntityListFields(McpServer& server, const Json& args)
        {
            if (!server.Context().GetActiveScene)
                return ToolResult::Error("Scene reads are not available in this editor build.");

            if (!args.contains("entity"))
                return ToolResult::Error("Missing required argument 'entity' (entity UUID).");
            u64 entityUuid = 0;
            if (!ParseUuid(args["entity"], entityUuid))
                return ToolResult::Error("Invalid 'entity': expected a UUID as a string or number.");
            const std::string componentFilter =
                (args.contains("component") && args["component"].is_string()) ? args["component"].get<std::string>() : std::string();

            const Json result = server.MarshalRead([&server, entityUuid, componentFilter]() -> Json
                                                   {
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;
                bool entityFound = false;
                return GenericFieldWrite::ListFields(scene, entityUuid, componentFilter, entityFound); });

            return ToolResult::Structured(result);
        }

        // MarshalRead's default 5s watchdog is sized for the typical read-only tool
        // (a snapshot, a query) — nowhere near enough for a full scene load/copy at
        // stress-test entity counts (a 50k-entity YAML deserialize measured ~55-60s
        // wall time; a 100k-entity transform-only scene ~24s: see
        // docs/analysis/perf-stress-findings-2026-07.md). Below that ceiling
        // MarshalRead's CALLER gives up and returns an error while the job it
        // enqueued keeps running on the game thread regardless (nothing dequeues
        // it) — a driver that reacts to that "timeout" by immediately queuing the
        // NEXT scene's open compounds a single-threaded backlog that never
        // recovers (found running scripts/perf/run-perf-battery.ps1 against a
        // long-lived editor instance, #316 follow-up). olo_scene_open /
        // olo_scene_play / olo_scene_stop are the only tools that legitimately need
        // more room; every other MarshalRead call site is a fast query and keeps
        // the 5s default deliberately, so slow tools genuinely hang rather than
        // silently blocking behind the queue for minutes.
        constexpr std::chrono::milliseconds kSceneControlTimeout{ 120000 };

        // ---- olo_scene_open (main-marshaled; PROJECT WRITE) --------------------
        // Open / switch the active scene over MCP — the consented-write scene switch
        // (issue #316). Loads the requested scene file directly through the
        // editor's OpenSceneFromMcp hook, which installs it the same way the editor's
        // Open Scene menu does but WITHOUT the auto-save recovery modal (a remote
        // agent can't click it) and without the file dialog. Gated at dispatch by the
        // "Allow writes" session toggle (ToolDef::ProjectWrite): switching scenes
        // discards the current in-memory scene state, so it crosses the read-only line
        // by design. The load runs inside the MarshalRead job, i.e. on the main
        // thread, since it touches the EnTT registry / renderer settings. The shared
        // schema + path validation + result shaping live in McpSceneControl.h so they
        // are unit-tested at the dispatch seam without this TU.
        ToolResult Handle_SceneOpen(McpServer& server, const Json& args)
        {
            using namespace SceneControl;

            if (!args.contains("path") || !args["path"].is_string())
                return ToolResult::Error("Missing required argument 'path' (a .olo or .scene scene file).");
            const std::string path = args["path"].get<std::string>();
            if (const auto error = ValidateScenePath(path))
                return ToolResult::Error(*error);

            if (!server.Context().OpenSceneFromMcp)
                return ToolResult::Error("Scene open is not available in this editor build.");

            // path is captured BY VALUE: MarshalRead's caller-side wait can time out
            // (kSceneControlTimeout) while the job it enqueued is still running on
            // the game thread — nothing dequeues an abandoned job. A by-reference
            // capture of this function-local string would dangle once
            // Handle_SceneOpen's stack frame unwinds on that timeout; server is a
            // long-lived object (owned by EditorLayer for the whole session) so a
            // reference capture there is safe.
            const Json result = server.MarshalRead([&server, path]() -> Json
                                                   {
                if (!server.Context().OpenSceneFromMcp)
                    return Json{ { "__error", "Scene open is not available in this editor build." } };
                const McpSceneOpenResult opened = server.Context().OpenSceneFromMcp(path);
                return ToJson(opened); },
                                                   kSceneControlTimeout);

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());

            // Settle before returning (#519 "first perf-lever write right after
            // scene load doesn't take effect on the GPU"). The load above ran
            // synchronously inside the MarshalRead job, blocking the main-thread
            // frame pump for however long it took; the very next OnUpdate sees
            // that stall as an inflated timestep and trips the render-budget
            // throttle for a beat, during which Renderer3D::BeginScene (and so
            // RendererProfiler::BeginFrame/EndFrame) never runs. A caller that
            // immediately writes a renderer setting and reads back
            // olo_perf_snapshot in that window sees stale pre-load data, not the
            // new state — Handle_RendererSettingsSet settles on its own end too,
            // but waiting here as well means a plain scene-load caller (no
            // follow-up settings write) also gets a scene that has actually
            // rendered before the tool returns.
            constexpr int kPostLoadSettleFrames = 2;
            if (server.Context().GetFrameIndex)
            {
                const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                         { return Json{ { "frame", server.Context().GetFrameIndex() } }; })
                                          .value("frame", static_cast<u64>(0));
                AwaitRenderedFrames(server, baseFrame, kPostLoadSettleFrames);
            }

            return ToolResult::Structured(result);
        }

        // ---- olo_scene_play / simulate / stop (main-marshaled; PROJECT WRITE) --
        // Select scene mode over MCP through the same paths as the editor toolbar.
        // Gated at dispatch by the "Allow writes" session
        // toggle (ToolDef::ProjectWrite): entering Play copies the scene and executes
        // the user's game scripts, so it crosses the read-only line — but it is
        // transient (stopping restores the authored scene, exactly like the editor).
        // olo_scene_summary reports `isPlaying`, so an agent can confirm the
        // transition took. The transition runs inside the MarshalRead job (main
        // thread), since it mutates scene state.
        enum class SceneModeRequest : u8
        {
            Edit,
            Play,
            Simulate
        };

        ToolResult Handle_ScenePlayState(McpServer& server, SceneModeRequest request)
        {
            using namespace SceneControl;

            const bool available = request == SceneModeRequest::Simulate
                                       ? static_cast<bool>(server.Context().SetSceneSimulateState)
                                       : static_cast<bool>(server.Context().SetScenePlayState);
            if (!available)
                return ToolResult::Error("Scene-mode control is not available in this editor build.");

            const Json result = server.MarshalRead([&server, request]() -> Json
                                                   {
                McpScenePlayResult r;
                if (request == SceneModeRequest::Simulate)
                {
                    if (!server.Context().SetSceneSimulateState)
                        return Json{ { "__error", "Scene-mode control is not available in this editor build." } };
                    r = server.Context().SetSceneSimulateState();
                }
                else
                {
                    if (!server.Context().SetScenePlayState)
                        return Json{ { "__error", "Scene-mode control is not available in this editor build." } };
                    r = server.Context().SetScenePlayState(request == SceneModeRequest::Play);
                }
                return ToJson(r); },
                                                   kSceneControlTimeout);

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());

            // Settle rendered frames before returning (issue #607 — the
            // uniform-grey olo_screenshot case). MarshalRead jobs drain BEFORE
            // the frame's OnUpdate, so the state transition above has not been
            // RENDERED yet when this tool returns; an immediate follow-up
            // olo_screenshot captured the last pre-transition frame (an
            // Edit-mode frame — for a 2D-sprite scene in the 3D editor, a
            // uniform-grey clear, since sprites only draw through the Play
            // overlay callback). Waiting until the new state has produced
            // frames makes "olo_scene_play then olo_screenshot" show what is
            // actually playing.
            // Best-effort: the transition above already SUCCEEDED, and these
            // settle marshals run on the 5s default timeout right after a
            // transition that legitimately gets 120s — a heavy scene's first
            // post-transition frame can outlive 5s, and that must degrade to
            // "returned before settling", never to a reported tool failure.
            if (result.value("changed", false) && server.Context().GetFrameIndex)
            {
                try
                {
                    constexpr int kPostTransitionSettleFrames = 2;
                    const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                             { return Json{ { "frame", server.Context().GetFrameIndex() } }; })
                                              .value("frame", static_cast<u64>(0));
                    AwaitRenderedFrames(server, baseFrame, kPostTransitionSettleFrames);
                }
                catch (...)
                {
                    // Settle timed out — the transition result stands.
                }
            }
            return ToolResult::Structured(result);
        }

        ToolResult Handle_ScenePlay(McpServer& server, const Json&)
        {
            return Handle_ScenePlayState(server, SceneModeRequest::Play);
        }

        ToolResult Handle_SceneSimulate(McpServer& server, const Json&)
        {
            return Handle_ScenePlayState(server, SceneModeRequest::Simulate);
        }

        ToolResult Handle_SceneStop(McpServer& server, const Json&)
        {
            return Handle_ScenePlayState(server, SceneModeRequest::Edit);
        }

        Json BakeNamedReflectionProbe(McpServer& server, const std::string& entityName)
        {
            Ref<Scene> scene = server.Context().GetActiveScene
                                   ? server.Context().GetActiveScene()
                                   : nullptr;
            if (!scene)
                return Json{ { "__error", "No active scene." } };

            Entity probeEntity;
            u32 matchCount = 0;
            auto view = scene->GetAllEntitiesWith<TagComponent>();
            for (const auto handle : view)
            {
                if (view.get<TagComponent>(handle).Tag != entityName)
                    continue;
                ++matchCount;
                probeEntity = Entity{ handle, scene.get() };
            }

            if (matchCount == 0)
                return Json{ { "__error", "No entity named '" + entityName + "' exists in the active scene." } };
            if (matchCount > 1)
            {
                return Json{ { "__error", "Entity name '" + entityName + "' is ambiguous (" +
                                              std::to_string(matchCount) +
                                              " exact matches); reflection-probe entity names must be unique." } };
            }
            if (!probeEntity.HasComponent<ReflectionProbeComponent>())
                return Json{ { "__error", "Entity '" + entityName + "' has no ReflectionProbeComponent." } };
            if (!probeEntity.HasComponent<TransformComponent>())
                return Json{ { "__error", "Entity '" + entityName + "' has no TransformComponent." } };

            auto& probe = probeEntity.GetComponent<ReflectionProbeComponent>();
            const glm::vec3 position = probeEntity.GetComponent<TransformComponent>().Translation;
            const bool baked = ReflectionProbeBaker::BakeProbe(scene, position, probe);
            return ReflectionProbeBake::Result(
                entityName, baked,
                baked ? "Baked reflection probe '" + entityName + "'."
                      : "Reflection probe bake failed for '" + entityName + "' (see the engine log).");
        }

        // ---- olo_reflection_probe_bake (main-marshaled; PROJECT WRITE) --------
        // The bake renders the live scene six times and replaces the component's
        // in-memory EnvironmentMap, so entity resolution and the complete bake run
        // on the game thread. Names are exact and must be unique: silently choosing
        // the first duplicate would bake the wrong probe while reporting success.
        ToolResult Handle_ReflectionProbeBake(McpServer& server, const Json& args)
        {
            std::string entityName;
            if (const auto error = ReflectionProbeBake::ParseEntityName(args, entityName))
                return ToolResult::Error(*error);

            const Json result = server.MarshalRead(
                [&server, entityName]() -> Json
                { return BakeNamedReflectionProbe(server, entityName); },
                kSceneControlTimeout);

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_editor_select_entity (main-marshaled; PROJECT WRITE) ----------
        // Select / clear the Scene Hierarchy panel's selection over MCP (issue
        // #607) — the write that makes the Properties inspector draw a given
        // entity's components, unblocking screenshot verification of the whole
        // DrawComponent<T> surface. olo_input_inject can't reliably land a
        // panel-space click for this (the OS cursor reasserts over the synthetic
        // position between injected frames), so this is a direct write instead.
        // Gated at dispatch by the "Allow writes" session toggle
        // (ToolDef::ProjectWrite): it mutates editor UI state on the agent's
        // behalf, mirroring the other scene-control writes, even though it never
        // touches project/scene DATA (not routed through CommandHistory/undo —
        // selection isn't undoable). The shared schema + arg parsing + result
        // shaping live in MCP/McpSelectEntity.h so they are unit-tested at the
        // dispatch seam without this TU.
        ToolResult Handle_SelectEntity(McpServer& server, const Json& args)
        {
            using namespace SelectEntity;

            Request request;
            if (const auto error = ParseArgs(args, request))
                return ToolResult::Error(*error);

            if (!server.Context().SelectEntityInEditor)
                return ToolResult::Error("Entity selection is not available in this editor build.");

            const Json result = server.MarshalRead([&server, request]() -> Json
                                                   {
                if (!server.Context().SelectEntityInEditor)
                    return Json{ { "__error", "Entity selection is not available in this editor build." } };
                return ToJson(server.Context().SelectEntityInEditor(request.EntityUuid, request.Clear)); });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_scheduler_graph (main-marshaled) ------------------------------
        // Scene::GetGameplayScheduler() is a process-global function-local static
        // shared by every Scene, and ExportGraph() calls Build() (which mutates the
        // cached derivation) — so this is marshaled onto the game thread rather than
        // read from the HTTP worker, exactly like the scene readers above. The tool
        // needs no active scene: the schedule is authored once at build time.
        ToolResult Handle_SchedulerGraph(McpServer& server, const Json& args)
        {
            const std::string format = args.value("format", std::string{ "json" });

            Json snapshotJson;
            try
            {
                // `format` is captured BY VALUE: MarshalRead can throw on a timeout
                // while its enqueued job still runs later on the game thread, and a
                // reference to this frame's local would be dangling by then.
                snapshotJson = server.MarshalRead(
                    [format]() -> Json
                    {
                        const SystemScheduler::GraphSnapshot graph = Scene::GetGameplayScheduler().ExportGraph();

                        SchedulerGraph::Snapshot snap;
                        snap.ParallelExecutionEnabled = graph.ParallelExecutionEnabled;
                        snap.Nodes.reserve(graph.Nodes.size());
                        for (const auto& node : graph.Nodes)
                        {
                            snap.Nodes.push_back(SchedulerGraph::NodeInfo{ node.Name, node.Reads, node.Writes,
                                                                           node.After, node.Before, node.Parallel,
                                                                           node.OrderIndex });
                        }
                        snap.Edges.reserve(graph.Edges.size());
                        for (const auto& edge : graph.Edges)
                            snap.Edges.push_back(SchedulerGraph::EdgeInfo{ edge.From, edge.To });

                        // Shape it here, inside the job: the Snapshot borrows nothing
                        // from the scheduler, but keeping the whole conversion in one
                        // marshaled step means the graph cannot be re-derived under us
                        // between the copy and the render. Only the REQUESTED format is
                        // built — this runs on the game thread, so the two unused
                        // renderings would be pure per-call frame-time cost.
                        if (format == "mermaid")
                            return Json{ { "text", SchedulerGraph::BuildMermaid(snap) } };
                        if (format == "dot")
                            return Json{ { "text", SchedulerGraph::BuildDot(snap) } };
                        return Json{ { "json", SchedulerGraph::BuildJson(snap) } };
                    });
            }
            catch (const SystemSchedulerError& error)
            {
                // A duplicate name / dangling reference / cycle. Build() throws in
                // every config by design, and a schedule that cannot be derived is
                // exactly the case an agent reaches for this tool to understand, so
                // report the scheduler's own message rather than a generic failure.
                return ToolResult::Error(std::string("The gameplay system schedule is invalid: ") + error.what());
            }

            if (snapshotJson.contains("text"))
                return ToolResult::Text(snapshotJson.value("text", std::string{}));
            return ToolResult::Structured(snapshotJson.value("json", Json::object()));
        }

    } // namespace

    void RegisterSceneTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_scene_summary";
            tool.Toolset = "scene";
            tool.Title = "Scene summary";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Summarise the active scene currently open in the editor: its name, whether the game "
                "is playing or paused, whether a scene is loaded, and the total entity count. Read "
                "directly from the live ECS on the editor's main thread (a consistent frame snapshot).";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("hasActiveScene", Schema::Bool().Desc("Whether a scene is currently loaded."))
                                    .Prop("isPlaying", Schema::Bool().Desc("Whether the game is in Play mode."))
                                    .Prop("name", Schema::String().Desc("Active scene name (only when a scene is loaded)."))
                                    .Prop("isPaused", Schema::Bool().Desc("Whether the playing scene is paused (only when a scene is loaded)."))
                                    .Prop("entityCount", Schema::Int().Min(0).Desc("Total entity count (only when a scene is loaded)."))
                                    .Required({ "hasActiveScene", "isPlaying" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSummary;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_list_entities";
            tool.Toolset = "scene";
            tool.Title = "List scene entities";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List entities in the active scene (paginated). Each entry has the entity's UUID, name, "
                "parent UUID (if any), and child count. Optionally filter by a name substring. Use this "
                "to find an entity, then call olo_scene_get_entity with its id for full component data.";
            tool.InputSchema = Schema::Object()
                                   .Prop("namePattern", Schema::String().Desc("Case-sensitive substring to match against entity names."))
                                   .Pagination("Entities per page (default 50, max 200).")
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("total", Schema::Int().Min(0).Desc("Total matching entities before pagination."))
                                    .Prop("page", Schema::Int().Min(0))
                                    .Prop("pageSize", Schema::Int().Min(1).Desc("Effective page size after clamping (1..200)."))
                                    .Prop("returned", Schema::Int().Min(0).Desc("Entities in this page."))
                                    .Prop("nextPage", Schema::Int().Min(1).Desc("Next zero-based page; omitted on the last page."))
                                    .Prop("entities", Schema::Array(Schema::Object()
                                                                        .Prop("id", Schema::String().Desc("Entity UUID."))
                                                                        .Prop("name", Schema::String())
                                                                        .Prop("parent", Schema::String().Desc("Parent entity UUID; omitted when the entity has no parent."))
                                                                        .Prop("childCount", Schema::Int().Min(0))))
                                    .Required({ "total", "page", "pageSize", "returned", "entities" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneListEntities;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_get_entity";
            tool.Toolset = "scene";
            tool.Title = "Get entity components";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Get the full component data of one entity by UUID, serialized from the live scene (YAML "
                "in 'componentsYaml', plus structured id/name/parent/children). Pair with "
                "olo_scene_list_entities or olo_scene_summary to obtain the UUID.";
            tool.InputSchema = Schema::Object()
                                   .Prop("id", Schema::EntityId())
                                   .Required({ "id" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("found", Schema::Bool().Desc("True when the entity exists (a miss is returned as isError instead)."))
                                    .Prop("id", Schema::String().Desc("Entity UUID."))
                                    .Prop("name", Schema::String().Desc("Entity tag/name (empty when it has no TagComponent)."))
                                    .Prop("parent", Schema::String().Desc("Parent entity UUID; omitted when the entity has no parent."))
                                    .Prop("children", Schema::Array(Schema::String()).Desc("Child entity UUIDs."))
                                    .Prop("componentsYaml", Schema::String().Desc("All components serialized as scene YAML."))
                                    .Required({ "found", "id", "name", "children", "componentsYaml" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneGetEntity;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_entity_list_fields";
            tool.Toolset = "scene";
            tool.Title = "List entity writable fields";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the writable component fields of one entity, with each field's type, current value and "
                "(where the field has a serializer-enforced range) its 'min'/'max' — the read-only discovery "
                "half of olo_entity_set_field. Only components the entity actually has (and that expose writable "
                "fields) are returned, so the result is exactly what you can write right now. Pass an optional "
                "'component' to restrict the listing. Field names match the keys shown in olo_scene_get_entity's YAML.";
            tool.InputSchema = GenericFieldWrite::ListInputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("entity", Schema::String().Desc("Requested entity UUID echoed back."))
                                    .Prop("found", Schema::Bool().Desc("Whether the UUID resolved in the active scene. A miss is a SUCCESS result with found:false and empty components, not isError."))
                                    .Prop("components", Schema::Array(Schema::Object()
                                                                          .Prop("component", Schema::String())
                                                                          .Prop("fields", Schema::Array(Schema::Object()
                                                                                                            .Prop("field", Schema::String().Desc("Field name; a map-typed field expands to one dotted entry per current key, e.g. 'Weights.Smile'."))
                                                                                                            .Prop("type", Schema::String())
                                                                                                            .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } })
                                                                                                                               .Desc("Current value, typed to match the field."))
                                                                                                            .Prop("min", Schema::Number().Desc("Serializer-enforced lower bound; omitted when the field has none."))
                                                                                                            .Prop("max", Schema::Number().Desc("Serializer-enforced upper bound; omitted when the field has none."))))))
                                    .Required({ "entity", "found", "components" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_EntityListFields;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_entity_set_field";
            tool.Toolset = "scene";
            tool.Title = "Set component field (undoable)";
            // The generic project-WRITE tool (#306): gated behind
            // the session "Allow writes" toggle and routed through the editor undo
            // stack. readOnlyHint:false (not idempotent — each call snapshots the prior
            // value into a distinct undo command; not destructive — fully reversible
            // via Ctrl-Z / undo).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Set a single component field on an entity by (component, field, value) — the generic, "
                "undoable successor to olo_set_collision_layer. The registry is GENERATED from every component "
                "definition (issue #607), so it covers the whole ECS surface: transforms, meshes/models/materials, "
                "VirtualMesh (Nanite), lights, fog/probes/sky, physics bodies + colliders, text, UI, nav, water, "
                "terrain, ... — everything except per-tick runtime state. The value type must match the field: a "
                "boolean, a number, a string (also for an AssetHandle: a decimal-digit u64 string), or an array of "
                "numbers for a vector (e.g. [r,g,b] for a vec3 color). A field with a serializer-enforced range is "
                "CLAMPED into it and the result says clamped:true with the original requestedValue. The result also "
                "echoes 'value' READ BACK from the component after the write plus changed:true/false — verify those "
                "rather than assuming a returned call applied. Applied through the editor's undo stack, so it is a "
                "single Ctrl-Z. This is a WRITE tool: refused unless 'Allow writes' is enabled in the editor's MCP "
                "Server panel (off by default). A map-typed field (e.g. MorphTargetComponent's per-target 'Weights') "
                "is addressed one entry at a time with a dotted key, e.g. field 'Weights.Smile' — its current keys "
                "are only discoverable per-entity, not ahead of time. Discover the exact writable (component, field) "
                "names, value shapes and ranges for an entity with olo_entity_list_fields.";
            tool.InputSchema = GenericFieldWrite::InputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("entity", Schema::String().Desc("Target entity UUID."))
                                    .Prop("component", Schema::String())
                                    .Prop("field", Schema::String().Desc("Field written; the dotted 'Weights.<key>' form for a map-keyed write."))
                                    .Prop("type", Schema::String().Desc("The field's type name (e.g. float, vec3, bool)."))
                                    .Prop("previousValue", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } })
                                                               .Desc("Value before the write, typed to match the field."))
                                    .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } })
                                                       .Desc("Value read back from the live component AFTER the write."))
                                    .Prop("changed", Schema::Bool().Desc("Whether the write actually changed the field."))
                                    .Prop("undoable", Schema::Bool().Desc("Whether an undo command was pushed (a no-op pushes nothing); always false for Play-mode direct writes."))
                                    .Prop("clamped", Schema::Bool().Desc("True when the requested value was clamped into the field's serializer-enforced range."))
                                    .Prop("requestedValue", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } })
                                                                .Desc("Original pre-clamp value; present only when clamped:true."))
                                    .Prop("key", Schema::String().Desc("Map key; present only for a map-keyed (dotted-field) write."))
                                    .Required({ "entity", "component", "field", "type", "previousValue", "value", "changed", "undoable", "clamped" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_EntitySetField;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_open";
            tool.Toolset = "scene";
            tool.Title = "Open / switch scene";
            // A project-WRITE tool: switching scenes discards the current in-memory
            // scene, so it is gated behind "Allow writes". readOnlyHint:false; NOT
            // idempotent (each call reloads from disk, discarding unsaved state); not
            // destructive to the project files (it never writes — the source scene is
            // untouched, only the in-editor scene changes).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Open / switch the active scene — the scriptable scene-switch that lets an agent set up a repro "
                "without a manual project StartScene edit + editor relaunch. Give a 'path' to a .olo or .scene file "
                "(relative paths resolve against the project asset directory, e.g. \"Scenes/Sandbox.olo\"; an absolute "
                "path also works). Loads the scene directly, the same install path as the editor's File > Open Scene "
                "but WITHOUT the auto-save recovery modal (a remote agent can't click it). If Play mode is running it "
                "is stopped first. Returns whether the scene loaded (ok), the resolved path, the new scene name and "
                "entity count. This is a WRITE tool: it is refused unless 'Allow writes' is enabled in the editor's "
                "MCP Server panel (off by default).";
            tool.InputSchema = SceneControl::OpenInputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("available", Schema::Bool().Desc("False when this editor build has no scene-open hook."))
                                    .Prop("ok", Schema::Bool().Desc("Whether the scene loaded."))
                                    .Prop("path", Schema::String().Desc("Resolved scene path."))
                                    .Prop("sceneName", Schema::String().Desc("Name of the newly active scene."))
                                    .Prop("entityCount", Schema::Int().Min(0))
                                    .Prop("message", Schema::String().Desc("Human-readable outcome detail."))
                                    .Required({ "available", "ok", "path", "sceneName", "entityCount", "message" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneOpen;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_play";
            tool.Toolset = "scene";
            tool.Title = "Enter Play mode";
            // A project-WRITE tool: entering Play copies the scene and runs the user's
            // game scripts, so it is gated behind "Allow writes". readOnlyHint:false;
            // idempotent (already playing -> no-op, changed:false); not destructive
            // (fully reversible — olo_scene_stop restores the authored scene).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Enter Play mode — start the runtime simulation, the same as the editor's Play button (and the "
                "OLO_EDITOR_AUTOPLAY workaround, without a relaunch). Needed to verify anything that only runs in "
                "Play: physics, cloth/soft-body, scripts, animation. Copies the authored scene and starts the "
                "runtime; already-playing is a no-op (changed:false). Entering Play can fail if the scene has no "
                "primary camera — then ok:false and the editor stays in Edit (see the message). Confirm with "
                "olo_scene_summary's isPlaying, then olo_screenshot. Fully reversible via olo_scene_stop. This is a "
                "WRITE tool: it is refused unless 'Allow writes' is enabled in the editor's MCP Server panel (off by "
                "default).";
            tool.InputSchema = SceneControl::PlayStopInputSchema();
            tool.OutputSchema = SceneControl::SceneStateOutputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_ScenePlay;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_simulate";
            tool.Toolset = "scene";
            tool.Title = "Enter Simulate mode";
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Enter Simulate mode — run physics and simulation while keeping the editor camera, the same as the "
                "editor's Simulate button. Already simulating is a no-op (changed:false); calling from Play stops "
                "Play first. The authored scene is restored by olo_scene_stop. Reports playing, simulating, and the "
                "exact resulting mode. This is a WRITE tool: it is refused unless 'Allow writes' is enabled in the "
                "editor's MCP Server panel (off by default).";
            tool.InputSchema = SceneControl::PlayStopInputSchema();
            tool.OutputSchema = SceneControl::SceneStateOutputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSimulate;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_stop";
            tool.Toolset = "scene";
            tool.Title = "Stop Play mode";
            // A project-WRITE tool for symmetry with olo_scene_play (it ends the
            // runtime and restores the authored scene). idempotent (already stopped ->
            // no-op); not destructive (restores the pre-Play authored scene).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Stop Play mode — end the runtime simulation and restore the authored (Edit-mode) scene, the same as "
                "the editor's Stop button. Discards the transient runtime scene copy, so any runtime-only changes are "
                "dropped (exactly like the editor). Already-stopped is a no-op (changed:false). Confirm with "
                "olo_scene_summary's isPlaying. This is a WRITE tool: it is refused unless 'Allow writes' is enabled "
                "in the editor's MCP Server panel (off by default).";
            tool.InputSchema = SceneControl::PlayStopInputSchema();
            // Same shape as olo_scene_play — both registrations mirror the shared
            // Handle_ScenePlayState / SceneControl::ToJson(McpScenePlayResult) payload.
            tool.OutputSchema = SceneControl::SceneStateOutputSchema();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneStop;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_reflection_probe_bake";
            tool.Toolset = "scene";
            tool.Title = "Bake reflection probe";
            tool.ProjectWrite = true;
            // Re-baking replaces the prior in-memory environment and depends on the
            // scene's current rendered state, so it is neither idempotent nor undoable.
            tool.Annotations = DestructiveMutatingAnnotations();
            tool.Description =
                "Bake the uniquely named reflection-probe entity from its Transform position using the editor's "
                "real ReflectionProbeBaker path. The entity name is an exact, case-sensitive TagComponent match; "
                "duplicate exact names are refused rather than choosing one. Requires ReflectionProbeComponent "
                "and TransformComponent. The synchronous bake replaces the probe's prior in-memory environment "
                "and is not undoable. This is a WRITE tool: it is refused unless 'Allow writes' is enabled in the "
                "editor's MCP Server panel (off by default).";
            tool.InputSchema = ReflectionProbeBake::InputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("entity", Schema::String().Desc("Exact name of the baked probe entity."))
                                    .Prop("baked", Schema::Bool().Desc("Whether the bake produced a usable environment map."))
                                    .Prop("message", Schema::String().Desc("Human-readable bake outcome."))
                                    .Required({ "entity", "baked", "message" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ReflectionProbeBake;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_editor_select_entity";
            tool.Toolset = "scene";
            tool.Title = "Select entity in editor";
            // A project-WRITE tool: it mutates the editor's Scene Hierarchy
            // selection, gated behind "Allow writes" for consistency with every
            // other editor-state write (olo_scene_open, olo_scene_play/stop) even
            // though it never touches project/scene DATA — selection isn't
            // undoable, so there is no CommandHistory entry. readOnlyHint:false;
            // idempotent (selecting the already-selected entity, or clearing an
            // already-empty selection, is a no-op); not destructive.
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Select (or clear) the entity shown in the editor's Scene Hierarchy / Properties panels — the "
                "only way to drive the Properties inspector onto a specific entity over MCP, so its rendered "
                "component UI (every DrawComponent<T>) becomes reachable for screenshot verification. "
                "olo_input_inject cannot reliably land a click on a Scene Hierarchy row (the OS cursor reasserts "
                "over the synthetic position between injected frames), so use this instead. Give 'entity' (a "
                "UUID, see olo_scene_list_entities / olo_scene_get_entity) to select it, or 'clear':true to "
                "deselect — exactly one of the two. An unknown 'entity' UUID leaves the CURRENT selection "
                "untouched and reports ok:false rather than silently clearing it. The result's 'changed' field "
                "distinguishes a real transition from a no-op (re-selecting the already-selected entity, or "
                "clearing an already-empty selection). Follow with olo_screenshot (space:'window', to capture "
                "the Properties panel outside the 3D viewport) to see the result. This is a WRITE tool: it is "
                "refused unless 'Allow writes' is enabled in the editor's MCP Server panel (off by default).";
            tool.InputSchema = SelectEntity::InputSchema();
            tool.OutputSchema = Schema::Object()
                                    .Prop("available", Schema::Bool().Desc("False when this editor build has no selection hook."))
                                    .Prop("ok", Schema::Bool().Desc("Requested transition applied; an unknown UUID is ok:false with the current selection untouched."))
                                    .Prop("changed", Schema::Bool().Desc("Real transition vs. no-op (re-selecting the already-selected entity, or clearing an already-empty selection)."))
                                    .Prop("selected", Schema::Bool().Desc("Whether an entity is selected after the call."))
                                    .Prop("message", Schema::String().Desc("Human-readable outcome detail."))
                                    .Prop("entity", Schema::String().Desc("Selected entity UUID; present only when selected:true."))
                                    .Prop("name", Schema::String().Desc("Selected entity name; present only when selected:true."))
                                    .Required({ "available", "ok", "changed", "selected", "message" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SelectEntity;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scheduler_graph";
            tool.Toolset = "scene";
            tool.Title = "Gameplay system schedule";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Export the DERIVED dependency graph of the per-tick gameplay systems (Scene::"
                "GetGameplayScheduler) as structured JSON, a Mermaid flowchart, or Graphviz DOT. Systems declare "
                "the named channels they read/write plus optional after/before ordering, and the execution order "
                "is derived by a topological sort — so the graph you have to reason about is written down in no "
                "source file, and until now could only be interrogated one yes/no question at a time via "
                "SystemScheduler::DependsOn.\n\n"
                "Reports the full derived edge set (including the read/write hazard edges — RAW/WAW/WAR — which "
                "are the majority and the ones no source file shows), the execution order, every channel with "
                "its readers and writers, and, for each Parallelizable system, 'mayOverlapWith': the other "
                "marked systems it is NOT ordered against and can therefore genuinely race. That last field is "
                "the point of the tool — a MISSING edge is invisible in the sequential order (the "
                "registration-order tie-break supplies it anyway) and only becomes a data race under the "
                "parallel executor. Use it to check a new edge landed, to see what a system you just marked "
                "Parallelizable must be audited against, or to spot a misspelled channel (it appears with "
                "writers and no readers). Sibling of olo_render_graph_topology_export for the other DAG.";
            tool.InputSchema = Schema::Object()
                                   .Prop("format", Schema::String()
                                                       .Enum({ "json", "mermaid", "dot" })
                                                       .Desc("'json' (default) for the full structured document; "
                                                             "'mermaid' for a flowchart LR DAG; 'dot' for Graphviz."))
                                   .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("systemCount", Schema::Int().Min(0))
                    .Prop("parallelSystemCount", Schema::Int().Min(0).Desc("Systems marked Parallelizable."))
                    .Prop("parallelExecutionEnabled", Schema::Bool().Desc("False when the process-wide kill-switch is off (OLO_GAMEPLAY_SCHEDULER_SEQUENTIAL=1) or nothing is marked — every system then runs on the calling thread in the same derived order."))
                    .Prop("executionOrder", Schema::Array(Schema::String()).Desc("System names in derived run order."))
                    .Prop("systems", Schema::Array(Schema::Object()
                                                       .Prop("name", Schema::String())
                                                       .Prop("orderIndex", Schema::Int().Min(0))
                                                       .Prop("parallel", Schema::Bool())
                                                       .Prop("reads", Schema::Array(Schema::String()))
                                                       .Prop("writes", Schema::Array(Schema::String()))
                                                       .Prop("after", Schema::Array(Schema::String()).Desc("Explicit After() declarations; omitted when none."))
                                                       .Prop("before", Schema::Array(Schema::String()).Desc("Explicit Before() declarations; omitted when none."))
                                                       .Prop("mayOverlapWith", Schema::Array(Schema::String()).Desc("Parallel systems only: the other marked systems this one is NOT transitively ordered against, i.e. exactly what its thread-safety audit must cover."))))
                    .Prop("edgeCount", Schema::Int().Min(0))
                    .Prop("edges", Schema::Array(Schema::Object().Prop("from", Schema::String()).Prop("to", Schema::String()))
                                       .Desc("Derived ordering constraints: 'from' must finish before 'to' may start."))
                    .Prop("channelCount", Schema::Int().Min(0))
                    .Prop("channels", Schema::Array(Schema::Object()
                                                        .Prop("name", Schema::String())
                                                        .Prop("readers", Schema::Array(Schema::String()))
                                                        .Prop("writers", Schema::Array(Schema::String())))
                                          .Desc("Every named channel with its readers and writers — the inverse index. A channel with writers and no readers (or vice versa) is usually a typo in one declaration."))
                    .Prop("note", Schema::String())
                    .Required({ "systemCount", "parallelSystemCount", "executionOrder", "systems", "edgeCount", "edges" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SchedulerGraph;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
