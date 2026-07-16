#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpGenericFieldWrite.h"
#include "MCP/McpSceneControl.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

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
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_entity_set_field (main-marshaled; PROJECT WRITE) --------------
        // The GENERIC consented, undoable write tool (#306 item C, second slice):
        // set ANY registered component field by (component, field, value) through the
        // editor's undo stack — the catch-all successor to olo_set_collision_layer's
        // one-tool-per-field shape. Gated at dispatch by the "Allow writes" session
        // toggle (ToolDef::ProjectWrite); the shared reflect+coerce+apply core lives
        // in McpGenericFieldWrite.h so it is unit-tested at the dispatch seam without
        // this TU. The command is built + executed inside the MarshalRead job, i.e.
        // on the main thread, since it touches the EnTT registry and command stack.
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
                CommandHistory* history = server.Context().GetCommandHistory
                                              ? server.Context().GetCommandHistory()
                                              : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                if (!history)
                    return Json{ { "__error", "No editor command history available." } };

                const GenericFieldWrite::ApplyResult applied =
                    GenericFieldWrite::Apply(scene, *history, entityUuid, component, field, value);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                return applied.Data; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
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

            return ToolResult::Text(result.dump(2));
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
        // long-lived editor instance, #316 Part 5 follow-up). olo_scene_open /
        // olo_scene_play / olo_scene_stop are the only tools that legitimately need
        // more room; every other MarshalRead call site is a fast query and keeps
        // the 5s default deliberately, so slow tools genuinely hang rather than
        // silently blocking behind the queue for minutes.
        constexpr std::chrono::milliseconds kSceneControlTimeout{ 120000 };

        // ---- olo_scene_open (main-marshaled; PROJECT WRITE) --------------------
        // Open / switch the active scene over MCP — the consented-write scene switch
        // (issue #316 Part 5). Loads the requested scene file directly through the
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

            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_scene_play / olo_scene_stop (main-marshaled; PROJECT WRITE) ---
        // Toggle Play mode over MCP — the consented-write, fully-reversible play/stop
        // switch (issue #316 Part 5). Wraps the editor's OnScenePlay / OnSceneStop
        // (the same path the editor's Play / Stop toolbar buttons drive) through the
        // SetScenePlayState hook. Gated at dispatch by the "Allow writes" session
        // toggle (ToolDef::ProjectWrite): entering Play copies the scene and executes
        // the user's game scripts, so it crosses the read-only line — but it is
        // transient (stopping restores the authored scene, exactly like the editor).
        // olo_scene_summary reports `isPlaying`, so an agent can confirm the
        // transition took. The transition runs inside the MarshalRead job (main
        // thread), since it mutates scene state.
        ToolResult Handle_ScenePlayState(McpServer& server, bool play)
        {
            using namespace SceneControl;

            if (!server.Context().SetScenePlayState)
                return ToolResult::Error("Play-mode control is not available in this editor build.");

            const Json result = server.MarshalRead([&server, play]() -> Json
                                                   {
                if (!server.Context().SetScenePlayState)
                    return Json{ { "__error", "Play-mode control is not available in this editor build." } };
                const McpScenePlayResult r = server.Context().SetScenePlayState(play);
                return ToJson(r); },
                                                   kSceneControlTimeout);

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Text(result.dump(2));
        }

        ToolResult Handle_ScenePlay(McpServer& server, const Json&)
        {
            return Handle_ScenePlayState(server, /*play*/ true);
        }

        ToolResult Handle_SceneStop(McpServer& server, const Json&)
        {
            return Handle_ScenePlayState(server, /*play*/ false);
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
            tool.MainMarshaled = true;
            tool.Handler = Handle_EntityListFields;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_entity_set_field";
            tool.Toolset = "scene";
            tool.Title = "Set component field (undoable)";
            // The generic project-WRITE tool (#306 item C, second slice): gated behind
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
                "Server panel (off by default). Discover the exact writable (component, field) names, value shapes "
                "and ranges for an entity with olo_entity_list_fields.";
            tool.InputSchema = GenericFieldWrite::InputSchema();
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
            tool.MainMarshaled = true;
            tool.Handler = Handle_ScenePlay;
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
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneStop;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP
