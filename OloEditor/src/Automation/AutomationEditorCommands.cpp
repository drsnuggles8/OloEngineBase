#include "OloEnginePCH.h"
#include "Automation/AutomationEditorCommands.h"

#include "Automation/AutomationCommand.h"
#include "Automation/AutomationHost.h"
#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationResult.h"
#include "Automation/AutomationTransaction.h"
#include "MCP/McpEditorActions.h"
#include "MCP/McpServer.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

// The editor command registry (issue #1131, slice 1).
//
// Five commands, all in the `editor` toolset. olo_editor_actions is the
// declaration: it walks MCP::EditorActions::kEditorActions and joins every row
// with the registry it was registered into (the handler captures `registry`,
// exactly as olo_transaction_apply does), so the answer carries the live facts
// -- is the command available on THIS host, is it a project write, how is it
// taken back -- rather than a copy of them that could drift. A row that names a
// command the registry does not hold is still reported, marked unavailable and
// annotated, never dropped: the table is checked against the real surface by
// McpAutomationEditorCommandsTest, but a host that registers a subset (a test,
// a future headless host) must not make a declared action silently vanish.
//
// The other four are the actions that had no hook at all before #1131. Their
// authority classes follow what they touch, not what they resemble:
//   * pause / step / gizmo are EPHEMERAL RUNTIME CONTROL of the editor session
//     -- nothing they change is persisted, so they are ProjectWrite=false with
//     mutating annotations, the same class as olo_viewport_set_size, not
//     olo_scene_play (which runs the user's game scripts).
//   * build_shader_pack WRITES assets/ShaderPack.osp into the project, so it is
//     ProjectWrite=true, consent-gated, and Irreversible.

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
        namespace EditorActions = MCP::EditorActions;

        // MCP ToolAnnotations, hand-written like AutomationTransactionCommands.cpp
        // does rather than pulling MCP/McpToolsCommon.h (a transport-side header)
        // into the automation layer. Every command here acts on the local editor
        // session, so openWorldHint is false throughout.
        Json ReadOnlyAnnotations()
        {
            return Json{ { "readOnlyHint", true }, { "openWorldHint", false } };
        }

        Json MutatingAnnotations(bool idempotent)
        {
            Json annotations{ { "readOnlyHint", false }, { "openWorldHint", false }, { "destructiveHint", false } };
            if (idempotent)
                annotations["idempotentHint"] = true;
            return annotations;
        }

        // A write that overwrites a file (the shader pack) but lands in the same
        // state every time it runs: destructive AND idempotent.
        Json DestructiveIdempotentAnnotations()
        {
            return Json{ { "readOnlyHint", false },
                         { "destructiveHint", true },
                         { "idempotentHint", true },
                         { "openWorldHint", false } };
        }

        // ---- olo_editor_actions ---------------------------------------------

        Json DescribeAction(const EditorActions::EditorActionDescriptor& action,
                            const AutomationRegistry::CommandSnapshot& snapshot, const IAutomationHost& host)
        {
            Json entry{ { "name", std::string(action.Name) },
                        { "menuPath", std::string(action.MenuPath) },
                        { "shortcut", std::string(action.Shortcut) },
                        { "note", std::string(action.Note) } };
            if (action.Command.empty())
                return entry;

            const std::string commandName(action.Command);
            entry["command"] = commandName;
            if (const AutomationCommand* command = AutomationRegistry::Find(*snapshot, commandName); command != nullptr)
            {
                entry["available"] = command->AvailableOn(host);
                entry["projectWrite"] = command->ProjectWrite;
                // The same token olo_transaction_apply's reports use for the same
                // enum, so a reader learns one spelling.
                entry["undo"] = std::string(UndoToken(command->Undo));
                return entry;
            }

            // Declared, but this registry does not hold it. Say so in the row
            // rather than dropping the row: the declaration is the point.
            entry["available"] = false;
            std::string note = entry["note"].get<std::string>();
            if (!note.empty())
                note += ' ';
            note += "(not registered on this host)";
            entry["note"] = std::move(note);
            return entry;
        }

        AutomationResult HandleEditorActions(const AutomationRegistry& registry, const IAutomationHost& host,
                                             const Json& arguments)
        {
            if (arguments.contains("automatedOnly") && !arguments["automatedOnly"].is_boolean())
                return AutomationResult::Error("Expected 'automatedOnly' (boolean).");
            const bool automatedOnly = arguments.value("automatedOnly", false);

            // ONE snapshot for the whole listing, so every row is joined against
            // the same command vector.
            const AutomationRegistry::CommandSnapshot snapshot = registry.Snapshot();

            Json actions = Json::array();
            sizet automated = 0;
            for (const EditorActions::EditorActionDescriptor& action : EditorActions::kEditorActions)
            {
                const bool hasCommand = !action.Command.empty();
                if (automatedOnly && !hasCommand)
                    continue;
                if (hasCommand)
                    ++automated;
                actions.push_back(DescribeAction(action, snapshot, host));
            }

            return AutomationResult::Structured(
                Json{ { "count", actions.size() }, { "automated", automated }, { "actions", std::move(actions) } });
        }

        // ---- the four editor actions -------------------------------------------

        AutomationResult HandleEditorPause(IAutomationHost& host, const Json& arguments)
        {
            if (!host.Context().SetScenePauseState)
                return AutomationResult::Error("Pause/resume is not available in this host (no editor).");
            if (!arguments.contains("paused") || !arguments["paused"].is_boolean())
                return AutomationResult::Error("Expected 'paused' (boolean).");
            const bool paused = arguments["paused"].get<bool>();

            const Json result = host.MarshalRead([&host, paused]() -> Json
                                                 { return EditorActions::ToJson(host.Context().SetScenePauseState(paused)); });
            if (!result.value("ok", false))
                return AutomationResult::Error(result.value("message", "Could not change the pause state."));
            return AutomationResult::Structured(result);
        }

        AutomationResult HandleEditorStep(IAutomationHost& host, const Json& arguments)
        {
            if (!host.Context().StepScene)
                return AutomationResult::Error("Stepping is not available in this host (no editor).");
            int frames = 1;
            if (arguments.contains("frames"))
            {
                if (!arguments["frames"].is_number_integer())
                    return AutomationResult::Error("Expected 'frames' (integer, 1..60).");
                frames = arguments["frames"].get<int>();
            }
            // The schema says 1..60 and the registry enforces it; a caller that
            // reaches the handler directly gets the same refusal.
            if (frames < 1 || frames > 60)
                return AutomationResult::Error("'frames' must be between 1 and 60.");

            const Json result = host.MarshalRead([&host, frames]() -> Json
                                                 { return EditorActions::ToJson(host.Context().StepScene(frames)); });
            if (!result.value("ok", false))
                return AutomationResult::Error(result.value("message", "Could not step the scene."));
            return AutomationResult::Structured(result);
        }

        AutomationResult HandleEditorGizmoSet(IAutomationHost& host, const Json& arguments)
        {
            if (!host.Context().SetGizmoMode)
                return AutomationResult::Error("Gizmo control is not available in this host (no editor).");
            if (!arguments.contains("mode") || !arguments["mode"].is_string())
                return AutomationResult::Error("Expected 'mode' (one of none, translate, rotate, scale).");
            const std::string mode = arguments["mode"].get<std::string>();
            if (!EditorActions::IsGizmoMode(mode))
                return AutomationResult::Error("Unknown gizmo mode '" + mode + "'; expected none, translate, rotate or scale.");

            const Json result = host.MarshalRead([&host, mode]() -> Json
                                                 { return EditorActions::ToJson(host.Context().SetGizmoMode(mode)); });
            if (!result.value("ok", false))
                return AutomationResult::Error(result.value("message", "Could not change the gizmo mode."));
            return AutomationResult::Structured(result);
        }

        AutomationResult HandleEditorBuildShaderPack(IAutomationHost& host, const Json&)
        {
            if (!host.Context().BuildShaderPack)
                return AutomationResult::Error("Building the shader pack is not available in this host (no editor).");

            const Json result = host.MarshalRead([&host]() -> Json
                                                 { return EditorActions::ToJson(host.Context().BuildShaderPack()); });
            if (!result.value("ok", false))
                return AutomationResult::Error(result.value("message", "Shader pack build failed."));
            return AutomationResult::Structured(result);
        }
    } // namespace

    void RegisterEditorCommands(AutomationRegistry& registry)
    {
        {
            AutomationCommand command;
            command.Name = "olo_editor_actions";
            command.Toolset = "editor";
            command.Title = "List editor actions";
            command.Description =
                "The editor command registry: every menu, toolbar and shortcut action the editor declares, each with "
                "the registry command that performs it -- plus that command's availability on this host, its "
                "authority class (projectWrite) and how it is taken back (undo) -- or a note saying why no command "
                "does. automatedOnly:true lists only the actions that have a command.";
            command.InputSchema = EditorActions::ActionsInputSchema();
            command.OutputSchema = EditorActions::ActionsOutputSchema();
            command.Annotations = ReadOnlyAnnotations();
            command.ProjectWrite = false;
            command.MainMarshaled = false;
            command.Undo = AutomationUndo::None;
            // Captures the registry it is registered into, like
            // olo_transaction_apply: the listing must describe THIS registry's
            // commands, not a fixed idea of them.
            command.Handler = [&registry](IAutomationHost& host, const Json& arguments) -> AutomationResult
            { return HandleEditorActions(registry, host, arguments); };
            registry.Register(std::move(command));
        }
        {
            AutomationCommand command;
            command.Name = "olo_editor_pause";
            command.Toolset = "editor";
            command.Title = "Pause or resume the scene";
            command.Description =
                "Pause (paused:true) or resume (paused:false) the running Play or Simulate session -- the toolbar "
                "Pause/Resume button. Idempotent: changed:false when already in the requested state. An error in "
                "Edit mode, where there is nothing to pause; enter Play or Simulate first.";
            command.InputSchema = EditorActions::PauseInputSchema();
            command.OutputSchema = EditorActions::PauseOutputSchema();
            command.Annotations = MutatingAnnotations(/*idempotent*/ true);
            // Ephemeral runtime control of the editor session, not project data:
            // nothing here is persisted, so it is not a consented write. The
            // same class as olo_viewport_set_size, not olo_scene_play.
            command.ProjectWrite = false;
            command.MainMarshaled = true;
            command.Undo = AutomationUndo::None;
            command.IsAvailable = [](const IAutomationHost& host)
            { return static_cast<bool>(host.Context().SetScenePauseState); };
            command.Handler = HandleEditorPause;
            registry.Register(std::move(command));
        }
        {
            AutomationCommand command;
            command.Name = "olo_editor_step";
            command.Toolset = "editor";
            command.Title = "Step the paused scene";
            command.Description =
                "Advance a PAUSED Play or Simulate session by `frames` frames (1..60, default 1) -- the toolbar Step "
                "button. An error when the session is not paused or the editor is in Edit mode.";
            command.InputSchema = EditorActions::StepInputSchema();
            command.OutputSchema = EditorActions::StepOutputSchema();
            // Not idempotent: every call advances the simulation further.
            command.Annotations = MutatingAnnotations(/*idempotent*/ false);
            // Ephemeral runtime control, like olo_editor_pause: not a consented write.
            command.ProjectWrite = false;
            command.MainMarshaled = true;
            command.Undo = AutomationUndo::None;
            command.IsAvailable = [](const IAutomationHost& host)
            { return static_cast<bool>(host.Context().StepScene); };
            command.Handler = HandleEditorStep;
            registry.Register(std::move(command));
        }
        {
            AutomationCommand command;
            command.Name = "olo_editor_gizmo_set";
            command.Toolset = "editor";
            command.Title = "Set the viewport gizmo";
            command.Description =
                "Select the viewport gizmo shown on the selected entity: none, translate, rotate or scale (the Q/W/E/R "
                "shortcuts in order). Session UI state, not project data; changed:false when already in that mode.";
            command.InputSchema = EditorActions::GizmoInputSchema();
            command.OutputSchema = EditorActions::GizmoOutputSchema();
            command.Annotations = MutatingAnnotations(/*idempotent*/ true);
            // Editor UI state only: not a consented write.
            command.ProjectWrite = false;
            command.MainMarshaled = true;
            command.Undo = AutomationUndo::None;
            command.IsAvailable = [](const IAutomationHost& host)
            { return static_cast<bool>(host.Context().SetGizmoMode); };
            command.Handler = HandleEditorGizmoSet;
            registry.Register(std::move(command));
        }
        {
            AutomationCommand command;
            command.Name = "olo_editor_build_shader_pack";
            command.Toolset = "editor";
            command.Title = "Build the shader pack";
            command.Description =
                "Build > Build Shader Pack: write assets/ShaderPack.osp from the live 2D and 3D shader libraries, "
                "the same code path as the menu item. Synchronous; overwrites the previous pack; not undoable.";
            command.InputSchema = EditorActions::ShaderPackInputSchema();
            command.OutputSchema = EditorActions::ShaderPackOutputSchema();
            command.Annotations = DestructiveIdempotentAnnotations();
            // Writes a file into the project: a consented write, and there is no
            // way to take it back from inside this process.
            command.ProjectWrite = true;
            command.MainMarshaled = true;
            command.Undo = AutomationUndo::Irreversible;
            command.IsAvailable = [](const IAutomationHost& host)
            { return static_cast<bool>(host.Context().BuildShaderPack); };
            command.Handler = HandleEditorBuildShaderPack;
            registry.Register(std::move(command));
        }
    }
} // namespace OloEngine::Automation
