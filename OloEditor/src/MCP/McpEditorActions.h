#pragma once

// The editor command registry (issue #1131, slice 1): the editor's own actions,
// DECLARED — menu items, toolbar buttons and shortcuts — each mapped to the
// automation command that performs it, or to the reason none does.
//
// Why a table and not a parallel mechanism: the issue asked that editor actions
// be "exposed through the same Automation Registry as everything else". Almost
// every menu action already had a registry command by the time this landed
// (scene new/open/save, play/simulate/stop, undo/redo, panels, reload). What was
// missing was (a) the four that had no editor hook at all — pause/resume, step,
// gizmo mode, Build Shader Pack — and (b) the DECLARATION: a single place an
// agent can ask "what can this editor do, and which command does it?" without
// reading EditorLayer.cpp. kEditorActions is that declaration; olo_editor_actions
// serves it joined with the live registry (authority class, undo class,
// availability on this host). An action listed with an empty Command is
// declared NOT automated, and Note says why — a reader gets the honest gap, not
// an absence they have to infer.
//
// Header-only and engine-free apart from McpServer.h's result structs and the
// schema DSL, so the table, the schemas and the result shaping unit-test with no
// editor (McpAutomationEditorCommandsTest.cpp); the actions themselves run
// through EditorMcpContext hooks the editor fills in, like every other write.

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"

#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <string_view>

namespace OloEngine::MCP::EditorActions
{
    using Json = nlohmann::json;

    // ---- gizmo mode ---------------------------------------------------------

    // The viewport gizmo modes the Q/W/E/R shortcuts select. Spelled as the
    // strings olo_editor_gizmo_set accepts and reports; the editor maps them to
    // ImGuizmo's operation ids in EditorLayer (kept out of here so the header
    // needs no ImGuizmo).
    inline constexpr std::array<std::string_view, 4> kGizmoModes{ "none", "translate", "rotate", "scale" };

    [[nodiscard]] inline bool IsGizmoMode(std::string_view mode)
    {
        for (const std::string_view candidate : kGizmoModes)
        {
            if (candidate == mode)
                return true;
        }
        return false;
    }

    // ---- the declared action table -----------------------------------------

    struct EditorActionDescriptor
    {
        std::string_view Name;     // stable snake_case identity, e.g. "scene_save"
        std::string_view MenuPath; // where a human finds it: "File > Save Scene", "Toolbar > Pause"
        std::string_view Shortcut; // "" when there is none
        std::string_view Command;  // the registry command that performs it, or "" when none does
        std::string_view Note;     // for an un-automated action, why; otherwise a one-line hint
    };

