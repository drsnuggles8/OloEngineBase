#include "OloEnginePCH.h"
#include "Automation/AutomationPrefabCommands.h"

#include "Automation/AutomationComponentRegistry.h"
#include "Automation/AutomationFileWrite.h"
#include "Automation/AutomationRegistry.h"
#include "MCP/McpGenericFieldWrite.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "OloEngine/Asset/AssetExtensions.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Prefab.h"
#include "OloEngine/Scene/Scene.h"
#include "UndoRedo/EditorCommand.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine::Automation
{
    namespace
    {
        using Json = nlohmann::json;
        namespace Schema = MCP::Schema;
        namespace Fields = MCP::GenericFieldWrite;

        Json Error(std::string message)
        {
            return Json{ { "__error", std::move(message) } };
        }

        std::string Identity(UUID uuid)
        {
            return std::to_string(static_cast<u64>(uuid));
        }

        std::optional<u64> ParseDecimal(const Json& value)
        {
            if (!value.is_string())
                return std::nullopt;
            const auto& text = value.get_ref<const std::string&>();
            if (text.empty() || !std::ranges::all_of(text, [](char ch)
                                                     { return ch >= '0' && ch <= '9'; }))
                return std::nullopt;
            u64 parsed = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (error != std::errc{} || end != text.data() + text.size())
                return std::nullopt;
            return parsed;
        }

        // ---- the multi-scene memento ----------------------------------------
        //
        // A prefab apply writes into the prefab asset's OWN Scene as well as the
        // active one, so a memento that remembered only a UUID would restore the
        // wrong object (or nothing). Every captured state names its scene.

        struct EntityComponents
        {
            Ref<Scene> OwningScene;
            // Nonzero for an entity inside a PREFAB ASSET's scene. Writing the
            // .oloprefab trips the content watcher, and a prefab reload
            // OBJECT-REPLACES the asset (only Texture2D refreshes in place), so a
            // memento holding the Ref<Scene> it captured would, after the reload,
            // restore into an orphaned scene: the file would come back and the live
            // prefab would not. Re-resolve through the handle instead.
            AssetHandle SourcePrefab{ 0 };
            UUID Id{ 0 };
            std::vector<std::pair<const ComponentTypeEntry*, std::shared_ptr<const ComponentSnapshot>>> Present;
        };

        // Callers run ValidateEntitySnapshot first; capture walks the authored
        // types so the memento also records what is ABSENT.
        EntityComponents CaptureEntity(const Ref<Scene>& scene, Entity entity, AssetHandle sourcePrefab = AssetHandle(0))
        {
            EntityComponents state;
            state.OwningScene = scene;
            state.SourcePrefab = sourcePrefab;
            state.Id = entity.GetUUID();
            for (const auto& type : ComponentTypes())
            {
                if (type.Capture && type.Has(entity))
                {
                    state.Present.emplace_back(&type, type.Capture(entity));
                }
            }
            return state;
        }

        void RestoreEntity(const EntityComponents& state)
        {
            Ref<Scene> scene = state.OwningScene;
            if (static_cast<u64>(state.SourcePrefab) != 0)
            {
                Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(state.SourcePrefab);
                if (!prefab || !prefab->GetScene())
                {
                    throw std::runtime_error("Prefab " + std::to_string(static_cast<u64>(state.SourcePrefab)) +
                                             " is no longer available, so its side of this operation cannot be "
                                             "taken back.");
                }
                scene = prefab->GetScene();
            }
            if (!scene)
            {
                return;
            }
            auto entity = scene->TryGetEntityWithUUID(state.Id);
            if (!entity)
            {
                return;
            }
            // Clear first, restore second: an apply can ADD a component to the
            // source prefab, and undo has to take that addition away rather than
            // only overwriting the components the memento happens to list.
            for (const auto& type : ComponentTypes())
            {
                if (!type.Capture || !type.Remove)
                {
                    continue;
                }
                const bool captured = std::ranges::any_of(state.Present, [&type](const auto& held)
                                                          { return held.first == &type; });
                if (!captured)
                {
                    type.Remove(*entity);
                }
            }
            for (const auto& [type, snapshot] : state.Present)
            {
                snapshot->Restore(*entity);
            }
        }

        void RestoreAll(const std::vector<EntityComponents>& states)
        {
            for (const auto& state : states)
            {
                RestoreEntity(state);
            }
        }

        // One undo entry for a component move that spans scenes: the prefab's
        // source entity, the instance that supplied the override, and every peer
        // instance that re-synced from the source.
        class PrefabStateCommand final : public EditorCommand
        {
          public:
            PrefabStateCommand(std::vector<EntityComponents> before, std::vector<EntityComponents> after,
                               std::string description)
                : m_Before(std::move(before)), m_After(std::move(after)), m_Description(std::move(description))
            {
            }

            void Execute() override
            {
                RestoreAll(m_After);
            }

            void Undo() override
            {
                RestoreAll(m_Before);
            }

            [[nodiscard]] std::string GetDescription() const override
            {
                return m_Description;
            }

          private:
            std::vector<EntityComponents> m_Before;
            std::vector<EntityComponents> m_After;
            std::string m_Description;
        };

        // The .oloprefab half of the same entry. An apply that never reaches the
        // file is lost on the next editor start, so the write belongs to the
        // operation rather than to a later save -- and it is guarded in both
        // directions, so neither one overwrites an edit made in between.
        class PrefabFileCommand final : public EditorCommand
        {
          public:
            PrefabFileCommand(std::filesystem::path path, FileContents before, FileContents after,
                              std::string description)
                : m_Path(std::move(path)), m_Before(std::move(before)), m_After(std::move(after)),
                  m_Description(std::move(description))
            {
            }

            void Execute() override
            {
                ReplaceFileContents(m_Path, m_Before, m_After);
            }

            void Undo() override
            {
                ReplaceFileContents(m_Path, m_After, m_Before);
            }

            [[nodiscard]] std::string GetDescription() const override
            {
                return m_Description;
            }

          private:
            std::filesystem::path m_Path;
            FileContents m_Before;
            FileContents m_After;
            std::string m_Description;
        };

        Ref<EditorAssetManager> EditorManager()
        {
            if (!Project::HasAssetManager())
            {
                return {};
            }
            return Project::GetAssetManager().As<EditorAssetManager>();
        }

        // The registry half of create-from-subtree. RemoveAsset drops the handle,
        // the metadata and the memory asset but never touches the file, which is
        // why the file has its own command and this one can undo without
        // fighting it.
        class PrefabRegistrationCommand final : public EditorCommand
        {
          public:
            PrefabRegistrationCommand(Ref<Prefab> prefab, AssetMetadata metadata)
                : m_Prefab(std::move(prefab)), m_Metadata(std::move(metadata))
            {
            }

            void Execute() override
            {
                auto manager = EditorManager();
                if (!manager)
                {
                    throw std::runtime_error("Cannot register the prefab: the project has no editor asset manager.");
                }
                manager->AddMemoryOnlyAsset(m_Prefab);
                manager->SetMetadata(m_Metadata.Handle, m_Metadata);
                if (!manager->SerializeAssetRegistry())
                {
                    throw std::runtime_error("Failed to write the asset registry.");
                }
            }

            void Undo() override
            {
                auto manager = EditorManager();
                if (!manager)
                {
                    throw std::runtime_error("Cannot deregister the prefab: the project has no editor asset manager.");
                }
                manager->RemoveAsset(m_Metadata.Handle);
                if (!manager->SerializeAssetRegistry())
                {
                    throw std::runtime_error("Failed to write the asset registry.");
                }
            }

            [[nodiscard]] std::string GetDescription() const override
            {
                return "Register Prefab";
            }

          private:
            Ref<Prefab> m_Prefab;
            AssetMetadata m_Metadata;
        };

        // ---- subtree presence (instantiate) ---------------------------------

        struct SubtreeNode
        {
            UUID Id{ 0 };
            std::string Name;
            ComponentSnapshots Components;
        };

        // Instantiate creates a whole hierarchy; undo destroys it and redo brings
        // it back under the SAME UUIDs, so ids a caller stored stay valid across
        // Ctrl-Z/Ctrl-Y exactly as they do for olo_entity_create. The hierarchy
        // needs no separate bookkeeping: every node's RelationshipComponent is
        // captured, and restoring those restores both the parent links and the
        // exact sibling order. `endpoints` carries the one entity OUTSIDE the
        // subtree the operation touched -- the parent the root was attached to.
        class PrefabSubtreeCommand final : public EditorCommand
        {
          public:
            PrefabSubtreeCommand(Ref<Scene> scene, std::vector<SubtreeNode> nodes,
                                 std::vector<EntityComponents> before, std::vector<EntityComponents> after,
                                 bool presentAfter, std::string description, std::function<void()> clearSelection)
                : m_Scene(std::move(scene)), m_Nodes(std::move(nodes)), m_Before(std::move(before)),
                  m_After(std::move(after)), m_PresentAfter(presentAfter), m_Description(std::move(description)),
                  m_ClearSelection(std::move(clearSelection))
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
            void Apply(bool present, const std::vector<EntityComponents>& endpoints)
            {
                if (present)
                {
                    Create();
                }
                else
                {
                    Destroy();
                }
                RestoreAll(endpoints);
            }

            void Create()
            {
                for (const auto& node : m_Nodes)
                {
                    Entity entity = m_Scene->CreateEntityWithUUID(node.Id, node.Name);
                    // CreateEntity normalizes an empty name; a prefab child whose
                    // authored tag is empty must come back empty all the same.
                    if (auto& tag = entity.GetComponent<TagComponent>().Tag; tag != node.Name)
                    {
                        const std::string createdName = tag;
                        tag = node.Name;
                        m_Scene->UpdateEntityName(static_cast<entt::entity>(entity), createdName, tag);
                    }
                    for (const auto& snapshot : node.Components)
                    {
                        snapshot->Restore(entity);
                    }
                }
            }

            void Destroy()
            {
                if (m_ClearSelection)
                {
                    m_ClearSelection();
                }
                // Deepest first, so every node's own children are already gone and
                // the only children left to detach are FOREIGN ones a later edit
                // parented under the instance. Those survive as roots, matching
                // olo_entity_destroy rather than taking somebody else's subtree.
                for (auto node = m_Nodes.rbegin(); node != m_Nodes.rend(); ++node)
                {
                    auto entity = m_Scene->TryGetEntityWithUUID(node->Id);
                    if (!entity)
                    {
                        continue;
                    }
                    if (entity->GetParent())
                    {
                        entity->SetParent({});
                    }
                    const auto children = entity->Children();
                    for (UUID childId : children)
                    {
                        if (auto child = m_Scene->TryGetEntityWithUUID(childId))
                        {
                            child->SetParent({});
                        }
                    }
                    m_Scene->DestroyEntity(*entity);
                }
            }

            Ref<Scene> m_Scene;
            std::vector<SubtreeNode> m_Nodes;
            std::vector<EntityComponents> m_Before;
            std::vector<EntityComponents> m_After;
            bool m_PresentAfter;
            std::string m_Description;
            std::function<void()> m_ClearSelection;
        };

        // ---- what a prefab actually carries ---------------------------------

        // Prefab::CopyableComponentNames resolved against the generated type
        // table, in the engine's own copy order. A name that fails to resolve
        // means the two lists disagree about what a component is called, which
        // would silently drop it from every prefab operation -- so it is logged
        // loudly and McpAutomationPrefabCommandsTest pins the whole set.
        const std::vector<const ComponentTypeEntry*>& PrefabComponentTypes()
        {
            static const std::vector<const ComponentTypeEntry*> s_Types = []
            {
                std::vector<const ComponentTypeEntry*> types;
                for (const auto& name : Prefab::CopyableComponentNames())
                {
                    if (const auto* entry = FindComponentType(name))
                    {
                        types.push_back(entry);
                    }
                    else
                    {
                        OLO_CORE_ERROR("Prefab copyable component '{}' is not a known component type; prefab "
                                       "automation cannot speak for it.",
                                       name);
                    }
                }
                return types;
            }();
            return s_Types;
        }

        const ComponentTypeEntry* FindPrefabComponentType(std::string_view name)
        {
            const auto& types = PrefabComponentTypes();
            const auto found = std::ranges::find_if(types, [name](const ComponentTypeEntry* entry)
                                                    { return entry->Name == name; });
            return found == types.end() ? nullptr : *found;
        }

        // Every registered field of one component, map-keyed entries included, so
        // a caller can COUNT what it could not compare instead of reporting a
        // partial comparison as a clean bill of health.
        std::vector<const Fields::FieldEntry*> RegisteredFields(const std::string& component)
        {
            std::vector<const Fields::FieldEntry*> entries;
            for (const Fields::FieldEntry& entry : Fields::Registry())
            {
                if (entry.Component == component)
                {
                    entries.push_back(&entry);
                }
            }
            return entries;
        }

        // ---- override detection ---------------------------------------------

        struct FieldDiff
        {
            std::string Field;
            Json Instance;
            Json Source;
        };

        struct ComponentDiff
        {
            std::string Component;
            std::string State; // added | removed | modified | identical | unknown
            std::vector<FieldDiff> Fields;
            u32 ComparedFields = 0;
            u32 UncomparedFields = 0;
        };

        // Compare an instance entity against the prefab entity it was stamped
        // from, FIELD BY FIELD. Prefab::DetectOverrides answers a narrower
        // question -- component presence plus whatever the editor happened to
        // mark while a human dragged a slider -- so it cannot answer "what has
        // this instance diverged on" for an instance nobody edited in the editor.
        // This walks the same field registry olo_entity_set_field writes through,
        // so a divergence it reports is a divergence that tool could have caused.
        std::vector<ComponentDiff> DiffEntity(Entity instance, Entity source)
        {
            std::vector<ComponentDiff> diffs;
            for (const ComponentTypeEntry* type : PrefabComponentTypes())
            {
                const bool onInstance = type->Has(instance);
                const bool onSource = type->Has(source);
                if (!onInstance && !onSource)
                {
                    continue;
                }
                ComponentDiff diff;
                diff.Component = type->Name;
                if (onInstance != onSource)
                {
                    diff.State = onInstance ? "added" : "removed";
                    diffs.push_back(std::move(diff));
                    continue;
                }
                const auto fields = RegisteredFields(type->Name);
                for (const Fields::FieldEntry* entry : fields)
                {
                    // A map-keyed field has no compile-time key set, and the two
                    // sides can disagree on the KEYS as well as the values. Count
                    // it rather than pretend the component was fully compared.
                    if (entry->IsMapKeyed)
                    {
                        ++diff.UncomparedFields;
                        continue;
                    }
                    const auto left = entry->Read(instance);
                    const auto right = entry->Read(source);
                    if (!left || !right)
                    {
                        ++diff.UncomparedFields;
                        continue;
                    }
                    ++diff.ComparedFields;
                    if (*left != *right)
                    {
                        diff.Fields.push_back(FieldDiff{ entry->Field, *left, *right });
                    }
                }
                if (!diff.Fields.empty())
                {
                    diff.State = "modified";
                }
                else if (diff.ComparedFields == 0)
                {
                    // Nothing about this component is reflected, so "identical"
                    // would be a claim this code cannot make.
                    diff.State = "unknown";
                }
                else
                {
                    diff.State = "identical";
                }
                diffs.push_back(std::move(diff));
            }
            return diffs;
        }

        Json DescribeDiff(const ComponentDiff& diff)
        {
            Json fields = Json::array();
            for (const auto& field : diff.Fields)
            {
                fields.push_back(Json{ { "field", field.Field }, { "instance", field.Instance }, { "prefab", field.Source } });
            }
            return Json{ { "component", diff.Component },
                         { "state", diff.State },
                         { "fields", std::move(fields) },
                         { "comparedFields", diff.ComparedFields },
                         { "uncomparedFields", diff.UncomparedFields } };
        }

        // Authored components an instance carries that a prefab does NOT copy.
        // Reported rather than ignored: the instance really does differ from
        // anything the prefab can express, and a caller told "no overrides" about
        // an entity carrying one of these has been misled.
        std::vector<std::string> UntrackedComponents(Entity entity)
        {
            static const std::vector<std::string_view> kHandledSeparately{ "IDComponent", "TagComponent",
                                                                           "PrefabComponent", "RelationshipComponent" };
            std::vector<std::string> names;
            for (const auto& type : ComponentTypes())
            {
                if (!type.Authored || !type.Has(entity))
                {
                    continue;
                }
                if (std::ranges::find(kHandledSeparately, type.Name) != kHandledSeparately.end())
                {
                    continue;
                }
                if (FindPrefabComponentType(type.Name) == nullptr)
                {
                    names.push_back(type.Name);
                }
            }
            return names;
        }

        // ---- resolving the prefab an operation is about ----------------------

        struct InstanceScope
        {
            Ref<Scene> ActiveScene;
            CommandHistory* History = nullptr;
            Entity Instance;
            Ref<Prefab> Prefab;
            AssetHandle Handle{ 0 };
            Entity Source;
            UUID PrefabRootId{ 0 };
        };

        Json ResolveInstance(const Ref<Scene>& scene, const Json& args, InstanceScope& scope)
        {
            const auto it = args.find("entity");
            if (it == args.end())
            {
                return Error("entity must be a nonzero decimal UUID string.");
            }
            const auto id = ParseDecimal(*it);
            if (!id || *id == 0)
            {
                return Error("entity must be a nonzero decimal UUID string.");
            }
            auto entity = scene->TryGetEntityWithUUID(UUID(*id));
            if (!entity)
            {
                return Error("Entity does not exist: " + std::to_string(*id));
            }
            scope.Instance = *entity;
            if (!entity->HasComponent<PrefabComponent>() || !entity->GetComponent<PrefabComponent>().IsValid())
            {
                return Error("Entity " + std::to_string(*id) + " is not a prefab instance.");
            }
            scope.Handle = entity->GetComponent<PrefabComponent>().m_PrefabID;
            // GetAsset resolves memory-only assets as well as registered ones, so
            // this also covers a prefab that exists only in this session.
            scope.Prefab = AssetManager::GetAsset<Prefab>(scope.Handle);
            if (!scope.Prefab || !scope.Prefab->GetScene() || !scope.Prefab->GetRootEntity())
            {
                return Error("The source prefab asset " + Identity(scope.Handle) +
                             " is missing or could not be loaded.");
            }
            scope.PrefabRootId = scope.Prefab->GetRootEntity().GetUUID();
            scope.Source = scope.Prefab->FindSourceEntity(*entity);
            if (!scope.Source)
            {
                return Error("Entity " + std::to_string(*id) + " points at prefab entity " +
                             Identity(entity->GetComponent<PrefabComponent>().m_PrefabEntityID) +
                             ", which no longer exists in prefab " + Identity(scope.Handle) +
                             ". The link is broken; re-instantiate or unpack the instance.");
            }
            return Json::object();
        }

        // The instance subtree that belongs to ONE instance: the entity plus every
        // descendant stamped from the same prefab and not itself an instance root.
        // The walk STOPS at a nested instance and records it instead, because
        // sweeping it in would let an operation on the outer instance silently
        // rewrite the inner one.
        //
        // Two things make a child nested, and the second is easy to miss: a
        // DIFFERENT prefab handle, or the SAME handle with the prefab's ROOT as its
        // source entity -- that is a second instance of this very prefab parented
        // under the first, and unpacking the outer one must not unpack it.
        void CollectInstanceSubtree(const Ref<Scene>& scene, Entity root, AssetHandle handle, UUID prefabRootId,
                                    std::vector<Entity>& outOwned, std::vector<Entity>& outNested)
        {
            outOwned.push_back(root);
            for (UUID childId : root.Children())
            {
                auto child = scene->TryGetEntityWithUUID(childId);
                if (!child)
                {
                    continue;
                }
                if (!child->HasComponent<PrefabComponent>() || !child->GetComponent<PrefabComponent>().IsValid())
                {
                    // An ordinary scene entity somebody parented under the instance.
                    // It is not part of the instance at all: including it would let
                    // its runtime state refuse an unpack, and would ask the override
                    // query to diff an entity with no prefab side.
                    continue;
                }
                const auto& pc = child->GetComponent<PrefabComponent>();
                if (pc.m_PrefabID != handle || pc.m_PrefabEntityID == prefabRootId)
                {
                    outNested.push_back(*child);
                    continue;
                }
                CollectInstanceSubtree(scene, *child, handle, prefabRootId, outOwned, outNested);
            }
        }

        void CollectSubtree(const Ref<Scene>& scene, Entity root, std::vector<Entity>& out)
        {
            out.push_back(root);
            for (UUID childId : root.Children())
            {
                if (auto child = scene->TryGetEntityWithUUID(childId))
                {
                    CollectSubtree(scene, *child, out);
                }
            }
        }

        // ---- moving component state between two entities --------------------

        // Copying a component to a second entity is a DUPLICATION: both ends end
        // up referring to whatever it holds. ValidateCopy is the per-component
        // form of the check olo_entity_duplicate already runs.
        std::string CopyComponent(const ComponentTypeEntry& type, Entity from, Entity to)
        {
            if (type.Has(from))
            {
                if (auto refusal = type.ValidateCopy(from); !refusal.empty())
                {
                    return refusal;
                }
                type.Capture(from)->Restore(to);
                return {};
            }
            if (!type.Remove)
            {
                return type.Name + " cannot be removed, so the absence cannot be propagated.";
            }
            type.Remove(to);
            return {};
        }

        // One registered field, moved through the same JSON the field-write tool
        // uses. FieldToJson and CoerceJson are inverses for every supported field
        // type, so the round trip is lossless and there is no second copy path to
        // drift from the one an agent can already drive.
        std::string CopyField(const Fields::FieldEntry& entry, Entity from, Entity to, UUID toId)
        {
            const auto value = entry.Read(from);
            if (!value)
            {
                return "The source entity has no " + entry.Component + ".";
            }
            const auto applied = entry.ApplyDirect(to, static_cast<u64>(toId), *value);
            if (!applied.Ok)
            {
                return applied.Error.empty() ? ("Could not write " + entry.Component + "." + entry.Field + ".")
                                             : applied.Error;
            }
            return {};
        }

        // What one apply/revert call was asked to move.
        struct MoveItem
        {
            const ComponentTypeEntry* Type = nullptr;
            const Fields::FieldEntry* Field = nullptr; // null => the whole component
            std::string State;                         // the diff state that selected it
        };

        // Resolve the explicit (component, field) arguments, or fall back to
        // every divergence the diff reports. An explicit component outside the
        // prefab-carried set is refused by name rather than quietly doing nothing,
        // and an explicit one that does not actually differ selects NOTHING -- a
        // no-op request must not rewrite the prefab file or leave an undo entry to
        // press Ctrl-Z past.
        Json SelectMoves(const Json& args, Entity instance, Entity source, bool isInstanceRoot,
                         std::vector<MoveItem>& out, Json& outSkipped)
        {
            const bool hasComponent = args.contains("component");
            const bool hasField = args.contains("field");
            if (hasField && !hasComponent)
            {
                return Error("field requires component.");
            }
            if (!hasComponent)
            {
                for (const auto& diff : DiffEntity(instance, source))
                {
                    if (diff.State != "added" && diff.State != "removed" && diff.State != "modified")
                    {
                        continue;
                    }
                    // An instance ROOT's transform is where somebody PUT this copy,
                    // not what the prefab says. A bare apply that swept it up would
                    // teleport every other instance to wherever this one happens to
                    // stand -- so it is left out and SAID so; naming the component
                    // explicitly still applies it.
                    if (isInstanceRoot && diff.Component == "TransformComponent")
                    {
                        outSkipped.push_back(
                            Json{ { "component", diff.Component },
                                  { "reason", "An instance root's transform is its placement in the scene, not prefab "
                                              "data. Pass component:\"TransformComponent\" to apply it anyway." } });
                        continue;
                    }
                    if (const ComponentTypeEntry* type = FindPrefabComponentType(diff.Component))
                    {
                        out.push_back(MoveItem{ type, nullptr, diff.State });
                    }
                }
                return Json::object();
            }
            if (!args.at("component").is_string())
            {
                return Error("component must be a string.");
            }
            const auto name = args.at("component").get<std::string>();
            const ComponentTypeEntry* type = FindPrefabComponentType(name);
            if (!type)
            {
                return Error("A prefab does not carry '" + name +
                             "'. Use olo_prefab_overrides to see the components it does carry.");
            }
            if (!hasField)
            {
                for (const auto& diff : DiffEntity(instance, source))
                {
                    if (diff.Component != name)
                    {
                        continue;
                    }
                    // "unknown" means no field of this component is reflected, so
                    // nothing here can say it is identical -- move it.
                    if (diff.State != "identical")
                    {
                        out.push_back(MoveItem{ type, nullptr, diff.State });
                    }
                    return Json::object();
                }
                return Json::object(); // present on neither side.
            }
            if (!args.at("field").is_string())
            {
                return Error("field must be a string.");
            }
            const auto fieldName = args.at("field").get<std::string>();
            const Fields::FieldEntry* field = Fields::Find(name, fieldName);
            if (!field)
            {
                std::string key;
                field = Fields::FindMapKeyed(name, fieldName, key);
                if (field)
                {
                    return Error("Map-keyed field '" + fieldName +
                                 "' cannot be moved on its own; apply or revert the whole component.");
                }
                return Error(Fields::DescribeUnknownField(name, fieldName));
            }
            if (!type->Has(instance) || !type->Has(source))
            {
                return Error("Both the instance and its prefab source must carry " + name +
                             " for a single-field operation; move the whole component instead.");
            }
            const auto instanceValue = field->Read(instance);
            const auto sourceValue = field->Read(source);
            if (instanceValue && sourceValue && *instanceValue == *sourceValue)
            {
                return Json::object(); // already equal: nothing to move.
            }
            out.push_back(MoveItem{ type, field, "field" });
            return Json::object();
        }

        // An override mark is per COMPONENT -- PrefabComponent has no finer unit --
        // so a FIELD-scoped apply or revert must not drop it while the component's
        // other fields still diverge: the next apply from another instance would
        // then stomp exactly the values that mark was protecting. Clear it only
        // once the component has actually stopped diverging.
        void ClearSettledOverrideMarks(Entity instance, Entity source, const std::vector<MoveItem>& moves)
        {
            auto& marks = instance.GetComponent<PrefabComponent>();
            const auto diffs = DiffEntity(instance, source);
            for (const MoveItem& move : moves)
            {
                const auto settled = std::ranges::find_if(diffs, [&move](const ComponentDiff& diff)
                                                          { return diff.Component == move.Type->Name; });
                if (settled == diffs.end() || settled->State == "identical")
                {
                    marks.ClearComponentOverride(move.Type->Name);
                }
            }
        }

        Json DescribeMove(const MoveItem& item)
        {
            Json described{ { "component", item.Type->Name }, { "state", item.State } };
            if (item.Field)
            {
                described["field"] = item.Field->Field;
            }
            return described;
        }

        // ---- prefab file + registry -----------------------------------------

        struct PrefabFile
        {
            bool Registered = false;
            std::filesystem::path Path;
            std::string Note;
        };

        PrefabFile LocatePrefabFile(AssetHandle handle)
        {
            PrefabFile located;
            auto manager = EditorManager();
            if (!manager)
            {
                located.Note = "The project has no editor asset manager, so the change lives only in memory.";
                return located;
            }
            const AssetMetadata metadata = manager->GetMetadata(handle);
            if (!metadata.IsValid() || metadata.FilePath.empty())
            {
                located.Note = "Prefab " + Identity(handle) +
                               " is a memory-only asset with no file, so the change lives only in memory.";
                return located;
            }
            located.Registered = true;
            located.Path = manager->GetFileSystemPath(metadata);
            return located;
        }

        // Throws rather than returning the empty string the serializer answers for
        // an invalid prefab: writing that out would leave a valid-looking
        // .oloprefab with nothing in it, and the caller would report success.
        std::string SerializePrefab(const Ref<Prefab>& prefab)
        {
            const PrefabSerializer serializer;
            std::string yaml = serializer.SerializeToYAML(prefab);
            if (yaml.empty())
            {
                throw std::runtime_error("The prefab could not be serialized; nothing was written.");
            }
            return yaml;
        }

        // ---- instantiate -----------------------------------------------------

        Json InstantiatePrefab(Ref<Scene> scene, CommandHistory& history, const Json& args,
                               const std::function<void()>& clearSelection)
        {
            const auto it = args.find("prefab");
            if (it == args.end())
            {
                return Error("prefab must be a decimal asset-handle string.");
            }
            const auto handleValue = ParseDecimal(*it);
            if (!handleValue || *handleValue == 0)
            {
                return Error("prefab must be a nonzero decimal asset-handle string.");
            }
            const AssetHandle handle{ *handleValue };
            Ref<Prefab> prefab = AssetManager::GetAsset<Prefab>(handle);
            if (!prefab)
            {
                return Error("No prefab asset is available under handle " + std::to_string(*handleValue) + ".");
            }
            if (!prefab->GetScene() || !prefab->GetRootEntity())
            {
                return Error("Prefab " + std::to_string(*handleValue) + " has no root entity to instantiate.");
            }

            Entity parent;
            if (const auto parentArg = args.find("parent"); parentArg != args.end())
            {
                const auto parentId = ParseDecimal(*parentArg);
                if (!parentId)
                {
                    return Error("parent must be a decimal UUID string ('0' leaves the instance a root).");
                }
                if (*parentId != 0)
                {
                    auto found = scene->TryGetEntityWithUUID(UUID(*parentId));
                    if (!found)
                    {
                        return Error("Parent entity does not exist: " + std::to_string(*parentId));
                    }
                    parent = *found;
                }
            }

            std::vector<EntityComponents> before;
            if (parent)
            {
                if (auto refusal = ValidateEntitySnapshot(parent); !refusal.empty())
                {
                    return Error(std::move(refusal));
                }
                before.push_back(CaptureEntity(scene, parent));
            }

            Entity root = prefab->Instantiate(*scene);
            if (!root)
            {
                return Error("Prefab " + std::to_string(*handleValue) + " failed to instantiate.");
            }
            if (const auto nameArg = args.find("name"); nameArg != args.end() && nameArg->is_string())
            {
                auto& tag = root.GetComponent<TagComponent>().Tag;
                const std::string previous = tag;
                tag = nameArg->get<std::string>();
                scene->UpdateEntityName(static_cast<entt::entity>(root), previous, tag);
            }
            if (parent)
            {
                root.SetParent(parent);
            }

            std::vector<Entity> created;
            CollectSubtree(scene, root, created);

            std::vector<SubtreeNode> nodes;
            nodes.reserve(created.size());
            for (Entity entity : created)
            {
                if (auto refusal = ValidateEntitySnapshot(entity); !refusal.empty())
                {
                    // The hierarchy exists but cannot be captured, so it could not
                    // be taken back either. Remove it and refuse, rather than
                    // leaving an instance behind with an undo entry that lies.
                    scene->DestroyEntityAndChildren(root);
                    RestoreAll(before);
                    return Error("The instantiated hierarchy cannot be captured for undo: " + refusal);
                }
                nodes.push_back(SubtreeNode{ entity.GetUUID(), entity.GetName(), CaptureEntityComponents(entity) });
            }

            std::vector<EntityComponents> after;
            if (parent)
            {
                after.push_back(CaptureEntity(scene, parent));
            }
            history.PushAlreadyExecuted(std::make_unique<PrefabSubtreeCommand>(
                scene, std::move(nodes), std::move(before), std::move(after), true, "Instantiate Prefab", clearSelection));

            Json children = Json::array();
            for (sizet index = 1; index < created.size(); ++index)
            {
                children.push_back(Identity(created[index].GetUUID()));
            }
            Json nestedHandles = Json::array();
            for (const AssetHandle nestedHandle : prefab->NestedPrefabHandles())
            {
                nestedHandles.push_back(Identity(nestedHandle));
            }
            return Json{ { "entity", Identity(root.GetUUID()) },
                         { "name", root.GetName() },
                         { "parent", Identity(root.GetParentUUID()) },
                         { "prefab", Identity(handle) },
                         { "children", std::move(children) },
                         { "entityCount", static_cast<u64>(created.size()) },
                         { "nestedPrefabs", std::move(nestedHandles) },
                         { "changed", true },
                         { "undoable", true } };
        }

        // ---- unpack ----------------------------------------------------------

        Json UnpackInstance(Ref<Scene> scene, CommandHistory& history, const Json& args,
                            const std::function<void()>& /*clearSelection*/)
        {
            InstanceScope scope;
            if (Json failure = ResolveInstance(scene, args, scope); failure.contains("__error"))
            {
                return failure;
            }
            const bool recursive = args.value("recursive", false);

            std::vector<Entity> owned;
            std::vector<Entity> nested;
            CollectInstanceSubtree(scene, scope.Instance, scope.Handle, scope.PrefabRootId, owned, nested);

            std::vector<Entity> targets = owned;
            std::vector<Entity> keptNested;
            if (recursive)
            {
                // A nested instance is unpacked too, and so is anything nested
                // inside IT -- each level relative to its own prefab handle.
                std::vector<Entity> frontier = nested;
                while (!frontier.empty())
                {
                    Entity entity = frontier.back();
                    frontier.pop_back();
                    const AssetHandle handle = entity.GetComponent<PrefabComponent>().m_PrefabID;
                    Ref<Prefab> innerPrefab = AssetManager::GetAsset<Prefab>(handle);
                    if (!innerPrefab || !innerPrefab->GetRootEntity())
                    {
                        // Its prefab is gone, so its own subtree boundary cannot be
                        // decided. Unpack just this entity and say nothing more.
                        targets.push_back(entity);
                        continue;
                    }
                    std::vector<Entity> innerOwned;
                    std::vector<Entity> innerNested;
                    CollectInstanceSubtree(scene, entity, handle, innerPrefab->GetRootEntity().GetUUID(), innerOwned,
                                           innerNested);
                    targets.insert(targets.end(), innerOwned.begin(), innerOwned.end());
                    frontier.insert(frontier.end(), innerNested.begin(), innerNested.end());
                }
            }
            else
            {
                keptNested = nested;
            }

            std::vector<EntityComponents> before;
            std::vector<EntityComponents> after;
            before.reserve(targets.size());
            after.reserve(targets.size());
            for (Entity entity : targets)
            {
                if (auto refusal = ValidateEntitySnapshot(entity); !refusal.empty())
                {
                    return Error(std::move(refusal));
                }
            }
            for (Entity entity : targets)
            {
                before.push_back(CaptureEntity(scene, entity));
            }
            u32 unpacked = 0;
            for (Entity entity : targets)
            {
                if (entity.HasComponent<PrefabComponent>())
                {
                    entity.RemoveComponent<PrefabComponent>();
                    ++unpacked;
                }
            }
            for (Entity entity : targets)
            {
                after.push_back(CaptureEntity(scene, entity));
            }
            if (unpacked == 0)
            {
                return Json{ { "entity", Identity(scope.Instance.GetUUID()) },
                             { "prefab", Identity(scope.Handle) },
                             { "unpacked", 0 },
                             { "nestedInstancesKept", Json::array() },
                             { "changed", false },
                             { "undoable", false } };
            }
            history.PushAlreadyExecuted(
                std::make_unique<PrefabStateCommand>(std::move(before), std::move(after), "Unpack Prefab Instance"));

            Json kept = Json::array();
            for (Entity entity : keptNested)
            {
                kept.push_back(Json{ { "entity", Identity(entity.GetUUID()) },
                                     { "prefab", Identity(entity.GetComponent<PrefabComponent>().m_PrefabID) } });
            }
            return Json{ { "entity", Identity(scope.Instance.GetUUID()) },
                         { "prefab", Identity(scope.Handle) },
                         { "unpacked", unpacked },
                         { "nestedInstancesKept", std::move(kept) },
                         { "changed", true },
                         { "undoable", true } };
        }

        // ---- override query --------------------------------------------------

        // An unordered_set reported verbatim would vary run to run, which makes a
        // result impossible to diff between two sessions.
        std::vector<std::string> SortedNames(const std::unordered_set<std::string>& names)
        {
            std::vector<std::string> sorted(names.begin(), names.end());
            std::ranges::sort(sorted);
            return sorted;
        }

        Json QueryOverrides(const Ref<Scene>& scene, const Json& args)
        {
            InstanceScope scope;
            if (Json failure = ResolveInstance(scene, args, scope); failure.contains("__error"))
            {
                return failure;
            }
            const bool includeChildren = args.value("includeChildren", true);

            std::vector<Entity> owned;
            std::vector<Entity> nested;
            CollectInstanceSubtree(scene, scope.Instance, scope.Handle, scope.PrefabRootId, owned, nested);
            if (!includeChildren)
            {
                owned.resize(1);
            }

            Json entities = Json::array();
            u32 divergedEntities = 0;
            u32 divergedComponents = 0;
            u32 brokenLinks = 0;
            for (Entity entity : owned)
            {
                Json described{ { "entity", Identity(entity.GetUUID()) }, { "name", entity.GetName() } };
                Entity source = scope.Prefab->FindSourceEntity(entity);
                if (!source)
                {
                    ++brokenLinks;
                    described["prefabEntity"] = Identity(entity.GetComponent<PrefabComponent>().m_PrefabEntityID);
                    described["linkBroken"] = true;
                    described["components"] = Json::array();
                    described["untrackedComponents"] = UntrackedComponents(entity);
                    described["markedOverridden"] = Json::array();
                    described["markedAdded"] = Json::array();
                    described["markedRemoved"] = Json::array();
                    described["diverged"] = false;
                    entities.push_back(std::move(described));
                    continue;
                }
                described["prefabEntity"] = Identity(source.GetUUID());
                described["linkBroken"] = false;

                Json components = Json::array();
                bool diverged = false;
                for (const auto& diff : DiffEntity(entity, source))
                {
                    if (diff.State == "identical")
                    {
                        continue;
                    }
                    if (diff.State != "unknown")
                    {
                        diverged = true;
                        ++divergedComponents;
                    }
                    components.push_back(DescribeDiff(diff));
                }
                described["components"] = std::move(components);
                described["untrackedComponents"] = UntrackedComponents(entity);

                // The editor's own marks, kept beside the computed diff rather
                // than merged into it: a mark with no value difference means a
                // human touched the component and put the value back, and that is
                // still what decides whether an update from the prefab skips it.
                const auto& pc = entity.GetComponent<PrefabComponent>();
                described["markedOverridden"] = SortedNames(pc.GetOverriddenComponents());
                described["markedAdded"] = SortedNames(pc.GetAddedComponents());
                described["markedRemoved"] = SortedNames(pc.GetRemovedComponents());
                described["diverged"] = diverged;
                if (diverged)
                {
                    ++divergedEntities;
                }
                entities.push_back(std::move(described));
            }

            Json nestedInstances = Json::array();
            for (Entity entity : nested)
            {
                nestedInstances.push_back(Json{ { "entity", Identity(entity.GetUUID()) },
                                                { "prefab", Identity(entity.GetComponent<PrefabComponent>().m_PrefabID) } });
            }
            return Json{ { "entity", Identity(scope.Instance.GetUUID()) },
                         { "prefab", Identity(scope.Handle) },
                         { "entities", std::move(entities) },
                         { "nestedInstances", std::move(nestedInstances) },
                         { "divergedEntities", divergedEntities },
                         { "divergedComponents", divergedComponents },
                         { "brokenLinks", brokenLinks },
                         { "changed", false },
                         { "undoable", false } };
        }

        // ---- apply -----------------------------------------------------------

        // A source entity that is itself a nested instance belongs to ANOTHER
        // prefab, so writing into it would edit that prefab under a caller who
        // named this one -- the "silently wrong nested apply" the issue calls the
        // worst outcome. Refused by name, never guessed at.
        //
        // The test is SELF-REFERENCE, not the handle: Prefab::Create stamps every
        // entity it owns with its own id as m_PrefabEntityID, while a nested
        // instance's points into the nested prefab's scene. Comparing m_PrefabID to
        // the live handle instead would refuse everything after a re-import, which
        // mints a new handle without re-stamping the scene the YAML restored.
        std::string RefuseNestedSource(const InstanceScope& scope)
        {
            if (!scope.Source.HasComponent<PrefabComponent>())
            {
                return {};
            }
            const auto& pc = scope.Source.GetComponent<PrefabComponent>();
            if (static_cast<u64>(pc.m_PrefabEntityID) == 0 || pc.m_PrefabEntityID == scope.Source.GetUUID())
            {
                return {};
            }
            return "Prefab entity " + Identity(scope.Source.GetUUID()) + " inside prefab " + Identity(scope.Handle) +
                   " is itself an instance of nested prefab " + Identity(pc.m_PrefabID) + " (source entity " +
                   Identity(pc.m_PrefabEntityID) +
                   "). Applying here would rewrite that prefab instead. Apply to the nested prefab's own instance, or "
                   "unpack the nesting first.";
        }

        Json ApplyOverrides(Ref<Scene> scene, CommandHistory& history, const Json& args,
                            const std::function<void()>& /*clearSelection*/)
        {
            InstanceScope scope;
            if (Json failure = ResolveInstance(scene, args, scope); failure.contains("__error"))
            {
                return failure;
            }
            if (auto refusal = RefuseNestedSource(scope); !refusal.empty())
            {
                return Error(std::move(refusal));
            }
            std::vector<MoveItem> moves;
            Json skippedComponents = Json::array();
            const bool isRoot = scope.Source.GetUUID() == scope.PrefabRootId;
            if (Json failure = SelectMoves(args, scope.Instance, scope.Source, isRoot, moves, skippedComponents);
                failure.contains("__error"))
            {
                return failure;
            }
            const bool resync = args.value("resyncInstances", true);
            const PrefabFile file = LocatePrefabFile(scope.Handle);
            if (moves.empty())
            {
                // Nothing diverges. Rewriting the .oloprefab with identical bytes
                // would move its timestamp and wake the asset watcher for nothing,
                // and the undo entry would be one the user has to step past.
                Json unchanged{ { "entity", Identity(scope.Instance.GetUUID()) },
                                { "prefab", Identity(scope.Handle) },
                                { "prefabEntity", Identity(scope.Source.GetUUID()) },
                                { "applied", Json::array() },
                                { "resyncedInstances", Json::array() },
                                { "skippedInstances", Json::array() },
                                { "skippedComponents", std::move(skippedComponents) },
                                { "prefabFileWritten", false },
                                { "changed", false },
                                { "undoable", false } };
                if (!file.Note.empty())
                {
                    unchanged["prefabFileNote"] = file.Note;
                }
                return unchanged;
            }

            FileContents fileBefore;
            if (file.Registered)
            {
                fileBefore = ReadFileContents(file.Path);
            }

            // Peers: every OTHER instance in the active scene stamped from the same
            // prefab entity. They are what makes "one undo puts all of them back"
            // a cross-object problem rather than a single-entity one.
            std::vector<Entity> peers;
            if (resync)
            {
                for (auto handleEntity : scene->GetAllEntitiesWith<PrefabComponent>())
                {
                    Entity candidate{ handleEntity, *scene };
                    if (candidate == scope.Instance)
                    {
                        continue;
                    }
                    const auto& pc = candidate.GetComponent<PrefabComponent>();
                    if (pc.m_PrefabID == scope.Handle && pc.m_PrefabEntityID == scope.Source.GetUUID())
                    {
                        peers.push_back(candidate);
                    }
                }
            }

            for (Entity peer : peers)
            {
                if (auto refusal = ValidateEntitySnapshot(peer); !refusal.empty())
                {
                    return Error("Instance " + Identity(peer.GetUUID()) + " cannot be re-synced undoably: " + refusal);
                }
            }
            if (auto refusal = ValidateEntitySnapshot(scope.Instance); !refusal.empty())
            {
                return Error(std::move(refusal));
            }
            if (auto refusal = ValidateEntitySnapshot(scope.Source); !refusal.empty())
            {
                return Error("The prefab's source entity cannot be captured for undo: " + refusal);
            }
            for (const MoveItem& move : moves)
            {
                if (move.Field == nullptr)
                {
                    if (auto refusal = move.Type->ValidateCopy(scope.Instance); !refusal.empty())
                    {
                        return Error(std::move(refusal));
                    }
                }
            }

            // Everything that will change, captured BEFORE anything does.
            const Ref<Scene> prefabScene = scope.Prefab->GetScene();
            std::vector<EntityComponents> before;
            before.push_back(CaptureEntity(prefabScene, scope.Source, scope.Handle));
            before.push_back(CaptureEntity(scene, scope.Instance));
            for (Entity peer : peers)
            {
                before.push_back(CaptureEntity(scene, peer));
            }

            Json applied = Json::array();
            for (const MoveItem& move : moves)
            {
                const std::string failure = move.Field
                                                ? CopyField(*move.Field, scope.Instance, scope.Source, scope.Source.GetUUID())
                                                : CopyComponent(*move.Type, scope.Instance, scope.Source);
                if (!failure.empty())
                {
                    // Nothing is committed to the history yet, so put back what
                    // the earlier moves already changed rather than leaving a
                    // half-applied prefab nobody can undo.
                    RestoreAll(before);
                    return Error("Applying " + move.Type->Name + " failed: " + failure);
                }
                applied.push_back(DescribeMove(move));
            }

            ClearSettledOverrideMarks(scope.Instance, scope.Source, moves);

            Json resynced = Json::array();
            Json skipped = Json::array();
            for (Entity peer : peers)
            {
                Json movedHere = Json::array();
                Json skippedHere = Json::array();
                for (const MoveItem& move : moves)
                {
                    const auto& peerPrefab = peer.GetComponent<PrefabComponent>();
                    // A peer that deliberately overrides this component keeps its
                    // own value: that is what an override IS, and stomping it
                    // would make apply a way to lose somebody else's edit.
                    if (peerPrefab.IsComponentOverridden(move.Type->Name) ||
                        peerPrefab.IsComponentAdded(move.Type->Name) ||
                        peerPrefab.IsComponentRemoved(move.Type->Name))
                    {
                        skippedHere.push_back(move.Type->Name);
                        continue;
                    }
                    // At the SAME granularity the caller asked for. Copying the
                    // whole component for a single-field apply would wipe a peer's
                    // own divergence in that component's OTHER fields -- divergence
                    // nothing marked, and which the caller never asked to touch.
                    if (move.Field && !move.Type->Has(peer))
                    {
                        skippedHere.push_back(move.Type->Name);
                        continue;
                    }
                    const std::string failure = move.Field
                                                    ? CopyField(*move.Field, scope.Source, peer, peer.GetUUID())
                                                    : CopyComponent(*move.Type, scope.Source, peer);
                    if (!failure.empty())
                    {
                        RestoreAll(before);
                        return Error("Re-syncing instance " + Identity(peer.GetUUID()) + " failed: " + failure);
                    }
                    movedHere.push_back(move.Field ? (move.Type->Name + "." + move.Field->Field) : move.Type->Name);
                }
                if (!movedHere.empty())
                {
                    resynced.push_back(Json{ { "entity", Identity(peer.GetUUID()) }, { "components", std::move(movedHere) } });
                }
                if (!skippedHere.empty())
                {
                    skipped.push_back(Json{ { "entity", Identity(peer.GetUUID()) }, { "components", std::move(skippedHere) } });
                }
            }

            std::vector<EntityComponents> after;
            after.push_back(CaptureEntity(prefabScene, scope.Source, scope.Handle));
            after.push_back(CaptureEntity(scene, scope.Instance));
            for (Entity peer : peers)
            {
                after.push_back(CaptureEntity(scene, peer));
            }

            FileContents fileAfter;
            if (file.Registered)
            {
                try
                {
                    fileAfter = SerializePrefab(scope.Prefab);
                    ReplaceFileContents(file.Path, fileBefore, fileAfter);
                }
                catch (const std::exception& e)
                {
                    // Nothing is in the history yet, so the whole apply is taken
                    // back rather than leaving a prefab in memory that disagrees
                    // with the file on disk.
                    RestoreAll(before);
                    return Error(std::string("The prefab file could not be written, so nothing was applied: ") + e.what());
                }
            }

            auto group = std::make_unique<CompoundCommand>("Apply Prefab Override");
            group->Add(std::make_unique<PrefabStateCommand>(std::move(before), std::move(after), "Apply Prefab Override"));
            if (file.Registered)
            {
                group->Add(std::make_unique<PrefabFileCommand>(file.Path, std::move(fileBefore), std::move(fileAfter),
                                                               "Write Prefab File"));
            }
            history.PushAlreadyExecuted(std::move(group));

            Json result{ { "entity", Identity(scope.Instance.GetUUID()) },
                         { "prefab", Identity(scope.Handle) },
                         { "prefabEntity", Identity(scope.Source.GetUUID()) },
                         { "applied", std::move(applied) },
                         { "resyncedInstances", std::move(resynced) },
                         { "skippedInstances", std::move(skipped) },
                         { "skippedComponents", std::move(skippedComponents) },
                         { "prefabFileWritten", file.Registered },
                         { "changed", true },
                         { "undoable", true } };
            if (!file.Note.empty())
            {
                result["prefabFileNote"] = file.Note;
            }
            return result;
        }

        // ---- revert ----------------------------------------------------------

        Json RevertOverrides(Ref<Scene> scene, CommandHistory& history, const Json& args,
                             const std::function<void()>& /*clearSelection*/)
        {
            InstanceScope scope;
            if (Json failure = ResolveInstance(scene, args, scope); failure.contains("__error"))
            {
                return failure;
            }
            std::vector<MoveItem> moves;
            Json ignoredSkips = Json::array();
            // isInstanceRoot=false on purpose: the root-transform carve-out exists
            // because an APPLY would push one instance's placement onto every other
            // one. A revert only moves THIS instance back to where the prefab says,
            // which is precisely what it is being asked to do.
            if (Json failure = SelectMoves(args, scope.Instance, scope.Source, false, moves, ignoredSkips);
                failure.contains("__error"))
            {
                return failure;
            }
            if (auto refusal = ValidateEntitySnapshot(scope.Instance); !refusal.empty())
            {
                return Error(std::move(refusal));
            }
            for (const MoveItem& move : moves)
            {
                if (move.Field == nullptr)
                {
                    if (auto refusal = move.Type->ValidateCopy(scope.Source); !refusal.empty())
                    {
                        return Error(std::move(refusal));
                    }
                }
            }

            std::vector<EntityComponents> before{ CaptureEntity(scene, scope.Instance) };
            Json reverted = Json::array();
            for (const MoveItem& move : moves)
            {
                const std::string failure = move.Field
                                                ? CopyField(*move.Field, scope.Source, scope.Instance, scope.Instance.GetUUID())
                                                : CopyComponent(*move.Type, scope.Source, scope.Instance);
                if (!failure.empty())
                {
                    RestoreAll(before);
                    return Error("Reverting " + move.Type->Name + " failed: " + failure);
                }
                reverted.push_back(DescribeMove(move));
            }
            ClearSettledOverrideMarks(scope.Instance, scope.Source, moves);
            std::vector<EntityComponents> after{ CaptureEntity(scene, scope.Instance) };

            const bool changed = !reverted.empty();
            if (changed)
            {
                history.PushAlreadyExecuted(
                    std::make_unique<PrefabStateCommand>(std::move(before), std::move(after), "Revert Prefab Override"));
            }
            return Json{ { "entity", Identity(scope.Instance.GetUUID()) },
                         { "prefab", Identity(scope.Handle) },
                         { "prefabEntity", Identity(scope.Source.GetUUID()) },
                         { "reverted", std::move(reverted) },
                         { "changed", changed },
                         { "undoable", changed } };
        }

        // ---- create from an entity subtree -----------------------------------

        // Prefab::Create mints FRESH uuids for the prefab-scene copies and returns
        // no mapping, so stamping the source subtree needs one. The prefab scene
        // is built by walking the source's children in order and SetParent-ing
        // each copy, and SetParent appends -- so a parallel walk recovers the
        // pairing by construction rather than by guessing. If the two shapes ever
        // disagree the walk REFUSES: a mis-paired stamp would make every later
        // apply write one entity's override into a different entity.
        std::string PairSubtrees(const Ref<Scene>& sourceScene, Entity source, const Ref<Scene>& prefabScene,
                                 Entity copy, std::vector<std::pair<Entity, UUID>>& out)
        {
            out.emplace_back(source, copy.GetUUID());
            const auto& sourceChildren = source.Children();
            const auto& copyChildren = copy.Children();
            if (sourceChildren.size() != copyChildren.size())
            {
                return "The prefab copy of entity " + Identity(source.GetUUID()) + " has " +
                       std::to_string(copyChildren.size()) + " children where the source has " +
                       std::to_string(sourceChildren.size()) + "; the source entity cannot be linked to the prefab.";
            }
            for (sizet index = 0; index < sourceChildren.size(); ++index)
            {
                auto sourceChild = sourceScene->TryGetEntityWithUUID(sourceChildren[index]);
                auto copyChild = prefabScene->TryGetEntityWithUUID(copyChildren[index]);
                if (!sourceChild || !copyChild)
                {
                    return "A child entity vanished while pairing the prefab copy with its source.";
                }
                if (auto failure = PairSubtrees(sourceScene, *sourceChild, prefabScene, *copyChild, out);
                    !failure.empty())
                {
                    return failure;
                }
            }
            return {};
        }

        Json CreatePrefab(Ref<Scene> scene, CommandHistory& history, const Json& args,
                          const std::function<void()>& /*clearSelection*/)
        {
            auto manager = EditorManager();
            if (!manager)
            {
                return Error("Creating a prefab requires an editor asset manager (no active project).");
            }
            const auto entityArg = args.find("entity");
            if (entityArg == args.end())
            {
                return Error("entity must be a nonzero decimal UUID string.");
            }
            const auto entityId = ParseDecimal(*entityArg);
            if (!entityId || *entityId == 0)
            {
                return Error("entity must be a nonzero decimal UUID string.");
            }
            auto root = scene->TryGetEntityWithUUID(UUID(*entityId));
            if (!root)
            {
                return Error("Entity does not exist: " + std::to_string(*entityId));
            }
            if (!args.contains("path") || !args.at("path").is_string())
            {
                return Error("path must be a project-relative .oloprefab path.");
            }

            std::vector<Entity> subtree;
            CollectSubtree(scene, *root, subtree);
            for (Entity entity : subtree)
            {
                if (entity.HasComponent<PrefabComponent>() && entity.GetComponent<PrefabComponent>().IsValid())
                {
                    return Error("Entity " + Identity(entity.GetUUID()) + " is already an instance of prefab " +
                                 Identity(entity.GetComponent<PrefabComponent>().m_PrefabID) +
                                 ". Creating a prefab from it would nest that prefab, and a nested apply cannot be "
                                 "resolved here. Unpack it first with olo_prefab_unpack.");
                }
                if (auto refusal = ValidateEntityDuplication(entity); !refusal.empty())
                {
                    return Error("Entity " + Identity(entity.GetUUID()) + " cannot be copied into a prefab: " + refusal);
                }
            }

            const std::filesystem::path requested = args.at("path").get<std::string>();
            if (requested.is_absolute())
            {
                return Error("path must be project-relative.");
            }
            if (AssetExtensions::GetAssetTypeFromPath(requested.string()) != AssetType::Prefab)
            {
                return Error("path must end in " + std::string(OloExtensions::Prefab) + ".");
            }
            const std::filesystem::path absolute = (Project::GetProjectDirectory() / requested).lexically_normal();
            const std::filesystem::path assetDirectory = Project::GetAssetDirectory().lexically_normal();
            // lexically_relative rather than a string prefix: a prefix test lets
            // "Assets2/x.oloprefab" pass as being inside "Assets".
            const std::filesystem::path insideAssets = absolute.lexically_relative(assetDirectory);
            if (insideAssets.empty() || *insideAssets.begin() == "..")
            {
                return Error("path must be inside the project asset directory (" +
                             assetDirectory.filename().generic_string() + "/).");
            }
            std::error_code ec;
            const bool exists = std::filesystem::exists(absolute, ec) && !ec;
            if (exists)
            {
                return Error("A file already exists at " + requested.generic_string() +
                             ". Prefab creation never overwrites; pick another path or delete it with "
                             "olo_asset_delete.");
            }
            std::filesystem::create_directories(absolute.parent_path(), ec);
            if (ec)
            {
                return Error("Could not create the destination directory: " + ec.message());
            }

            Ref<Prefab> prefab = Ref<Prefab>::Create();
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset(prefab);
            if (static_cast<u64>(handle) == 0)
            {
                return Error("The asset manager did not mint a handle for the new prefab.");
            }
            prefab->Create(*root, false);
            if (!prefab->GetRootEntity())
            {
                manager->RemoveAsset(handle);
                return Error("The prefab was created empty; nothing was written.");
            }

            std::vector<std::pair<Entity, UUID>> pairs;
            if (auto failure = PairSubtrees(scene, *root, prefab->GetScene(), prefab->GetRootEntity(), pairs);
                !failure.empty())
            {
                manager->RemoveAsset(handle);
                return Error(std::move(failure));
            }

            std::vector<EntityComponents> before;
            std::vector<EntityComponents> after;
            before.reserve(pairs.size());
            after.reserve(pairs.size());
            for (const auto& [entity, sourceId] : pairs)
            {
                before.push_back(CaptureEntity(scene, entity));
            }
            for (const auto& [entity, sourceId] : pairs)
            {
                Entity stamped = entity;
                stamped.AddOrReplaceComponent<PrefabComponent>(handle, sourceId);
            }
            for (const auto& [entity, sourceId] : pairs)
            {
                after.push_back(CaptureEntity(scene, entity));
            }

            AssetMetadata metadata;
            metadata.Handle = handle;
            metadata.FilePath = EditorAssetManager::MakeRegistryKey(absolute, Project::GetProjectDirectory());
            metadata.Type = AssetType::Prefab;
            metadata.IsDataLoaded = true;

            std::string yaml;
            try
            {
                yaml = SerializePrefab(prefab);
                ReplaceFileContents(absolute, std::nullopt, yaml);
            }
            catch (const std::exception& e)
            {
                RestoreAll(before);
                manager->RemoveAsset(handle);
                return Error(std::string("The prefab file could not be written: ") + e.what());
            }
            manager->SetMetadata(handle, metadata);
            if (!manager->SerializeAssetRegistry())
            {
                RestoreAll(before);
                manager->RemoveAsset(handle);
                ReplaceFileContents(absolute, yaml, std::nullopt);
                return Error("The asset registry could not be written; the prefab was not created.");
            }

            auto group = std::make_unique<CompoundCommand>("Create Prefab");
            group->Add(std::make_unique<PrefabStateCommand>(std::move(before), std::move(after), "Link Prefab Instance"));
            group->Add(std::make_unique<PrefabFileCommand>(absolute, std::nullopt, yaml, "Write Prefab File"));
            group->Add(std::make_unique<PrefabRegistrationCommand>(prefab, metadata));
            history.PushAlreadyExecuted(std::move(group));

            return Json{ { "prefab", Identity(handle) },
                         { "path", metadata.FilePath.generic_string() },
                         { "entity", Identity(root->GetUUID()) },
                         { "entityCount", static_cast<u64>(pairs.size()) },
                         { "linkedInstance", true },
                         { "changed", true },
                         { "undoable", true } };
        }

        // ---- registration ----------------------------------------------------

        using PrefabMutation = Json (*)(Ref<Scene>, CommandHistory&, const Json&, const std::function<void()>&);

        AutomationResult RunMutation(IAutomationHost& host, const Json& args, PrefabMutation mutation)
        {
            const auto getScene = host.Context().GetActiveScene;
            const auto getHistory = host.Context().GetCommandHistory;
            const auto select = host.Context().SelectEntityInEditor;
            const auto invalidate = host.Context().InvalidateEntityReferences;
            Json result = host.MarshalRead([args, mutation, getScene, getHistory, select, invalidate]() -> Json
                                           {
                if (!getScene || !getHistory)
                    return Error("Prefab authoring requires an active editor scene and command history.");
                Ref<Scene> scene = getScene();
                CommandHistory* history = getHistory();
                if (!scene || !history)
                    return Error("Prefab authoring is available only in Edit mode with an active scene.");
                const std::function<void()> clearSelection = [select, invalidate]()
                {
                    if (invalidate)
                        invalidate();
                    else if (select)
                        select(0, true);
                };
                try
                {
                    return mutation(scene, *history, args, clearSelection);
                }
                catch (const std::exception& e)
                {
                    // A component capture refuses by throwing (a missing generated
                    // LOD asset). Report it as a command error instead of letting
                    // it escape into the marshalling machinery.
                    return Error(std::string("The prefab operation was refused: ") + e.what());
                } });
            if (result.contains("__error"))
            {
                return AutomationResult::Error(result.at("__error").get<std::string>());
            }
            return AutomationResult::Structured(result);
        }

        AutomationResult RunQuery(IAutomationHost& host, const Json& args)
        {
            const auto getScene = host.Context().GetActiveScene;
            Json result = host.MarshalRead([args, getScene]() -> Json
                                           {
                if (!getScene)
                    return Error("No active editor scene is available.");
                Ref<Scene> scene = getScene();
                if (!scene)
                    return Error("No active editor scene is available.");
                return QueryOverrides(scene, args); });
            if (result.contains("__error"))
            {
                return AutomationResult::Error(result.at("__error").get<std::string>());
            }
            return AutomationResult::Structured(result);
        }

        Schema::Node EntityIdSchema()
        {
            return Schema::String().Desc("Nonzero decimal entity UUID string.");
        }

        Schema::Node ComponentSelectorSchema()
        {
            return Schema::String().Desc(
                "Canonical component type carried by prefabs. Omit to move every divergence olo_prefab_overrides "
                "reports for this entity.");
        }

        Schema::Node FieldSelectorSchema()
        {
            return Schema::String().Desc(
                "One registered field of `component` (the olo_entity_set_field spelling, dotted for a nested "
                "member). Requires component; omit for the whole component.");
        }

        Schema::Node MoveListSchema()
        {
            return Schema::Array(Schema::Object()
                                     .Prop("component", Schema::String())
                                     .Prop("field", Schema::String())
                                     .Prop("state", Schema::String()));
        }

        Schema::Node InstanceComponentListSchema()
        {
            return Schema::Array(Schema::Object()
                                     .Prop("entity", Schema::String())
                                     .Prop("components", Schema::Array(Schema::String())));
        }

        void RegisterMutation(AutomationRegistry& registry, std::string name, std::string title, std::string description,
                              const Schema::Node& input, const Schema::Node& output, bool destructive,
                              PrefabMutation mutation)
        {
            AutomationCommand command;
            command.Name = std::move(name);
            command.Title = std::move(title);
            command.Description = std::move(description);
            command.Toolset = "scene";
            command.InputSchema = input;
            command.OutputSchema = output;
            command.Annotations = Json{ { "readOnlyHint", false },
                                        { "destructiveHint", destructive },
                                        { "idempotentHint", false },
                                        { "openWorldHint", false } };
            command.ProjectWrite = true;
            command.MainMarshaled = true;
            command.Undo = AutomationUndo::EditorUndoStack;
            command.Handler = [mutation](IAutomationHost& host, const Json& args)
            { return RunMutation(host, args, mutation); };
            registry.Register(std::move(command));
        }
    } // namespace

    void RegisterPrefabCommands(AutomationRegistry& registry)
    {
        RegisterMutation(
            registry, "olo_prefab_instantiate", "Instantiate prefab",
            "Instantiate a prefab asset into the active scene in Edit mode, optionally under `parent` and with a "
            "`name` override. Returns the root plus every child UUID; one undo removes the whole hierarchy and redo "
            "restores it under the same UUIDs. `nestedPrefabs` lists the prefab handles the instantiated hierarchy "
            "nests -- those children are instances of THEIR prefab, not of this one.",
            Schema::Object()
                .Prop("prefab", Schema::String().Desc("Nonzero decimal asset-handle string of the prefab."))
                .Prop("parent", Schema::String().Desc("Parent UUID string; omit or use '0' for a root instance."))
                .Prop("name", Schema::String().Desc("Name for the instance root; omitted keeps the prefab's own."))
                .Required({ "prefab" })
                .NoAdditional(),
            Schema::Object()
                .Prop("entity", Schema::String())
                .Prop("name", Schema::String())
                .Prop("parent", Schema::String())
                .Prop("prefab", Schema::String())
                .Prop("children", Schema::Array(Schema::String()))
                .Prop("entityCount", Schema::Int())
                .Prop("nestedPrefabs", Schema::Array(Schema::String()))
                .Prop("changed", Schema::Bool())
                .Prop("undoable", Schema::Bool())
                .Required({ "entity", "prefab", "children", "changed", "undoable" }),
            false, InstantiatePrefab);

        RegisterMutation(
            registry, "olo_prefab_unpack", "Unpack prefab instance",
            "Break the link between an instance and its prefab, leaving ordinary entities with the same components. "
            "One level by default: a nested instance keeps its own link and is reported under "
            "`nestedInstancesKept`. `recursive:true` unpacks those too, each against its own prefab. One undo "
            "restores every link.",
            Schema::Object()
                .Prop("entity", EntityIdSchema())
                .Prop("recursive", Schema::Bool().Desc("Also unpack nested prefab instances inside the subtree."))
                .Required({ "entity" })
                .NoAdditional(),
            Schema::Object()
                .Prop("entity", Schema::String())
                .Prop("prefab", Schema::String())
                .Prop("unpacked", Schema::Int())
                .Prop("nestedInstancesKept",
                      Schema::Array(Schema::Object().Prop("entity", Schema::String()).Prop("prefab", Schema::String())))
                .Prop("changed", Schema::Bool())
                .Prop("undoable", Schema::Bool())
                .Required({ "entity", "prefab", "unpacked", "changed", "undoable" }),
            true, UnpackInstance);

        {
            AutomationCommand command;
            command.Name = "olo_prefab_overrides";
            command.Title = "Query prefab overrides";
            command.Description =
                "What has this instance diverged on, FIELD BY FIELD, against its source prefab -- without opening the "
                "editor. Each entity in the instance subtree reports per-component `added` / `removed` / `modified` "
                "state with the differing fields' instance and prefab values, plus `comparedFields` and "
                "`uncomparedFields` so a partial comparison is never mistaken for a clean one (a component with no "
                "reflected fields reports state `unknown`, not `identical`). `untrackedComponents` names authored "
                "components a prefab does not carry at all, `nestedInstances` the children that belong to another "
                "prefab, and `markedOverridden`/`markedAdded`/`markedRemoved` the editor's own marks, which are what "
                "decide whether an update from the prefab skips a component.";
            command.Toolset = "scene";
            command.InputSchema = Schema::Object()
                                      .Prop("entity", EntityIdSchema())
                                      .Prop("includeChildren",
                                            Schema::Bool().Desc("Include the instance's linked descendants (default true)."))
                                      .Required({ "entity" })
                                      .NoAdditional();
            command.OutputSchema =
                Schema::Object()
                    .Prop("entity", Schema::String())
                    .Prop("prefab", Schema::String())
                    .Prop("entities", Schema::Array(Schema::Object()
                                                        .Prop("entity", Schema::String())
                                                        .Prop("name", Schema::String())
                                                        .Prop("prefabEntity", Schema::String())
                                                        .Prop("linkBroken", Schema::Bool())
                                                        .Prop("diverged", Schema::Bool())
                                                        .Prop("components", Schema::Array(Schema::Object()))
                                                        .Prop("untrackedComponents", Schema::Array(Schema::String()))
                                                        .Prop("markedOverridden", Schema::Array(Schema::String()))
                                                        .Prop("markedAdded", Schema::Array(Schema::String()))
                                                        .Prop("markedRemoved", Schema::Array(Schema::String()))))
                    .Prop("nestedInstances",
                          Schema::Array(Schema::Object().Prop("entity", Schema::String()).Prop("prefab", Schema::String())))
                    .Prop("divergedEntities", Schema::Int())
                    .Prop("divergedComponents", Schema::Int())
                    .Prop("brokenLinks", Schema::Int())
                    .Prop("changed", Schema::Bool())
                    .Prop("undoable", Schema::Bool())
                    .Required({ "entity", "prefab", "entities", "divergedComponents", "changed", "undoable" });
            command.Annotations = Json{ { "readOnlyHint", true },
                                        { "destructiveHint", false },
                                        { "idempotentHint", true },
                                        { "openWorldHint", false } };
            command.MainMarshaled = true;
            command.Undo = AutomationUndo::None;
            command.Handler = [](IAutomationHost& host, const Json& args)
            { return RunQuery(host, args); };
            registry.Register(std::move(command));
        }

        RegisterMutation(
            registry, "olo_prefab_apply", "Apply override to prefab",
            "Push an instance's divergence back into its source prefab: one `field`, one `component`, or every "
            "divergence when neither is given. The source prefab's entity is rewritten, the .oloprefab is "
            "re-written (atomically, and refused if somebody edited it in the meantime), and every OTHER instance of "
            "the same prefab entity in the active scene re-syncs AT THE SAME GRANULARITY -- a single-field apply "
            "moves that one field, never the whole component, so a peer's own divergence in the component's other "
            "fields survives. A peer that marks the component overridden itself keeps its value entirely and is "
            "listed under `skippedInstances`. All of that is ONE undo entry: a single Ctrl-Z puts the prefab, the "
            "file and every re-synced instance back. A bare apply on an instance ROOT leaves TransformComponent out "
            "and says so under `skippedComponents` -- a root's transform is where somebody put this copy, not prefab "
            "data -- but naming the component explicitly applies it. Refused when the source entity is itself a "
            "nested prefab instance, because the write would land in that prefab instead. Edit mode only.",
            Schema::Object()
                .Prop("entity", EntityIdSchema())
                .Prop("component", ComponentSelectorSchema())
                .Prop("field", FieldSelectorSchema())
                .Prop("resyncInstances",
                      Schema::Bool().Desc("Re-sync the prefab's other instances in the active scene (default true)."))
                .Required({ "entity" })
                .NoAdditional(),
            Schema::Object()
                .Prop("entity", Schema::String())
                .Prop("prefab", Schema::String())
                .Prop("prefabEntity", Schema::String())
                .Prop("applied", MoveListSchema())
                .Prop("resyncedInstances", InstanceComponentListSchema())
                .Prop("skippedInstances", InstanceComponentListSchema())
                .Prop("skippedComponents", Schema::Array(Schema::Object()
                                                             .Prop("component", Schema::String())
                                                             .Prop("reason", Schema::String())))
                .Prop("prefabFileWritten", Schema::Bool())
                .Prop("prefabFileNote", Schema::String())
                .Prop("changed", Schema::Bool())
                .Prop("undoable", Schema::Bool())
                .Required({ "entity", "prefab", "applied", "prefabFileWritten", "changed", "undoable" }),
            true, ApplyOverrides);

        RegisterMutation(
            registry, "olo_prefab_revert", "Revert override to prefab value",
            "Take an instance back to its prefab's values: one `field`, one `component`, or every divergence when "
            "neither is given. Only the instance changes -- the prefab asset and its other instances are untouched -- "
            "and the editor's override marks for the reverted components are cleared. One undo entry. Allowed even "
            "when the source entity is a nested instance, because reverting only READS the source. Edit mode only.",
            Schema::Object()
                .Prop("entity", EntityIdSchema())
                .Prop("component", ComponentSelectorSchema())
                .Prop("field", FieldSelectorSchema())
                .Required({ "entity" })
                .NoAdditional(),
            Schema::Object()
                .Prop("entity", Schema::String())
                .Prop("prefab", Schema::String())
                .Prop("prefabEntity", Schema::String())
                .Prop("reverted", MoveListSchema())
                .Prop("changed", Schema::Bool())
                .Prop("undoable", Schema::Bool())
                .Required({ "entity", "prefab", "reverted", "changed", "undoable" }),
            true, RevertOverrides);

        RegisterMutation(
            registry, "olo_prefab_create", "Create prefab from entity subtree",
            "Write an entity and its descendants to a new `.oloprefab` inside the asset directory, register it, and "
            "link the source subtree to it so it becomes a live instance (every entity gets the PrefabComponent that "
            "maps it to its copy, which is what makes a later apply/revert address the right entity). Never "
            "overwrites an existing file. Refused when the subtree already contains a prefab instance: that would "
            "nest a prefab, and a nested apply cannot be resolved -- unpack it first. One undo entry removes the "
            "links, the registry entry and the file. Edit mode only.",
            Schema::Object()
                .Prop("entity", EntityIdSchema())
                .Prop("path", Schema::String().Desc(
                                  "Project-relative destination ending in .oloprefab, inside the asset directory."))
                .Required({ "entity", "path" })
                .NoAdditional(),
            Schema::Object()
                .Prop("prefab", Schema::String())
                .Prop("path", Schema::String())
                .Prop("entity", Schema::String())
                .Prop("entityCount", Schema::Int())
                .Prop("linkedInstance", Schema::Bool())
                .Prop("changed", Schema::Bool())
                .Prop("undoable", Schema::Bool())
                .Required({ "prefab", "path", "entity", "changed", "undoable" }),
            true, CreatePrefab);
    }
} // namespace OloEngine::Automation
