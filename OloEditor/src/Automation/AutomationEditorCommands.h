#pragma once

namespace OloEngine::Automation
{
    class AutomationRegistry;

    // The editor command registry (issue #1131, slice 1).
    //
    // Registers olo_editor_actions -- the DECLARED table of the editor's menu,
    // toolbar and shortcut actions (MCP/McpEditorActions.h) joined with the live
    // registry, so an agent can ask "what can this editor do, and which command
    // does it?" -- and the four actions that had no automation hook before this
    // slice: olo_editor_pause, olo_editor_step, olo_editor_gizmo_set and
    // olo_editor_build_shader_pack.
    //
    // The four actions run through EditorMcpContext hooks that EditorLayer fills
    // in with the SAME code path the button or menu item runs. A host that owns
    // no editor leaves the hooks null; the commands then declare themselves
    // unavailable (AutomationCommand::IsAvailable) rather than failing deeper.
    void RegisterEditorCommands(AutomationRegistry& registry);
} // namespace OloEngine::Automation