    // Every action the editor's menu bar, toolbar and shortcut map expose, in
    // menu order. The names are the identities olo_editor_actions reports; the
    // Command column is checked against the real registry by
    // McpAutomationEditorCommandsTest, so a renamed command breaks the build's
    // tests rather than the table.
    inline constexpr std::array kEditorActions{
        // File
        EditorActionDescriptor{ "project_new", "File > New Project", "", "",
                                "Project switching restarts most of the editor; it is #1132's headless-host territory." },
        EditorActionDescriptor{ "project_open", "File > Open Project", "", "",
                                "Project switching restarts most of the editor; it is #1132's headless-host territory." },
        EditorActionDescriptor{ "project_save", "File > Save Project", "", "",
                                "EditorLayer::SaveProject is a stub today; nothing to automate until it does something." },
        EditorActionDescriptor{ "scene_new", "File > New Scene", "Ctrl+N", "olo_scene_new", "" },
        EditorActionDescriptor{ "scene_open", "File > Open Scene", "Ctrl+O", "olo_scene_open",
                                "Takes a path instead of opening the file dialog." },
        EditorActionDescriptor{ "scene_save", "File > Save Scene", "Ctrl+S", "olo_scene_save", "" },
        EditorActionDescriptor{ "scene_save_as", "File > Save Scene As", "Ctrl+Shift+S", "olo_scene_save_as",
                                "Takes a path instead of opening the file dialog." },
        EditorActionDescriptor{ "mesh_export_gltf", "File > Export Mesh to glTF", "", "",
                                "Drives a file dialog and the selection; no non-interactive path exists yet." },
        EditorActionDescriptor{ "quick_save", "File > Quick Save", "F5", "",
                                "A save-game slot of the running game, not an editor document; see the SaveGame panel." },
        EditorActionDescriptor{ "quick_load", "File > Quick Load", "F9", "",
                                "A save-game slot of the running game, not an editor document; see the SaveGame panel." },
        EditorActionDescriptor{ "exit", "File > Exit", "", "", "Never automated: it prompts to discard unsaved work." },
        // Edit
        EditorActionDescriptor{ "undo", "Edit > Undo", "Ctrl+Z", "olo_editor_undo", "" },
        EditorActionDescriptor{ "redo", "Edit > Redo", "Ctrl+Y", "olo_editor_redo", "" },
        EditorActionDescriptor{ "preferences", "Edit > Preferences", "", "",
                                "Opens a modal panel; the process-global settings it holds are olo_accessibility_get/set." },
        EditorActionDescriptor{ "entity_duplicate", "Edit > Duplicate", "Ctrl+D", "olo_entity_duplicate",
                                "Takes the entity explicitly rather than the current selection." },
        EditorActionDescriptor{ "entity_copy", "Edit > Copy", "Ctrl+C", "",
                                "Clipboard-only; olo_entity_duplicate covers the copy-then-paste use." },
        EditorActionDescriptor{ "entity_paste", "Edit > Paste", "Ctrl+V", "",
                                "Clipboard-only; olo_entity_duplicate covers the copy-then-paste use." },
        EditorActionDescriptor{ "entity_delete", "Edit > Delete", "Delete", "olo_entity_destroy",
                                "Takes the entity explicitly rather than the current selection." },
        EditorActionDescriptor{ "entity_select", "Scene Hierarchy > click", "", "olo_editor_select_entity", "" },
        EditorActionDescriptor{ "gizmo_mode", "Viewport > gizmo mode", "Q/W/E/R", "olo_editor_gizmo_set", "" },
        EditorActionDescriptor{ "camera_frame_entity", "Viewport > frame selection", "F", "olo_camera_frame_entity",
                                "Takes the entity explicitly rather than the current selection." },
        // Toolbar
        EditorActionDescriptor{ "play", "Toolbar > Play", "", "olo_scene_play", "" },
        EditorActionDescriptor{ "simulate", "Toolbar > Simulate", "", "olo_scene_simulate", "" },
        EditorActionDescriptor{ "stop", "Toolbar > Stop", "", "olo_scene_stop", "" },
        EditorActionDescriptor{ "pause", "Toolbar > Pause / Resume", "", "olo_editor_pause", "" },
        EditorActionDescriptor{ "step", "Toolbar > Step", "", "olo_editor_step", "Only while paused." },
        // Script / Shaders
        EditorActionDescriptor{ "script_reload", "Script > Reload assembly", "Ctrl+R", "olo_reload_script", "" },
        EditorActionDescriptor{ "shader_reload", "Shaders > Reload shader", "", "olo_shader_reload",
                                "Reloads one shader by name; the menu item reloads the whole 2D library." },
        // Build
        EditorActionDescriptor{ "asset_pack_build", "Build > Build Asset Pack", "", "",
                                "Runs on a worker with progress and cancel through the Asset Pack Builder panel; "
                                "not yet a command." },
        EditorActionDescriptor{ "shader_pack_build", "Build > Build Shader Pack", "", "olo_editor_build_shader_pack", "" },
        EditorActionDescriptor{ "lightmap_bake", "Build > Bake Lightmaps", "", "olo_lightmap_bake", "" },
        EditorActionDescriptor{ "asset_references_validate", "Build > Validate Asset References", "",
                                "olo_project_validate", "Composes the same validators the menu item runs." },
        EditorActionDescriptor{ "build_game", "Build > Build Game...", "", "",
                                "A panel; olo_editor_panel_set opens it and olo_build_run builds the CMake targets." },
        // Window / Debug
        EditorActionDescriptor{ "panel_toggle", "Window / Debug > (any panel)", "", "olo_editor_panel_set",
                                "One command for every panel checkbox; olo_editor_panel_list names them." },
    };

    [[nodiscard]] inline const EditorActionDescriptor* Find(std::string_view name)
    {
        for (const EditorActionDescriptor& action : kEditorActions)
        {
            if (action.Name == name)
                return &action;
        }
        return nullptr;
    }

    // ---- schemas ------------------------------------------------------------

    [[nodiscard]] inline Json ActionsInputSchema()
    {
        return Schema::Object()
            .Prop("automatedOnly", Schema::Bool().Desc("Only list actions that have a registry command (default false)."))
            .NoAdditional();
    }

    // A Schema::Node rather than a Json: Schema::Array(const Node&) takes it, and
    // Node's Json constructor is explicit by design.
    [[nodiscard]] inline Schema::Node ActionSchema()
    {
        return Schema::Object()
            .Prop("name", Schema::String())
            .Prop("menuPath", Schema::String())
            .Prop("shortcut", Schema::String().Desc("Empty when the action has no keyboard shortcut."))
            .Prop("command", Schema::String().Desc("The registry command that performs the action; absent when none does."))
            .Prop("available", Schema::Bool().Desc("Whether that command can run on this host right now; absent when there is no command."))
            .Prop("projectWrite", Schema::Bool().Desc("The command's authority class; absent when there is no command."))
            .Prop("undo", Schema::String().Desc("The command's declared AutomationUndo; absent when there is no command."))
            .Prop("note", Schema::String().Desc("Why the action is not automated, or a hint about how the command differs from the click."))
            .Required({ "name", "menuPath", "shortcut", "note" });
    }

