#include "OloEnginePCH.h"
#include "MCP/McpTools.h"
#include "MCP/McpEventStream.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpServer.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/DiagnosticsEventLog.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Composition point of the built-in MCP tool surface. The per-domain tool
// registrations live in their own TUs (McpToolsDiagnostics.cpp, McpToolsScene.cpp,
// ...; issue #357 split of the former 322 KB monolith); this file only composes
// them in a fixed order and registers the resources + prompts, so the full
// surface is assembled in exactly one place.

namespace OloEngine::MCP
{
    namespace
    {
        // How many diagnostics events `olo://events/recent` returns. Matches the
        // engine-log resource's 200-message window: enough to answer "what just
        // happened?" after a reconnect without shipping the whole 512-entry ring on
        // every subscription wake-up.
        constexpr std::size_t kEventsResourceMaxCount = 200;

        void RegisterBuiltinResources(McpServer& server)
        {
            // ---- Resources ---------------------------------------------------------

            {
                ResourceDef resource;
                resource.Uri = "olo://scene/current";
                resource.Name = "Current scene (YAML)";
                resource.Description = "The active scene serialized to YAML (every entity and component), read "
                                       "live from the editor on the main thread.";
                resource.MimeType = "text/yaml";
                resource.Reader = [](McpServer& s) -> std::string
                {
                    const Json marshaled = s.MarshalRead([&s]() -> Json
                                                         {
                        const Ref<Scene> scene = s.Context().GetActiveScene ? s.Context().GetActiveScene() : nullptr;
                        if (!scene)
                            return Json{ { "__error", "No active scene" } };
                        SceneSerializer serializer(scene);
                        return Json(serializer.SerializeToYAML()); });
                    if (marshaled.is_object() && marshaled.contains("__error"))
                        throw std::runtime_error(marshaled["__error"].get<std::string>());
                    return marshaled.get<std::string>();
                };
                server.RegisterResource(std::move(resource));
            }

            {
                ResourceDef resource;
                resource.Uri = "olo://logs/recent";
                resource.Name = "Recent engine logs";
                resource.Description = "The most recent engine log messages (up to 200) from the in-memory ring buffer.";
                resource.MimeType = "text/plain";
                resource.Reader = [](McpServer& /*s*/) -> std::string
                {
                    const std::vector<std::string> messages = Log::Get().GetRecentLogMessages(200);
                    std::string out;
                    for (const auto& message : messages)
                    {
                        std::string_view line = message;
                        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                            line.remove_suffix(1);
                        out.append(line);
                        out.push_back('\n');
                    }
                    return out;
                };
                server.RegisterResource(std::move(resource));
            }

            {
                // The `logging`-capability offramp (issue #777). The diagnostics
                // event stream is pushed today as `notifications/message` log
                // notifications over the GET SSE stream, and `logging` is deprecated
                // as of spec 2026-07-28. This resource is the successor carrier, and
                // it was chosen because it is the ONLY push shape that survives the
                // era change unchanged: in 2026-07-28 the GET stream is gone,
                // `notifications/message` becomes request-scoped, and the
                // `subscriptions/listen` filter is a CLOSED set of four notification
                // types — so a bespoke custom notification is not an option there,
                // while `notifications/resources/updated` + `resources/read` is.
                // Migrating later moves the plumbing (resources/subscribe ->
                // subscriptions/listen) and leaves this resource and the
                // notification untouched. See docs/agent-rules/mcp-protocol-eras.md.
                //
                // The payload is the same per-event shape olo_events_tail returns,
                // so a push subscriber and an incremental poller read identical
                // records — the property McpEventStream.h already guarantees for the
                // `notifications/message` carrier.
                ResourceDef resource;
                resource.Uri = "olo://events/recent";
                resource.Name = "Recent diagnostics events";
                // The window size and the cursor both belong in the description: a
                // reader that cannot tell this is the newest 200 rather than the whole
                // history has no reason to resume from `lastId`, and would silently
                // miss everything older on a busy session.
                resource.Description =
                    "The most recent 'what just happened?' engine events (up to 200: scene load, play/stop, "
                    "entity spawn/destroy, asset reload, script error) as JSON, newest last, with a 'lastId' "
                    "cursor to resume from. Subscribable: resources/subscribe on this URI and the server "
                    "pushes notifications/resources/updated whenever new events are recorded.";
                resource.MimeType = "application/json";
                resource.Reader = [](McpServer& /*s*/) -> std::string
                {
                    // Reads the mutex-guarded ring directly — no MarshalRead: the log
                    // is written from the game thread but is thread-safe by contract,
                    // and a resource read must not stall on a frame.
                    DiagnosticEventQuery query;
                    query.MaxCount = kEventsResourceMaxCount;
                    const DiagnosticEventQueryResult snapshot = DiagnosticsEventLog::Get().QueryWithCursor(query);
                    Json events = Json::array();
                    for (const DiagnosticEvent& event : snapshot.Events)
                        events.push_back(EventToJson(event));
                    // `lastId` lets a reader resume with olo_events_tail's sinceId
                    // instead of re-reading the whole window.
                    return Json{ { "events", std::move(events) }, { "lastId", snapshot.LastId } }.dump(2);
                };
                // Monotonic and cheap: the id of the newest event ever recorded. It
                // only ever increases, so a token change is exactly "something new
                // happened" — never a false positive from eviction.
                resource.ChangeToken = []
                { return DiagnosticsEventLog::Get().LastId(); };
                server.RegisterResource(std::move(resource));
            }
        }

