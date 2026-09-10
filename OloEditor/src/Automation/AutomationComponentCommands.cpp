#include "OloEnginePCH.h"
#include "Automation/AutomationComponentRegistry.h"
#include "Automation/AutomationRegistry.h"
#include "MCP/McpGenericFieldWrite.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "OloEngine/Scene/Entity.h"
#include "UndoRedo/EditorCommand.h"

#include <charconv>
#include <string>
#include <utility>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
        namespace Schema = MCP::Schema;

        Json TypeDescription(const ComponentTypeEntry& entry)
        {
            return Json{ { "component", entry.Name }, { "authored", entry.Authored }, { "addable", entry.Addable }, { "removable", entry.Removable }, { "rejectionReason", entry.RejectionReason } };
        }

        bool ParseEntityUUID(const std::string& value, u64& uuid)
        {
            if (value.empty())
            {
                return false;
            }
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), uuid);
            return error == std::errc{} && end == value.data() + value.size() && uuid != 0;
        }

        AutomationResult Finish(const Json& result)
        {
            return result.contains("__error")
                       ? AutomationResult::Error(result.at("__error").get<std::string>())
                       : AutomationResult::Structured(result);
        }

        AutomationResult HandleListTypes(IAutomationHost& host, const Json&)
        {
            return Finish(host.MarshalRead([]() -> Json
                                           {
                Json types = Json::array();
                for (const auto& entry : ComponentTypes())
                {
                    types.push_back(TypeDescription(entry));
                }
                return Json{ { "components", std::move(types) }, { "changed", false }, { "undoable", false } }; }));
        }

        enum class Operation
        {
            Get,
            Add,
            Remove,
        };

        AutomationResult HandleComponent(IAutomationHost& host, const Json& args, Operation operation)
        {
            const auto entityText = args.at("entity").get<std::string>();
            const auto name = args.at("component").get<std::string>();
            u64 uuid = 0;
            if (!ParseEntityUUID(entityText, uuid))
            {
                return AutomationResult::Error("entity must be a nonzero decimal UUID string within the unsigned 64-bit range.");
            }

            return Finish(host.MarshalRead([&host, uuid, name, operation]() -> Json
                                           {
                const auto& context = host.Context();
                if (!context.GetActiveScene)
                {
                    return Json{ { "__error", "No active editor scene is available." } };
                }
                const auto scene = context.GetActiveScene();
                if (!scene)
                {
                    return Json{ { "__error", "No active editor scene is available." } };
                }
                CommandHistory* history = nullptr;
                if (operation != Operation::Get)
                {
                    history = context.GetCommandHistory ? context.GetCommandHistory() : nullptr;
                    if (!history)
                    {
                        return Json{ { "__error", "Structural authoring requires Edit mode and an editor undo history. Stop Play or Simulate first." } };
                    }
                }
                const auto entity = scene->TryGetEntityWithUUID(UUID(uuid));
                if (!entity)
                {
                    return Json{ { "__error", "No entity with that UUID exists in the active scene." } };
                }
                const auto* entry = FindComponentType(name);
                if (!entry)
                {
                    return Json{ { "__error", "Unknown component type. Use olo_component_list_types for canonical names." } };
                }

                const bool present = entry->Has(*entity);
                Json result = TypeDescription(*entry);
                result["entity"] = std::to_string(uuid);
                result["present"] = present;
                result["changed"] = false;
                result["undoable"] = false;
                if (operation == Operation::Get)
                {
                    result["fields"] = Json::array();
                    if (present)
                    {
                        bool found = false;
                        const auto fields = MCP::GenericFieldWrite::ListFields(scene, uuid, name, found);
                        if (found && !fields.at("components").empty())
                        {
                            result["fields"] = fields.at("components").front().at("fields");
                        }
                        const auto refusal = entry->ValidateSnapshot(*entity);
                        if (!refusal.empty())
                        {
                            result["removable"] = false;
                            result["rejectionReason"] = refusal;
                        }
                    }
                    return result;
                }
                const bool add = operation == Operation::Add;
                if ((add && !entry->Addable) || (!add && !entry->Removable))
                {
                    return Json{ { "__error", entry->RejectionReason } };
                }
                if (add == present)
                {
                    return result; // Idempotent request; no undo entry or dirty change.
                }
                if (!add)
                {
                    if (const auto refusal = entry->ValidateSnapshot(*entity); !refusal.empty())
                    {
                        return Json{ { "__error", refusal } };
                    }
                }
                history->Execute(add ? entry->MakeAddCommand(scene, UUID(uuid))
                                     : entry->MakeRemoveCommand(scene, UUID(uuid)));
                result["present"] = entry->Has(*entity);
                result["changed"] = true;
                result["undoable"] = true;
                return result; }));
        }

        Schema::Node TypeSchema()
        {
            return Schema::Object()
                .Prop("component", Schema::String())
                .Prop("authored", Schema::Bool())
                .Prop("addable", Schema::Bool())
                .Prop("removable", Schema::Bool())
                .Prop("rejectionReason", Schema::String())
                .Required({ "component", "authored", "addable", "removable", "rejectionReason" });
        }

        AutomationCommand ComponentCommand(const char* name, const char* description, Operation operation)
        {
            const bool write = operation != Operation::Get;
            AutomationCommand command;
            command.Name = name;
            command.Description = description;
            command.Toolset = "scene";
            command.InputSchema = Schema::Object()
                                      .Prop("entity", Schema::String().Desc("Stable entity UUID as a nonzero decimal string."))
                                      .Prop("component", Schema::String().Desc("Canonical component type from olo_component_list_types."))
                                      .Required({ "entity", "component" })
                                      .NoAdditional();
            command.OutputSchema = TypeSchema()
                                       .Prop("entity", Schema::String())
                                       .Prop("present", Schema::Bool())
                                       .Prop("changed", Schema::Bool())
                                       .Prop("undoable", Schema::Bool())
                                       .Prop("fields", Schema::Array(Schema::Object()))
                                       .Required({ "entity", "component", "present", "changed", "undoable" });
            command.Annotations = Json{ { "readOnlyHint", !write }, { "destructiveHint", operation == Operation::Remove }, { "idempotentHint", true }, { "openWorldHint", false } };
            command.MainMarshaled = true;
            command.ProjectWrite = write;
            command.Undo = write ? AutomationUndo::EditorUndoStack : AutomationUndo::None;
            command.Handler = [operation](IAutomationHost& host, const Json& args)
            { return HandleComponent(host, args, operation); };
            return command;
        }
    } // namespace

    void RegisterComponentAuthoringCommands(AutomationRegistry& registry)
    {
        AutomationCommand list;
        list.Name = "olo_component_list_types";
        list.Description = "List every generated component type, including identity/runtime types, and whether direct structural authoring is supported. Discovery does not construct components.";
        list.Toolset = "scene";
        list.InputSchema = Schema::Object().NoAdditional();
        list.OutputSchema = Schema::Object()
                                .Prop("components", Schema::Array(TypeSchema()))
                                .Prop("changed", Schema::Bool())
                                .Prop("undoable", Schema::Bool())
                                .Required({ "components", "changed", "undoable" });
        list.Annotations = Json{ { "readOnlyHint", true }, { "destructiveHint", false }, { "idempotentHint", true }, { "openWorldHint", false } };
        list.MainMarshaled = true;
        list.Undo = AutomationUndo::None;
        list.Handler = HandleListTypes;
        registry.Register(std::move(list));
        registry.Register(ComponentCommand("olo_component_get", "Query component presence and reflected fields by canonical type and stable entity UUID.", Operation::Get));
        registry.Register(ComponentCommand("olo_component_add", "Add a default component in Edit mode as one undo step. Existing components are unchanged; redo restores the original defaults.", Operation::Add));
        registry.Register(ComponentCommand("olo_component_remove", "Remove a component in Edit mode as one undo step, preserving its authored data. Refuses protected types and state that cannot be restored exactly.", Operation::Remove));
    }
} // namespace OloEngine::Automation
