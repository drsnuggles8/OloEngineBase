#include "OloEnginePCH.h"
#include "Automation/AutomationComponentRegistry.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "UndoRedo/EditorCommand.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace OloEngine::Automation
{
    namespace
    {
#include "OloEngine/Scene/Generated/ComponentTypes.Generated.inl"

        bool HasMutableTerrainResources(const TerrainComponent& data)
        {
            return data.m_TerrainData || data.m_Material || data.m_Streamer || data.m_VoxelOverride;
        }

        void RetainTerrainResources(TerrainComponent& target, const TerrainComponent& source)
        {
            target.m_TerrainData = source.m_TerrainData;
            target.m_ChunkManager = source.m_ChunkManager;
            target.m_Material = source.m_Material;
            target.m_Streamer = source.m_Streamer;
            target.m_VoxelOverride = source.m_VoxelOverride;
            target.m_VirtualTexture = source.m_VirtualTexture;
            target.m_VoxelMeshes = source.m_VoxelMeshes;
            target.m_VoxelQuadMeshes = source.m_VoxelQuadMeshes;
            target.m_VoxelAutoSeeded = source.m_VoxelAutoSeeded;
            target.m_NeedsRebuild = source.m_NeedsRebuild;
            target.m_MaterialNeedsRebuild = source.m_MaterialNeedsRebuild;
            target.m_AutoSplatNeedsRebuild = source.m_AutoSplatNeedsRebuild;
        }

        template<typename T>
        class TypedComponentSnapshot final : public ComponentSnapshot
        {
          public:
            explicit TypedComponentSnapshot(const T& data)
                : m_Data(data)
            {
            }

            void Restore(Entity entity) const override
            {
                if (!entity.HasComponent<T>())
                {
                    entity.AddComponent<T>(m_Data);
                }
                // The add hook initializes subsystem state (notably the camera's
                // viewport). Restore the captured authored values after that hook.
                entity.GetComponent<T>() = m_Data;
            }

          private:
            T m_Data;
        };

        // Sculpt/paint data is authored in GPU-backed resources that ordinary scene
        // copies deliberately discard. Hold the original resources while absent;
        // restoring them also keeps CPU/GPU revision and pending rebuild state.
        template<>
        class TypedComponentSnapshot<TerrainComponent> final : public ComponentSnapshot
        {
          public:
            explicit TypedComponentSnapshot(const TerrainComponent& data)
                : m_Data(data)
            {
                RetainTerrainResources(m_Data, data);
            }

            void Restore(Entity entity) const override
            {
                if (!entity.HasComponent<TerrainComponent>())
                {
                    entity.AddComponent<TerrainComponent>(m_Data);
                }
                auto& target = entity.GetComponent<TerrainComponent>();
                target = m_Data;
                RetainTerrainResources(target, m_Data);
            }

          private:
            TerrainComponent m_Data;
        };

        // The graph editor can author an unsaved graph in Edit mode. Ordinary
        // component copies reset that Ref for Play; an undo memento must retain it.
        template<>
        class TypedComponentSnapshot<AnimationGraphComponent> final : public ComponentSnapshot
        {
          public:
            explicit TypedComponentSnapshot(const AnimationGraphComponent& data)
                : m_Data(data), m_Graph(data.RuntimeGraph)
            {
            }

            void Restore(Entity entity) const override
            {
                if (!entity.HasComponent<AnimationGraphComponent>())
                    entity.AddComponent<AnimationGraphComponent>(m_Data);
                auto& target = entity.GetComponent<AnimationGraphComponent>();
                target = m_Data;
                target.RuntimeGraph = m_Graph;
            }

          private:
            AnimationGraphComponent m_Data;
            Ref<AnimationGraph> m_Graph;
        };

        template<>
        class TypedComponentSnapshot<LODGroupComponent> final : public ComponentSnapshot
        {
          public:
            explicit TypedComponentSnapshot(const LODGroupComponent& data)
                : m_Data(data)
            {
                m_Data.m_GeneratedLODHandles = data.m_GeneratedLODHandles;
                if (!data.m_GeneratedLODHandles.empty())
                {
                    m_Manager = Project::GetAssetManager();
                    for (const auto handle : data.m_GeneratedLODHandles)
                    {
                        auto asset = m_Manager->GetMemoryAsset(handle);
                        if (!asset)
                        {
                            throw std::invalid_argument("Cannot capture a missing generated LOD asset.");
                        }
                        m_Assets.push_back(std::move(asset));
                    }
                }
            }

            void Restore(Entity entity) const override
            {
                // Entity's removal hook unregisters owned LOD assets. The memento
                // retains the actual objects and restores the same handles before
                // any component starts referring to those handles again.
                auto manager = m_Manager;
                for (const auto& asset : m_Assets)
                {
                    manager->AddMemoryOnlyAsset(asset);
                }
                if (!entity.HasComponent<LODGroupComponent>())
                {
                    entity.AddComponent<LODGroupComponent>(m_Data);
                }
                auto& target = entity.GetComponent<LODGroupComponent>();
                target = m_Data;
                target.m_GeneratedLODHandles = m_Data.m_GeneratedLODHandles;
            }

          private:
            LODGroupComponent m_Data;
            Ref<AssetManagerBase> m_Manager;
            std::vector<Ref<Asset>> m_Assets;
        };

        template<typename T>
        std::string ValidateComponentSnapshot(Entity entity)
        {
            if (!entity.HasComponent<T>())
            {
                return {};
            }
            [[maybe_unused]] const auto& data = entity.GetComponent<T>();
            if constexpr (std::is_same_v<T, TerrainComponent>)
            {
                if (data.m_RuntimeCollisionBodyToken != 0)
                {
                    return "TerrainComponent has an active physics body; stop simulation before structural authoring.";
                }
            }
            else if constexpr (std::is_same_v<T, LODGroupComponent>)
            {
                if (!data.m_GeneratedLODHandles.empty())
                {
                    if (!Project::HasAssetManager())
                    {
                        return "LODGroupComponent references generated LOD assets, but no asset manager is available to retain them.";
                    }
                    for (const auto handle : data.m_GeneratedLODHandles)
                    {
                        if (handle == 0 || !AssetManager::GetMemoryAsset(handle))
                        {
                            return "LODGroupComponent references a missing generated memory asset; restore the asset before structural authoring.";
                        }
                    }
                }
            }
            else if constexpr (std::is_same_v<T, AudioSourceComponent>)
            {
                if (data.Source || data.ActiveEventID != 0)
                {
                    return "AudioSourceComponent has active playback state; stop playback before structural authoring.";
                }
            }
            else if constexpr (std::is_same_v<T, AudioSoundGraphComponent>)
            {
                if (data.Sound)
                {
                    return "AudioSoundGraphComponent has active playback state that removal would release.";
                }
            }
            else if constexpr (std::is_same_v<T, NavAgentComponent>)
            {
                if (data.m_CrowdAgentId >= 0 || data.m_HasTarget || data.m_HasPath || !data.m_PathCorners.empty())
                {
                    return "NavAgentComponent has active navigation state that authored snapshots do not capture.";
                }
            }
            else if constexpr (std::is_same_v<T, Rigidbody3DComponent>)
            {
                if (data.m_RuntimeBodyToken != 0)
                {
                    return "Rigidbody3DComponent has an active physics body; stop simulation before structural authoring.";
                }
            }
            else if constexpr (std::is_same_v<T, PhysicsJoint3DComponent>)
            {
                if (data.m_RuntimeConstraintToken != 0)
                {
                    return "PhysicsJoint3DComponent has an active constraint; stop simulation before structural authoring.";
                }
            }
            else if constexpr (std::is_same_v<T, VehicleComponent>)
            {
                if (data.m_RuntimeVehicleToken != 0)
                {
                    return "VehicleComponent has an active constraint; stop simulation before structural authoring.";
                }
            }
            else if constexpr (std::is_same_v<T, RagdollComponent>)
            {
                if (data.m_RuntimeRagdollToken != 0)
                {
                    return "RagdollComponent has an active physics body; stop simulation before structural authoring.";
                }
            }
            else if constexpr (std::is_same_v<T, VideoOverlayComponent> || std::is_same_v<T, VideoSurfaceComponent>)
            {
                if (data.Player)
                {
                    return "Video component has an active player that removal would unload.";
                }
            }
            if constexpr (std::is_same_v<T, SpringBoneComponent>)
            {
                if (entity.HasComponent<SpringBoneStateComponent>())
                {
                    return "Removing SpringBoneComponent would discard uncaptured simulation state.";
                }
            }
            else if constexpr (std::is_same_v<T, NoiseAnimationComponent>)
            {
                if (entity.HasComponent<NoiseAnimationStateComponent>())
                {
                    return "Removing NoiseAnimationComponent would discard uncaptured simulation state.";
                }
            }
            else if constexpr (std::is_same_v<T, LocomotionComponent>)
            {
                if (entity.HasComponent<LocomotionStateComponent>())
                {
                    return "Removing LocomotionComponent would discard uncaptured simulation state.";
                }
            }
            else if constexpr (std::is_same_v<T, FootIKComponent>)
            {
                if (entity.HasComponent<FootIKStateComponent>())
                {
                    return "Removing FootIKComponent would discard uncaptured simulation state.";
                }
            }
            else if constexpr (std::is_same_v<T, RetargetingComponent>)
            {
                if (entity.HasComponent<RetargetingStateComponent>())
                {
                    return "Removing RetargetingComponent would discard uncaptured simulation state.";
                }
            }
            return {};
        }

        template<typename T>
        class ComponentPresenceCommand final : public EditorCommand
        {
          public:
            ComponentPresenceCommand(Ref<Scene> scene, UUID uuid, bool add, std::string name)
                : m_Scene(std::move(scene)), m_UUID(uuid), m_Add(add), m_Name(std::move(name))
            {
                if (!m_Add)
                {
                    const auto entity = m_Scene->TryGetEntityWithUUID(m_UUID);
                    m_Snapshot = std::make_shared<TypedComponentSnapshot<T>>(entity->GetComponent<T>());
                }
            }

            void Execute() override
            {
                auto entity = m_Scene->TryGetEntityWithUUID(m_UUID);
                if (!entity)
                {
                    return;
                }
                if (!m_Add)
                {
                    entity->RemoveComponent<T>();
                }
                else if (m_Snapshot)
                {
                    m_Snapshot->Restore(*entity);
                }
                else
                {
                    auto& data = entity->AddComponent<T>();
                    // Defaults may contain newly generated UUIDs. Capture only
                    // after the first add hook, then reuse the same state on redo.
                    m_Snapshot = std::make_shared<TypedComponentSnapshot<T>>(data);
                }
            }

            void Undo() override
            {
                auto entity = m_Scene->TryGetEntityWithUUID(m_UUID);
                if (!entity)
                {
                    return;
                }
                if (m_Add)
                {
                    entity->RemoveComponent<T>();
                }
                else
                {
                    m_Snapshot->Restore(*entity);
                }
            }

            [[nodiscard]] std::string GetDescription() const override
            {
                return (m_Add ? "Add " : "Remove ") + m_Name;
            }

          private:
            Ref<Scene> m_Scene;
            UUID m_UUID;
            bool m_Add;
            std::string m_Name;
            std::shared_ptr<const ComponentSnapshot> m_Snapshot;
        };

        template<typename T, bool Authored>
        ComponentTypeEntry MakeEntry(const char* name)
        {
            ComponentTypeEntry entry;
            entry.Name = name;
            entry.Authored = Authored;
            entry.Has = [](Entity entity)
            { return entity.HasComponent<T>(); };
            entry.ValidateSnapshot = ValidateComponentSnapshot<T>;

            if constexpr (Authored)
            {
                static_assert(std::is_copy_constructible_v<T> && std::is_copy_assignable_v<T>,
                              "Authored component must support snapshot copying for structural automation");
                static_assert(std::is_default_constructible_v<T>,
                              "Authored component must support default construction for structural automation");
                entry.Capture = [](Entity entity) -> std::shared_ptr<const ComponentSnapshot>
                { return std::make_shared<TypedComponentSnapshot<T>>(entity.GetComponent<T>()); };
                if constexpr (std::is_same_v<T, TransformComponent> || std::is_same_v<T, RelationshipComponent>)
                {
                    entry.RejectionReason = "Core scene structure is managed by entity and hierarchy commands.";
                }
                else if constexpr (std::is_same_v<T, AnimationStateComponent>)
                {
                    entry.RejectionReason = "Animation playback state is managed by the animation system.";
                }
                else
                {
                    entry.Addable = true;
                    entry.Removable = true;
                    entry.MakeAddCommand = [name](Ref<Scene> scene, UUID uuid) -> std::unique_ptr<EditorCommand>
                    { return std::make_unique<ComponentPresenceCommand<T>>(std::move(scene), uuid, true, name); };
                    entry.MakeRemoveCommand = [name](Ref<Scene> scene, UUID uuid) -> std::unique_ptr<EditorCommand>
                    { return std::make_unique<ComponentPresenceCommand<T>>(std::move(scene), uuid, false, name); };
                }
            }
            else
            {
                entry.RejectionReason = "Entity identity and runtime-only components cannot be added or removed directly.";
                if constexpr (!std::is_same_v<T, IDComponent> && !std::is_same_v<T, TagComponent> &&
                              !std::is_same_v<T, WorldTransformComponent> && !std::is_same_v<T, UIResolvedRectComponent>)
                {
                    entry.ValidateSnapshot = [name](Entity entity) -> std::string
                    {
                        return entity.HasComponent<T>()
                                   ? std::string(name) + " contains runtime state that authored entity snapshots do not capture."
                                   : std::string{};
                    };
                }
            }
            return entry;
        }

        std::vector<ComponentTypeEntry> BuildComponentTypes()
        {
            std::vector<ComponentTypeEntry> entries;
            VisitComponentTypes([&entries]<typename T, bool Authored>(const char* name)
                                { entries.push_back(MakeEntry<T, Authored>(name)); });
            return entries;
        }
    } // namespace

    const std::vector<ComponentTypeEntry>& ComponentTypes()
    {
        static const std::vector<ComponentTypeEntry> s_Types = BuildComponentTypes();
        return s_Types;
    }

    const ComponentTypeEntry* FindComponentType(std::string_view name)
    {
        const auto& entries = ComponentTypes();
        const auto found = std::ranges::find(entries, name, &ComponentTypeEntry::Name);
        return found == entries.end() ? nullptr : &*found;
    }

    std::string ValidateEntitySnapshot(Entity entity)
    {
        for (const auto& entry : ComponentTypes())
        {
            if (const auto error = entry.ValidateSnapshot(entity); !error.empty())
            {
                return error;
            }
        }
        return {};
    }

    std::string ValidateEntityDuplication(Entity entity)
    {
        if (const auto error = ValidateEntitySnapshot(entity); !error.empty())
        {
            return error;
        }
        if (entity.HasComponent<TerrainComponent>() && HasMutableTerrainResources(entity.GetComponent<TerrainComponent>()))
        {
            return "TerrainComponent contains mutable sculpt/paint resources that cannot be shared by a duplicate. Remove and destroy remain undoable.";
        }
        if (entity.HasComponent<LODGroupComponent>() && !entity.GetComponent<LODGroupComponent>().m_GeneratedLODHandles.empty())
        {
            return "LODGroupComponent owns generated assets that cannot have two owners. Remove and destroy remain undoable.";
        }
        if (entity.HasComponent<AnimationGraphComponent>() && entity.GetComponent<AnimationGraphComponent>().RuntimeGraph)
        {
            return "AnimationGraphComponent contains a mutable editor graph that cannot be shared by a duplicate. Remove and destroy remain undoable.";
        }
        return {};
    }

    ComponentSnapshots CaptureEntityComponents(Entity entity)
    {
        if (const auto error = ValidateEntitySnapshot(entity); !error.empty())
        {
            throw std::invalid_argument(error);
        }
        ComponentSnapshots snapshots;
        for (const auto& entry : ComponentTypes())
        {
            if (entry.Capture && entry.Has(entity))
            {
                snapshots.push_back(entry.Capture(entity));
            }
        }
        return snapshots;
    }
} // namespace OloEngine::Automation