        void RegisterBuiltinPrompts(McpServer& server)
        {
            // ---- Prompts (canned workflows for non-expert users) -------------------

            {
                PromptDef prompt;
                prompt.Name = "diagnose-performance";
                prompt.Title = "Diagnose performance";
                prompt.Description = "Find out why the running scene is slow and what to do about it.";
                prompt.Text =
                    "Diagnose the performance of the scene currently running in OloEditor. Steps:\n"
                    "1. Call olo_perf_bottlenecks to see whether the frame is CPU/GPU/Memory/IO bound.\n"
                    "2. Call olo_perf_snapshot for fps, frame time, draw calls and instancing counts.\n"
                    "3. If GPU-bound, call olo_perf_capture_frame to find the most expensive passes/draw calls.\n"
                    "4. Call olo_memory_report if memory is implicated.\n"
                    "Then give a short, prioritized list of concrete fixes, citing the specific numbers you saw.";
                server.RegisterPrompt(std::move(prompt));
            }

            {
                PromptDef prompt;
                prompt.Name = "explain-last-script-error";
                prompt.Title = "Explain my last script error";
                prompt.Description = "Explain the most recent C#/Lua script exception and how to fix it.";
                prompt.Text =
                    "Explain the most recent scripting error in this OloEngine project. Steps:\n"
                    "1. Call olo_script_get_last_errors to get the latest C#/Lua exceptions (message, script, entity).\n"
                    "2. If an entity id is given, call olo_scene_get_entity on it to see its components.\n"
                    "3. Call olo_script_get_api (matching the error's language) to check the correct API usage.\n"
                    "4. Optionally call olo_log_tail for surrounding context.\n"
                    "Then explain the root cause in plain terms and give a concrete fix.";
                server.RegisterPrompt(std::move(prompt));
            }

            {
                PromptDef prompt;
                prompt.Name = "why-cant-i-see-my-object";
                prompt.Title = "Why can't I see my object?";
                prompt.Description = "Figure out why an entity isn't visible in the scene.";
                prompt.Text =
                    "Help figure out why an object isn't visible in the running OloEditor scene. Steps:\n"
                    "1. Call olo_scene_summary to confirm a scene is loaded and whether it's playing.\n"
                    "2. Call olo_scene_list_entities (optionally with a namePattern) to find the entity.\n"
                    "3. Call olo_scene_get_entity on it: check its Transform (position/scale), whether it has a "
                    "MeshComponent/MaterialComponent, and whether it's parented oddly.\n"
                    "4. Call olo_screenshot to see the current frame, and olo_shader_errors in case its material's "
                    "shader failed to compile.\n"
                    "Then state the most likely reason it's not visible and how to fix it.";
                server.RegisterPrompt(std::move(prompt));
            }
        }
    } // namespace

    void RegisterBuiltinTools(McpServer& server)
    {
        // One call per domain TU. Within a domain the registration order is
        // stable, so tools/list is grouped by toolset; the domain order below
        // follows each toolset's first appearance in the pre-split flat list.
        RegisterDiagnosticsTools(server);
        RegisterSceneTools(server);
        RegisterPerfTools(server);
        RegisterRenderTools(server);
        RegisterShaderTools(server);
        RegisterAssetTools(server);
        RegisterScriptingTools(server);
        RegisterCameraTools(server);
        RegisterPhysicsTools(server);
        RegisterInputTools(server);
        RegisterBenchmarkTools(server);

        RegisterBuiltinResources(server);
        RegisterBuiltinPrompts(server);
    }
} // namespace OloEngine::MCP
