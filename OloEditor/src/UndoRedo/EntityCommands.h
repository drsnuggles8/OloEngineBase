#pragma once

#include "EditorCommand.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <functional>

namespace OloEngine
{
    // Undo/Redo for transform changes (gizmo manipulation, panel edits)
    class TransformChangeCommand : public EditorCommand
    {
      public:
        TransformChangeCommand(Ref<Scene> scene, UUID entityUUID,
                               const glm::vec3& oldTranslation, const glm::vec3& oldRotation, const glm::vec3& oldScale,
                               const glm::vec3& newTranslation, const glm::vec3& newRotation, const glm::vec3& newScale)
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID), m_OldTranslation(oldTranslation), m_OldRotation(oldRotation), m_OldScale(oldScale), m_NewTranslation(newTranslation), m_NewRotation(newRotation), m_NewScale(newScale)
        {
        }

        void Execute() override
        {
            ApplyTransform(m_NewTranslation, m_NewRotation, m_NewScale);
        }

        void Undo() override
        {
            ApplyTransform(m_OldTranslation, m_OldRotation, m_OldScale);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Transform Change";
        }

      private:
        void ApplyTransform(const glm::vec3& translation, const glm::vec3& rotation, const glm::vec3& scale) const
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (!entityOpt)
            {
                return;
            }

