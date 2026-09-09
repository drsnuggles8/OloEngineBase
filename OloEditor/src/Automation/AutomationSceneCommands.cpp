#include "OloEnginePCH.h"
#include "Automation/AutomationSceneCommands.h"

#include "Automation/AutomationRegistry.h"
#include "Automation/AutomationSceneDocument.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "OloEngine/Scene/Components.h"
#include "UndoRedo/EditorCommand.h"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
        namespace Schema = MCP::Schema;

        Json DescribeDocument(const SceneDocumentAccess& access, CommandHistory* history, bool changed)
        {
            if (!access.Capture)
                throw std::runtime_error("Scene document lifecycle is unavailable on this host.");
            const auto document = access.Capture();
            return Json{ { "sceneName", document.Name }, { "path", document.Path.string() }, { "entityCount", document.SceneRef->GetAllEntitiesWith<IDComponent>().size() }, { "dirty", access.IsDirty ? access.IsDirty() : history->IsDirty() }, { "editMode", history != nullptr }, { "canUndo", history && history->CanUndo() }, { "canRedo", history && history->CanRedo() }, { "changed", changed } };
        }

        Json DocumentSchema()
        {
            return Schema::Object()
                .Prop("sceneName", Schema::String())
                .Prop("path", Schema::String().Desc("Absolute save destination, or empty for an untitled scene."))
                .Prop("entityCount", Schema::Int().Min(0))
                .Prop("dirty", Schema::Bool())
                .Prop("editMode", Schema::Bool())
                .Prop("canUndo", Schema::Bool())
                .Prop("canRedo", Schema::Bool())
                .Prop("changed", Schema::Bool().Desc("Whether this invocation applied an undoable operation."))
                .Required({ "sceneName", "path", "entityCount", "dirty", "editMode", "canUndo", "canRedo", "changed" });
        }

        using DocumentOperation = std::function<bool(const SceneDocumentAccess&, CommandHistory&, const Json&)>;

        AutomationHandler MakeHandler(DocumentOperation operation = {})
        {
            return [operation = std::move(operation)](IAutomationHost& host, const Json& arguments)
            {
                const auto access = host.Context().SceneDocument;
                const auto getHistory = host.Context().GetCommandHistory;
                return AutomationResult::Structured(host.MarshalRead([access, getHistory, operation, arguments]()
                                                                     {
                    auto* history = getHistory ? getHistory() : nullptr;
                    if (!access.Capture || (!access.IsDirty && !history))
                        throw std::runtime_error("Scene document status is unavailable on this host.");
                    if (!access.Capture().SceneRef)
                        throw std::runtime_error("No editor scene is available.");
                    if (operation && !history)
                        throw std::runtime_error("Scene authoring requires Edit mode and an editor command history.");
                    const bool changed = operation && operation(access, *history, arguments);
                    return DescribeDocument(access, history, changed); }));
            };
        }

        void Register(AutomationRegistry& registry, std::string name, std::string description,
                      Json input, DocumentOperation operation = {})
        {
            const bool write = static_cast<bool>(operation);
            AutomationCommand command;
            command.Name = std::move(name);
            command.Description = std::move(description);
            command.Toolset = "scene";
            command.InputSchema = std::move(input);
            command.OutputSchema = DocumentSchema();
            command.MainMarshaled = true;
            command.ProjectWrite = write;
            command.Undo = write ? AutomationUndo::EditorUndoStack : AutomationUndo::None;
            command.Annotations = Json{ { "readOnlyHint", !write }, { "destructiveHint", write }, { "idempotentHint", !write }, { "openWorldHint", false } };
            command.Handler = MakeHandler(std::move(operation));
            registry.Register(std::move(command));
        }
    } // namespace

    void RegisterSceneLifecycleCommands(AutomationRegistry& registry)
    {
        Register(registry, "olo_scene_status", "Read the authored scene name, save path, dirty state and undo availability.",
                 Schema::Object().NoAdditional());
        Register(registry, "olo_scene_new", "Create an empty authored scene. One undo restores the previous scene, path and selection.",
                 Schema::Object().Prop("name", Schema::String().Desc("Optional scene name; defaults to Untitled.")).NoAdditional(),
                 [](const SceneDocumentAccess& access, CommandHistory& history, const Json& arguments)
                 {
                     NewSceneDocument(access, history, arguments.value("name", std::string{}));
                     return true;
                 });
        Register(registry, "olo_scene_save", "Save to the current scene path. Undo restores prior file bytes and dirty state; an untitled scene requires save_as.",
                 Schema::Object().NoAdditional(),
                 [](const SceneDocumentAccess& access, CommandHistory& history, const Json&)
                 { return SaveSceneDocument(access, history); });
        Register(registry, "olo_scene_save_as", "Save to an explicit .olo or .scene path, relative to project assets or absolute. Undo restores the destination's prior bytes and document identity.",
                 Schema::Object().Prop("path", Schema::String()).Required({ "path" }).NoAdditional(),
                 [](const SceneDocumentAccess& access, CommandHistory& history, const Json& arguments)
                 {
                     const auto path = arguments.at("path").get<std::string>();
                     if (path.empty())
                         throw std::runtime_error("save_as requires a nonempty path.");
                     return SaveSceneDocument(access, history, std::filesystem::path(path));
                 });
        Register(registry, "olo_editor_undo", "Undo one editor operation. A scene save refuses to overwrite an externally modified file.",
                 Schema::Object().NoAdditional(),
                 [](const SceneDocumentAccess&, CommandHistory& history, const Json&)
                 {
                     const bool changed = history.CanUndo();
                     history.Undo();
                     return changed;
                 });
        Register(registry, "olo_editor_redo", "Redo one editor operation. Saved scenes reuse the exact bytes originally saved.",
                 Schema::Object().NoAdditional(),
                 [](const SceneDocumentAccess&, CommandHistory& history, const Json&)
                 {
                     const bool changed = history.CanRedo();
                     history.Redo();
                     return changed;
                 });
    }
} // namespace OloEngine::Automation
