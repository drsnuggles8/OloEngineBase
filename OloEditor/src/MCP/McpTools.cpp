#include "OloEnginePCH.h"
#include "MCP/McpTools.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpServer.h"

#include "OloEngine/Core/Log.h"
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

        RegisterBuiltinResources(server);
        RegisterBuiltinPrompts(server);
    }
} // namespace OloEngine::MCP