    [[nodiscard]] inline Json ActionsOutputSchema()
    {
        return Schema::Object()
            .Prop("count", Schema::Int().Min(0))
            .Prop("automated", Schema::Int().Min(0).Desc("How many of the listed actions have a registry command."))
            .Prop("actions", Schema::Array(ActionSchema()))
            .Required({ "count", "automated", "actions" });
    }

    [[nodiscard]] inline Json PauseInputSchema()
    {
        return Schema::Object()
            .Prop("paused", Schema::Bool().Desc("true pauses the running Play/Simulate session, false resumes it."))
            .Required({ "paused" })
            .NoAdditional();
    }

    [[nodiscard]] inline Json PauseOutputSchema()
    {
        return Schema::Object()
            .Prop("available", Schema::Bool())
            .Prop("ok", Schema::Bool())
            .Prop("changed", Schema::Bool())
            .Prop("paused", Schema::Bool())
            .Prop("mode", Schema::String().Enum({ "edit", "play", "simulate" }))
            .Prop("sceneName", Schema::String())
            .Prop("message", Schema::String())
            .Required({ "available", "ok", "changed", "paused", "mode", "message" });
    }

    [[nodiscard]] inline Json StepInputSchema()
    {
        return Schema::Object()
            .Prop("frames", Schema::Int().Min(1).Max(60).Desc("How many frames to advance (default 1)."))
            .NoAdditional();
    }

    [[nodiscard]] inline Json StepOutputSchema()
    {
        return Schema::Object()
            .Prop("available", Schema::Bool())
            .Prop("ok", Schema::Bool())
            .Prop("paused", Schema::Bool())
            .Prop("framesRequested", Schema::Int().Min(0))
            .Prop("mode", Schema::String().Enum({ "edit", "play", "simulate" }))
            .Prop("sceneName", Schema::String())
            .Prop("message", Schema::String())
            .Required({ "available", "ok", "paused", "framesRequested", "mode", "message" });
    }

    [[nodiscard]] inline Json GizmoInputSchema()
    {
        return Schema::Object()
            .Prop("mode", Schema::String()
                              .Enum({ "none", "translate", "rotate", "scale" })
                              .Desc("The gizmo to show on the selected entity; the Q/W/E/R shortcuts in order."))
            .Required({ "mode" })
            .NoAdditional();
    }

    [[nodiscard]] inline Json GizmoOutputSchema()
    {
        return Schema::Object()
            .Prop("available", Schema::Bool())
            .Prop("ok", Schema::Bool())
            .Prop("changed", Schema::Bool())
            .Prop("mode", Schema::String().Enum({ "none", "translate", "rotate", "scale" }))
            .Prop("message", Schema::String())
            .Required({ "available", "ok", "changed", "mode", "message" });
    }

    [[nodiscard]] inline Json ShaderPackInputSchema()
    {
        return Schema::Object().NoAdditional();
    }

    [[nodiscard]] inline Json ShaderPackOutputSchema()
    {
        return Schema::Object()
            .Prop("available", Schema::Bool())
            .Prop("ok", Schema::Bool())
            .Prop("outputPath", Schema::String().Desc("Where the pack was written, relative to the editor's working directory."))
            .Prop("message", Schema::String())
            .Required({ "available", "ok", "outputPath", "message" });
    }

    // ---- result shaping -----------------------------------------------------

    [[nodiscard]] inline Json ToJson(const McpEditorPauseResult& r)
    {
        return Json{ { "available", r.Available }, { "ok", r.Ok }, { "changed", r.Changed }, { "paused", r.Paused }, { "mode", r.Mode }, { "sceneName", r.SceneName }, { "message", r.Message } };
    }

    [[nodiscard]] inline Json ToJson(const McpEditorStepResult& r)
    {
        return Json{ { "available", r.Available }, { "ok", r.Ok }, { "paused", r.Paused }, { "framesRequested", r.FramesRequested }, { "mode", r.Mode }, { "sceneName", r.SceneName }, { "message", r.Message } };
    }

    [[nodiscard]] inline Json ToJson(const McpEditorGizmoResult& r)
    {
        return Json{ { "available", r.Available }, { "ok", r.Ok }, { "changed", r.Changed }, { "mode", r.Mode }, { "message", r.Message } };
    }

    [[nodiscard]] inline Json ToJson(const McpEditorShaderPackResult& r)
    {
        return Json{ { "available", r.Available },
                     { "ok", r.Ok },
                     { "outputPath", r.OutputPath },
                     { "message", r.Message } };
    }
} // namespace OloEngine::MCP::EditorActions
