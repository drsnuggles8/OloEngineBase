#include "OloEnginePCH.h"
#include "Automation/AutomationEntityCommands.h"
#include "Automation/AutomationComponentRegistry.h"
#include "Automation/AutomationRegistry.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "UndoRedo/EditorCommand.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
        namespace Schema = MCP::Schema;

        Json Error(std::string message)
        {
            return Json{ { "__error", std::move(message) } };
        }

        std::string Identity(UUID uuid)
        {
            return std::to_string(static_cast<u64>(uuid));
        }

        std::optional<UUID> ParseIdentity(const Json& args, std::string_view key, bool allowZero)
        {
            const auto it = args.find(std::string(key));
            if (it == args.end())
                return allowZero ? std::optional<UUID>(UUID(0)) : std::nullopt;
            if (!it->is_string())
                return std::nullopt;
            const auto& value = it->get_ref<const std::string&>();
            if (value.empty() || !std::ranges::all_of(value, [](char ch)
                                                      { return ch >= '0' && ch <= '9'; }))
                return std::nullopt;
            u64 parsed = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size() || (!allowZero && parsed == 0))
                return std::nullopt;
            return UUID(parsed);
        }

        struct RelationshipState
        {
            UUID EntityId{ 0 };
            std::optional<RelationshipComponent> Data;
        };

        // SetParent changes both ends of a relationship and may create components.
        // Keep every affected endpoint, including absence and the ordered children,
        // so undo never appends a formerly middle sibling or leaves a new component.
        class HierarchyState
        {
          public:
            void Capture(const Ref<Scene>& scene, UUID id)
            {
                if (static_cast<u64>(id) == 0 || std::ranges::any_of(m_States, [id](const auto& state)
                                                                     { return state.EntityId == id; }))
                    return;
                RelationshipState state{ id, std::nullopt };
                if (auto entity = scene->TryGetEntityWithUUID(id); entity && entity->HasComponent<RelationshipComponent>())
                    state.Data = entity->GetComponent<RelationshipComponent>();
                m_States.push_back(std::move(state));
            }

            [[nodiscard]] HierarchyState Recapture(const Ref<Scene>& scene) const
            {
                HierarchyState result;
                for (const auto& state : m_States)
                    result.Capture(scene, state.EntityId);
                return result;
            }

            void Restore(const Ref<Scene>& scene) const
            {
                // All endpoints are restored in this single main-thread command;
                // there is no frame where an observer sees half the relationship.
                for (const auto& state : m_States)
                {
                    auto entity = scene->TryGetEntityWithUUID(state.EntityId);
                    if (!entity)
                        continue;
                    if (state.Data)
                        entity->AddOrReplaceComponent<RelationshipComponent>(*state.Data);
                    else if (entity->HasComponent<RelationshipComponent>())
                        entity->RemoveComponent<RelationshipComponent>();
                }
            }

          private:
            std::vector<RelationshipState> m_States;
        };

        struct EntityState
        {
            UUID Id{ 0 };
            std::string Name;
            ComponentSnapshots Components;

            explicit EntityState(Entity entity)
                : Id(entity.GetUUID()), Name(entity.GetName()), Components(CaptureEntityComponents(entity))
            {
            }

            void Restore(Ref<Scene> scene) const
            {
                Entity entity = scene->CreateEntityWithUUID(Id, Name);
                // CreateEntity normalizes an empty name; an existing authored tag
                // can nevertheless be empty, and undo must preserve that value.
                if (auto& tag = entity.GetComponent<TagComponent>().Tag; tag != Name)
                {
                    const std::string createdName = tag;
                    tag = Name;
                    scene->UpdateEntityName(static_cast<entt::entity>(entity), createdName, tag);
                }
                for (const auto& snapshot : Components)
                    snapshot->Restore(entity);
            }
        };

        void DestroyOne(Ref<Scene> scene, UUID id, const std::function<void()>& clearSelection)
        {
            auto entity = scene->TryGetEntityWithUUID(id);
            if (!entity)
                return;
            if (clearSelection)
                clearSelection();
            if (entity->GetParent())
                entity->SetParent({});
            const auto children = entity->Children();
            for (UUID childId : children)
            {
                if (auto child = scene->TryGetEntityWithUUID(childId))
                    child->SetParent({});
            }
            scene->DestroyEntity(*entity);
        }

        class EntityPresenceCommand final : public EditorCommand
        {
          public:
            EntityPresenceCommand(Ref<Scene> scene, EntityState entity, HierarchyState before, HierarchyState after,
                                  bool presentAfter, std::string description, std::function<void()> clearSelection)
                : m_Scene(std::move(scene)), m_Entity(std::move(entity)), m_Before(std::move(before)), m_After(std::move(after)),
                  m_PresentAfter(presentAfter), m_Description(std::move(description)), m_ClearSelection(std::move(clearSelection))
            {
            }

            void Execute() override
            {
                Apply(m_PresentAfter, m_After);
            }

            void Undo() override
            {
                Apply(!m_PresentAfter, m_Before);
            }

            [[nodiscard]] std::string GetDescription() const override
            {
                return m_Description;
            }

          private:
            void Apply(bool present, const HierarchyState& hierarchy)
            {
                if (present)
                    m_Entity.Restore(m_Scene);
                else
                    DestroyOne(m_Scene, m_Entity.Id, m_ClearSelection);
                hierarchy.Restore(m_Scene);
            }

            Ref<Scene> m_Scene;
            EntityState m_Entity;
            HierarchyState m_Before;
            HierarchyState m_After;
            bool m_PresentAfter;
            std::string m_Description;
            std::function<void()> m_ClearSelection;
        };

        class HierarchyChangeCommand final : public EditorCommand
        {
          public:
            HierarchyChangeCommand(Ref<Scene> scene, HierarchyState before, HierarchyState after)
                : m_Scene(std::move(scene)), m_Before(std::move(before)), m_After(std::move(after))
            {
            }

            void Execute() override
            {
                m_After.Restore(m_Scene);
            }

            void Undo() override
            {
                m_Before.Restore(m_Scene);
            }

            [[nodiscard]] std::string GetDescription() const override
            {
                return "Reparent Entity";
            }

          private:
            Ref<Scene> m_Scene;
            HierarchyState m_Before;
            HierarchyState m_After;
        };

        // siblingIndex names the final insertion position after removing this child.
        // Roots have no ordered child list, so an index without a parent is invalid.
        std::string ValidateParent(const Ref<Scene>& scene, const Json& args, Entity child,
                                   Entity& parent, std::optional<sizet>& index)
        {
            auto parentId = ParseIdentity(args, "parent", true);
            if (!parentId)
                return "parent must be a decimal UUID string ('0' detaches the entity).";
            if (static_cast<u64>(*parentId) != 0)
            {
                const auto found = scene->TryGetEntityWithUUID(*parentId);
                if (!found)
                    return "Parent entity does not exist: " + Identity(*parentId);
                parent = *found;
                if (child && (child == parent || child.WouldCreateCycleWith(parent)))
                    return "Parent would create an entity hierarchy cycle.";
            }
            if (!args.contains("siblingIndex"))
                return {};
            if (!parent)
                return "siblingIndex requires a nonzero parent.";
            const auto& value = args.at("siblingIndex");
            if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<i64>() < 0))
                return "siblingIndex must be a nonnegative integer.";
            const auto requested = value.get<u64>();
            sizet remaining = parent.Children().size();
            if (child && std::ranges::find(parent.Children(), child.GetUUID()) != parent.Children().end())
                --remaining;
            if (requested > remaining)
                return "siblingIndex is beyond the parent's remaining children.";
            index = static_cast<sizet>(requested);
            return {};
        }

        void ApplyParent(Entity child, Entity parent, std::optional<sizet> index)
        {
            child.SetParent(parent);
            if (index)
            {
                auto& children = parent.GetOrCreateChildren();
                const UUID id = child.GetUUID();
                std::erase(children, id);
                children.insert(children.begin() + static_cast<std::ptrdiff_t>(*index), id);
            }
        }

        Json EntityResult(Entity entity, bool changed)
        {
            return Json{ { "entity", Identity(entity.GetUUID()) }, { "name", entity.GetName() }, { "parent", Identity(entity.GetParentUUID()) }, { "changed", changed }, { "undoable", changed } };
        }

        Json CreateEntity(Ref<Scene> scene, CommandHistory& history, const Json& args,
                          const std::function<void()>& clearSelection)
        {
            Entity parent;
            std::optional<sizet> index;
            if (auto error = ValidateParent(scene, args, {}, parent, index); !error.empty())
                return Error(std::move(error));
            HierarchyState before;
            if (parent)
                before.Capture(scene, parent.GetUUID());
            Entity entity = scene->CreateEntity(args.value("name", "Entity"));
            if (parent)
                ApplyParent(entity, parent, index);
            HierarchyState after = before.Recapture(scene);
            after.Capture(scene, entity.GetUUID());
            history.PushAlreadyExecuted(std::make_unique<EntityPresenceCommand>(
                scene, EntityState(entity), std::move(before), std::move(after), true, "Create Entity", clearSelection));
            return EntityResult(entity, true);
        }

        Json DestroyEntity(Ref<Scene> scene, CommandHistory& history, const Json& args,
                           const std::function<void()>& clearSelection)
        {
            auto id = ParseIdentity(args, "entity", false);
            if (!id)
                return Error("entity must be a nonzero decimal UUID string.");
            auto entity = scene->TryGetEntityWithUUID(*id);
            if (!entity)
                return Error("Entity does not exist: " + Identity(*id));
            if (auto error = ValidateEntitySnapshot(*entity); !error.empty())
                return Error(std::move(error));
            EntityState snapshot(*entity);
            HierarchyState before;
            before.Capture(scene, *id);
            before.Capture(scene, entity->GetParentUUID());
            for (UUID childId : entity->Children())
                before.Capture(scene, childId);
            DestroyOne(scene, *id, clearSelection);
            HierarchyState after = before.Recapture(scene);
            history.PushAlreadyExecuted(std::make_unique<EntityPresenceCommand>(
                scene, std::move(snapshot), std::move(before), std::move(after), false, "Destroy Entity", clearSelection));
            return Json{ { "entity", Identity(*id) }, { "changed", true }, { "undoable", true } };
        }

        Json DuplicateEntity(Ref<Scene> scene, CommandHistory& history, const Json& args,
                             const std::function<void()>& clearSelection)
        {
            auto id = ParseIdentity(args, "entity", false);
            if (!id)
                return Error("entity must be a nonzero decimal UUID string.");
            auto source = scene->TryGetEntityWithUUID(*id);
            if (!source)
                return Error("Entity does not exist: " + Identity(*id));
            Entity parent;
            std::optional<sizet> index;
            if (auto error = ValidateParent(scene, args, {}, parent, index); !error.empty())
                return Error(std::move(error));
            if (auto error = ValidateEntityDuplication(*source); !error.empty())
                return Error(std::move(error));
            HierarchyState before;
            if (parent)
                before.Capture(scene, parent.GetUUID());
            Entity entity = scene->DuplicateEntity(*source);
            if (args.contains("name"))
            {
                auto& tag = entity.GetComponent<TagComponent>().Tag;
                const std::string oldName = tag;
                tag = args.at("name").get<std::string>();
                scene->UpdateEntityName(static_cast<entt::entity>(entity), oldName, tag);
            }
            if (parent)
                ApplyParent(entity, parent, index);
            HierarchyState after = before.Recapture(scene);
            after.Capture(scene, entity.GetUUID());
            history.PushAlreadyExecuted(std::make_unique<EntityPresenceCommand>(
                scene, EntityState(entity), std::move(before), std::move(after), true, "Duplicate Entity", clearSelection));
            Json result = EntityResult(entity, true);
            result["source"] = Identity(*id);
            return result;
        }

        Json ReparentEntity(Ref<Scene> scene, CommandHistory& history, const Json& args,
                            const std::function<void()>& /*clearSelection*/)
        {
            auto id = ParseIdentity(args, "entity", false);
            if (!id)
                return Error("entity must be a nonzero decimal UUID string.");
            auto child = scene->TryGetEntityWithUUID(*id);
            if (!child)
                return Error("Entity does not exist: " + Identity(*id));
            Entity parent;
            std::optional<sizet> index;
            if (auto error = ValidateParent(scene, args, *child, parent, index); !error.empty())
                return Error(std::move(error));
            const UUID newParent = parent ? parent.GetUUID() : UUID(0);
            if (child->GetParentUUID() == newParent)
            {
                if (!index)
                    return EntityResult(*child, false);
                const auto current = std::ranges::find(parent.Children(), *id);
                if (current != parent.Children().end() && static_cast<sizet>(current - parent.Children().begin()) == *index)
                    return EntityResult(*child, false);
            }
            HierarchyState before;
            before.Capture(scene, *id);
            before.Capture(scene, child->GetParentUUID());
            before.Capture(scene, newParent);
            ApplyParent(*child, parent, index);
            HierarchyState after = before.Recapture(scene);
            history.PushAlreadyExecuted(std::make_unique<HierarchyChangeCommand>(scene, std::move(before), std::move(after)));
            return EntityResult(*child, true);
        }

        using EntityMutation = Json (*)(Ref<Scene>, CommandHistory&, const Json&, const std::function<void()>&);

        AutomationResult RunMutation(IAutomationHost& host, const Json& args, EntityMutation mutation)
        {
            // Value captures survive a queued job even when marshalling times out.
            const auto getScene = host.Context().GetActiveScene;
            const auto getHistory = host.Context().GetCommandHistory;
            const auto select = host.Context().SelectEntityInEditor;
            const auto invalidate = host.Context().InvalidateEntityReferences;
            Json result = host.MarshalRead([args, mutation, getScene, getHistory, select, invalidate]() -> Json
                                           {
                if (!getScene || !getHistory)
                    return Error("Scene authoring requires an active editor scene and command history.");
                Ref<Scene> scene = getScene();
                CommandHistory* history = getHistory();
                if (!scene || !history)
                    return Error("Scene authoring is available only in Edit mode with an active scene.");
                const std::function<void()> clearSelection = [select, invalidate]()
                {
                    if (invalidate)
                        invalidate();
                    else if (select)
                        select(0, true);
                };
                return mutation(scene, *history, args, clearSelection); });
            if (result.contains("__error"))
                return AutomationResult::Error(result.at("__error").get<std::string>());
            return AutomationResult::Structured(result);
        }

        void RegisterEntityCommand(AutomationRegistry& registry, std::string name, std::string title,
                                   std::string description, const Schema::Node& input, EntityMutation mutation)
        {
            AutomationCommand command;
            command.Name = std::move(name);
            command.Title = std::move(title);
            command.Description = std::move(description);
            command.Toolset = "scene";
            command.InputSchema = input;
            command.OutputSchema = Schema::Object()
                                       .Prop("entity", Schema::String().Desc("Stable decimal entity UUID, retained across undo and redo."))
                                       .Prop("parent", Schema::String().Desc("Parent UUID, or '0' for a root entity."))
                                       .Prop("name", Schema::String())
                                       .Prop("source", Schema::String().Desc("The source entity UUID for duplication."))
                                       .Prop("changed", Schema::Bool())
                                       .Prop("undoable", Schema::Bool())
                                       .Required({ "entity", "changed", "undoable" });
            command.Annotations = Json{ { "readOnlyHint", false }, { "destructiveHint", true }, { "idempotentHint", false }, { "openWorldHint", false } };
            command.ProjectWrite = true;
            command.MainMarshaled = true;
            command.Undo = AutomationUndo::EditorUndoStack;
            command.Handler = [mutation](IAutomationHost& host, const Json& args)
            { return RunMutation(host, args, mutation); };
            registry.Register(std::move(command));
        }
    } // namespace

    void RegisterEntityAuthoringCommands(AutomationRegistry& registry)
    {
        const auto entity = Schema::String().Desc("Nonzero decimal entity UUID string.");
        const auto parent = Schema::String().Desc("Parent UUID string; omit or use '0' for no parent.");
        const auto index = Schema::Int().Min(0).Desc("Final zero-based sibling position, after removing the moved child. Requires a parent; omitted appends on parent change.");
        RegisterEntityCommand(registry, "olo_entity_create", "Create entity",
                              "Create one entity, optionally parented at siblingIndex. Returns a stable UUID. One editor undo step; Edit mode only.",
                              Schema::Object().Prop("name", Schema::String()).Prop("parent", parent).Prop("siblingIndex", index).NoAdditional(), CreateEntity);
        RegisterEntityCommand(registry, "olo_entity_destroy", "Destroy entity",
                              "Destroy one entity and detach its surviving children. Undo restores its UUID, components and exact hierarchy. Edit mode only.",
                              Schema::Object().Prop("entity", entity).Required({ "entity" }).NoAdditional(), DestroyEntity);
        RegisterEntityCommand(registry, "olo_entity_duplicate", "Duplicate entity",
                              "Duplicate one entity without descendants. The new entity is a root unless parent is provided; its camera is not primary. Undo and redo retain the clone UUID. Edit mode only.",
                              Schema::Object().Prop("entity", entity).Prop("name", Schema::String()).Prop("parent", parent).Required({ "entity" }).NoAdditional(), DuplicateEntity);
        RegisterEntityCommand(registry, "olo_entity_reparent", "Reparent entity",
                              "Move an entity under parent, preserving its local transform. Omit parent or use '0' to detach. Optionally reorder with siblingIndex; unchanged requests add no undo entry. Edit mode only.",
                              Schema::Object().Prop("entity", entity).Prop("parent", parent).Prop("siblingIndex", index).Required({ "entity" }).NoAdditional(), ReparentEntity);
    }
} // namespace OloEngine::Automation