            auto& tc = entityOpt->GetComponent<TransformComponent>();
            tc.Translation = translation;
            tc.Rotation = rotation;
            tc.Scale = scale;
        }

        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        glm::vec3 m_OldTranslation;
        glm::vec3 m_OldRotation;
        glm::vec3 m_OldScale;
        glm::vec3 m_NewTranslation;
        glm::vec3 m_NewRotation;
        glm::vec3 m_NewScale;
    };

    // Undo/Redo for entity creation
    class CreateEntityCommand : public EditorCommand
    {
      public:
        CreateEntityCommand(Ref<Scene> scene, std::string name,
                            std::function<void(Entity)> onCreated = nullptr,
                            std::function<void()> onDestroyed = nullptr)
            : m_Scene(std::move(scene)), m_Name(std::move(name)), m_OnCreated(std::move(onCreated)), m_OnDestroyed(std::move(onDestroyed))
        {
        }

        void Execute() override
        {
            Entity entity;
            if (m_EntityUUID != UUID(0))
            {
                // Re-create with same UUID on redo
                entity = m_Scene->CreateEntityWithUUID(m_EntityUUID, m_Name);
            }
            else
            {
                entity = m_Scene->CreateEntity(m_Name);
                m_EntityUUID = entity.GetUUID();
            }

            if (m_OnCreated)
            {
                m_OnCreated(entity);
            }
        }

        void Undo() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt)
            {
                m_Scene->DestroyEntity(*entityOpt);
            }

            if (m_OnDestroyed)
            {
                m_OnDestroyed();
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Create Entity '" + m_Name + "'";
        }

        [[nodiscard]] UUID GetEntityUUID() const
        {
            return m_EntityUUID;
        }

      private:
        Ref<Scene> m_Scene;
        std::string m_Name;
        UUID m_EntityUUID{ 0 };
        std::function<void(Entity)> m_OnCreated;
        std::function<void()> m_OnDestroyed;
    };

    // Undo/Redo for entity deletion — snapshots the entity via DuplicateEntity pattern
    class DeleteEntityCommand : public EditorCommand
    {
      public:
        DeleteEntityCommand(Ref<Scene> scene, Entity entity,
                            std::function<void()> onDeleted = nullptr,
                            std::function<void(Entity)> onRestored = nullptr)
            : m_Scene(std::move(scene)), m_EntityUUID(entity.GetUUID()), m_EntityName(entity.GetComponent<TagComponent>().Tag), m_OnDeleted(std::move(onDeleted)), m_OnRestored(std::move(onRestored))
        {
            // Snapshot entity state before deletion
            SnapshotEntity(entity);
        }

        void Execute() override
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt)
            {
                m_Scene->DestroyEntity(*entityOpt);
            }

            if (m_OnDeleted)
            {
                m_OnDeleted();
            }
        }

        void Undo() override
        {
            // Recreate entity with the same UUID
            Entity restored = m_Scene->CreateEntityWithUUID(m_EntityUUID, m_EntityName);

            // Restore all component data from snapshot
            RestoreComponents(restored);

            if (m_OnRestored)
            {
                m_OnRestored(restored);
            }
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Delete Entity '" + m_EntityName + "'";
        }

      private:
        // Store component data by copying each component the entity has
        void SnapshotEntity(Entity entity)
        {
            SnapshotComponentIfExists<TransformComponent>(entity);
            SnapshotComponentIfExists<SpriteRendererComponent>(entity);
            SnapshotComponentIfExists<CircleRendererComponent>(entity);
            SnapshotComponentIfExists<CameraComponent>(entity);
            SnapshotComponentIfExists<PrefabComponent>(entity);
            SnapshotComponentIfExists<Rigidbody2DComponent>(entity);
            SnapshotComponentIfExists<BoxCollider2DComponent>(entity);
            SnapshotComponentIfExists<CircleCollider2DComponent>(entity);
            SnapshotComponentIfExists<Rigidbody3DComponent>(entity);
            SnapshotComponentIfExists<BoxCollider3DComponent>(entity);
            SnapshotComponentIfExists<SphereCollider3DComponent>(entity);
            SnapshotComponentIfExists<CapsuleCollider3DComponent>(entity);
            SnapshotComponentIfExists<MeshCollider3DComponent>(entity);
            SnapshotComponentIfExists<ConvexMeshCollider3DComponent>(entity);
            SnapshotComponentIfExists<TriangleMeshCollider3DComponent>(entity);
            SnapshotComponentIfExists<CharacterController3DComponent>(entity);
            SnapshotComponentIfExists<TextComponent>(entity);
            SnapshotComponentIfExists<ScriptComponent>(entity);
            SnapshotComponentIfExists<AudioSourceComponent>(entity);
            SnapshotComponentIfExists<AudioListenerComponent>(entity);
            SnapshotComponentIfExists<SubmeshComponent>(entity);
            SnapshotComponentIfExists<MeshComponent>(entity);
            SnapshotComponentIfExists<ModelComponent>(entity);
            SnapshotComponentIfExists<AnimationStateComponent>(entity);
            SnapshotComponentIfExists<SkeletonComponent>(entity);
            SnapshotComponentIfExists<MaterialComponent>(entity);
            SnapshotComponentIfExists<DirectionalLightComponent>(entity);
            SnapshotComponentIfExists<PointLightComponent>(entity);
            SnapshotComponentIfExists<SpotLightComponent>(entity);
            SnapshotComponentIfExists<EnvironmentMapComponent>(entity);
            SnapshotComponentIfExists<RelationshipComponent>(entity);
            SnapshotComponentIfExists<ParticleSystemComponent>(entity);
            SnapshotComponentIfExists<TerrainComponent>(entity);
            SnapshotComponentIfExists<FoliageComponent>(entity);
            SnapshotComponentIfExists<WaterComponent>(entity);
            SnapshotComponentIfExists<SnowDeformerComponent>(entity);
            SnapshotComponentIfExists<FogVolumeComponent>(entity);
            SnapshotComponentIfExists<DecalComponent>(entity);
            SnapshotComponentIfExists<LODGroupComponent>(entity);
            SnapshotComponentIfExists<LightProbeComponent>(entity);
            SnapshotComponentIfExists<LightProbeVolumeComponent>(entity);
            SnapshotComponentIfExists<StreamingVolumeComponent>(entity);
            SnapshotComponentIfExists<NetworkIdentityComponent>(entity);
            SnapshotComponentIfExists<DialogueComponent>(entity);
            // UI components
            SnapshotComponentIfExists<UICanvasComponent>(entity);
            SnapshotComponentIfExists<UIRectTransformComponent>(entity);
            SnapshotComponentIfExists<UIImageComponent>(entity);
            SnapshotComponentIfExists<UIPanelComponent>(entity);
            SnapshotComponentIfExists<UITextComponent>(entity);
            SnapshotComponentIfExists<UIButtonComponent>(entity);
            SnapshotComponentIfExists<UISliderComponent>(entity);
            SnapshotComponentIfExists<UICheckboxComponent>(entity);
            SnapshotComponentIfExists<UIProgressBarComponent>(entity);
            SnapshotComponentIfExists<UIInputFieldComponent>(entity);
            SnapshotComponentIfExists<UIScrollViewComponent>(entity);
            SnapshotComponentIfExists<UIDropdownComponent>(entity);
            SnapshotComponentIfExists<UIGridLayoutComponent>(entity);
            SnapshotComponentIfExists<UIToggleComponent>(entity);
        }

        template<typename T>
        void SnapshotComponentIfExists(Entity entity)
        {
            if (entity.HasComponent<T>())
            {
                m_ComponentRestorers.push_back([data = entity.GetComponent<T>()](Entity e)
                                               { e.AddOrReplaceComponent<T>(data); });
            }
        }

        void RestoreComponents(Entity entity) const
        {
            for (const auto& restorer : m_ComponentRestorers)
            {
                restorer(entity);
            }
        }

        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        std::string m_EntityName;
        std::vector<std::function<void(Entity)>> m_ComponentRestorers;
        std::function<void()> m_OnDeleted;
        std::function<void(Entity)> m_OnRestored;
    };

    // Undo/Redo for entity renaming
    class RenameEntityCommand : public EditorCommand
    {
      public:
        RenameEntityCommand(Ref<Scene> scene, UUID entityUUID,
                            std::string oldName, std::string newName)
            : m_Scene(std::move(scene)), m_EntityUUID(entityUUID), m_OldName(std::move(oldName)), m_NewName(std::move(newName))
        {
        }

        void Execute() override
        {
            SetName(m_NewName);
        }

        void Undo() override
        {
            SetName(m_OldName);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Rename Entity";
        }

      private:
        void SetName(const std::string& name) const
        {
            auto entityOpt = m_Scene->TryGetEntityWithUUID(m_EntityUUID);
            if (entityOpt)
            {
                entityOpt->GetComponent<TagComponent>().Tag = name;
            }
        }

        Ref<Scene> m_Scene;
        UUID m_EntityUUID;
        std::string m_OldName;
        std::string m_NewName;
    };

    // Wraps a DeleteEntityCommand with swapped Execute/Undo for use with duplicate undo.
    // When the user duplicates an entity, the clone already exists.
    // Undo = delete the clone (inner Execute), Redo = restore the clone (inner Undo).
    class DuplicateUndoCommand : public EditorCommand
    {
      public:
        explicit DuplicateUndoCommand(std::unique_ptr<DeleteEntityCommand> inner)
            : m_Inner(std::move(inner))
        {
        }

        void Execute() override
        {
            m_Inner->Undo();
        }
        void Undo() override
        {
            m_Inner->Execute();
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Duplicate Entity";
        }

      private:
        std::unique_ptr<DeleteEntityCommand> m_Inner;
    };

    // Generic swapped Execute/Undo wrapper for any EditorCommand.
    // Used when entities already exist (e.g. paste) and undo should delete them.
    class InvertedCommand : public EditorCommand
    {
      public:
        explicit InvertedCommand(std::unique_ptr<EditorCommand> inner)
            : m_Inner(std::move(inner))
        {
        }

        void Execute() override
        {
            m_Inner->Undo();
        }
        void Undo() override
        {
            m_Inner->Execute();
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return m_Inner->GetDescription();
        }

      private:
        std::unique_ptr<EditorCommand> m_Inner;
    };

    // Undo/Redo for entity reparenting (drag-and-drop in hierarchy)
    class ReparentEntityCommand : public EditorCommand
    {
      public:
        ReparentEntityCommand(Ref<Scene> scene, UUID childUUID, UUID oldParentUUID, UUID newParentUUID)
            : m_Scene(std::move(scene)), m_ChildUUID(childUUID), m_OldParentUUID(oldParentUUID), m_NewParentUUID(newParentUUID)
        {
        }

        void Execute() override
        {
            ApplyParent(m_NewParentUUID);
        }

        void Undo() override
        {
            ApplyParent(m_OldParentUUID);
        }

        [[nodiscard]] std::string GetDescription() const override
        {
            return "Reparent Entity";
        }

      private:
        void ApplyParent(UUID parentUUID) const
        {
            auto childOpt = m_Scene->TryGetEntityWithUUID(m_ChildUUID);
            if (!childOpt)
            {
                return;
            }

            if (parentUUID == UUID(0))
            {
                // Unparent: remove from current parent
                Entity currentParent = childOpt->GetParent();
                if (currentParent)
                {
                    currentParent.RemoveChild(*childOpt);
                }
            }
            else
            {
                auto parentOpt = m_Scene->TryGetEntityWithUUID(parentUUID);
                if (parentOpt)
                {
                    childOpt->SetParent(*parentOpt);
                }
            }
        }

        Ref<Scene> m_Scene;
        UUID m_ChildUUID;
        UUID m_OldParentUUID;
        UUID m_NewParentUUID;
    };
} // namespace OloEngine
