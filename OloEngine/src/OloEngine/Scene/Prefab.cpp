#include "OloEnginePCH.h"
#include "Prefab.h"

#include "Scene.h"
#include "Components.h"

namespace OloEngine
{
    Entity Prefab::CreatePrefabFromEntity(Entity entity)
    {
        OLO_CORE_ASSERT(GetHandle() != 0, "Prefab handle must be set before creating prefab from entity");

        Entity newEntity = m_Scene->CreateEntity();

        // Add PrefabComponent
        newEntity.AddComponent<PrefabComponent>(GetHandle(), newEntity.GetComponent<IDComponent>().ID);

        // Copy all components from source entity (excluding IDComponent)
        // TagComponent
        if (entity.HasComponent<TagComponent>())
        {
            newEntity.AddOrReplaceComponent<TagComponent>(entity.GetComponent<TagComponent>());
        }

        // TransformComponent  
        if (entity.HasComponent<TransformComponent>())
        {
            newEntity.AddOrReplaceComponent<TransformComponent>(entity.GetComponent<TransformComponent>());
        }

        // SpriteRendererComponent
        if (entity.HasComponent<SpriteRendererComponent>())
        {
            newEntity.AddOrReplaceComponent<SpriteRendererComponent>(entity.GetComponent<SpriteRendererComponent>());
        }

        // CircleRendererComponent
        if (entity.HasComponent<CircleRendererComponent>())
        {
            newEntity.AddOrReplaceComponent<CircleRendererComponent>(entity.GetComponent<CircleRendererComponent>());
        }

        // CameraComponent
        if (entity.HasComponent<CameraComponent>())
        {
            newEntity.AddOrReplaceComponent<CameraComponent>(entity.GetComponent<CameraComponent>());
        }

        // ScriptComponent
        if (entity.HasComponent<ScriptComponent>())
        {
            newEntity.AddOrReplaceComponent<ScriptComponent>(entity.GetComponent<ScriptComponent>());
        }

        // TextComponent
        if (entity.HasComponent<TextComponent>())
        {
            newEntity.AddOrReplaceComponent<TextComponent>(entity.GetComponent<TextComponent>());
        }

        // MaterialComponent
        if (entity.HasComponent<MaterialComponent>())
        {
            newEntity.AddOrReplaceComponent<MaterialComponent>(entity.GetComponent<MaterialComponent>());
        }

        // MeshComponent
        if (entity.HasComponent<MeshComponent>())
        {
            newEntity.AddOrReplaceComponent<MeshComponent>(entity.GetComponent<MeshComponent>());
        }

        // SubmeshComponent
        if (entity.HasComponent<SubmeshComponent>())
        {
            newEntity.AddOrReplaceComponent<SubmeshComponent>(entity.GetComponent<SubmeshComponent>());
        }

        // SkeletonComponent
        if (entity.HasComponent<SkeletonComponent>())
        {
            newEntity.AddOrReplaceComponent<SkeletonComponent>(entity.GetComponent<SkeletonComponent>());
        }

        // AnimationStateComponent
        if (entity.HasComponent<AnimationStateComponent>())
        {
            newEntity.AddOrReplaceComponent<AnimationStateComponent>(entity.GetComponent<AnimationStateComponent>());
        }

        // Rigidbody2DComponent
        if (entity.HasComponent<Rigidbody2DComponent>())
        {
            newEntity.AddOrReplaceComponent<Rigidbody2DComponent>(entity.GetComponent<Rigidbody2DComponent>());
        }

        // BoxCollider2DComponent
        if (entity.HasComponent<BoxCollider2DComponent>())
        {
            newEntity.AddOrReplaceComponent<BoxCollider2DComponent>(entity.GetComponent<BoxCollider2DComponent>());
        }

        // CircleCollider2DComponent
        if (entity.HasComponent<CircleCollider2DComponent>())
        {
            newEntity.AddOrReplaceComponent<CircleCollider2DComponent>(entity.GetComponent<CircleCollider2DComponent>());
        }

        // AudioSourceComponent
        if (entity.HasComponent<AudioSourceComponent>())
        {
            newEntity.AddOrReplaceComponent<AudioSourceComponent>(entity.GetComponent<AudioSourceComponent>());
        }

        // AudioListenerComponent
        if (entity.HasComponent<AudioListenerComponent>())
        {
            newEntity.AddOrReplaceComponent<AudioListenerComponent>(entity.GetComponent<AudioListenerComponent>());
        }

        // RelationshipComponent
        if (entity.HasComponent<RelationshipComponent>())
        {
            newEntity.AddOrReplaceComponent<RelationshipComponent>(entity.GetComponent<RelationshipComponent>());
        }

        return newEntity;
    }

    Prefab::Prefab()
        : m_Scene(nullptr), m_Entity{}
    {
        m_Scene = Scene::Create();
    }

    Prefab::~Prefab() = default;

    void Prefab::Create(Entity entity, bool serialize)
    {
        // Create new scene for the prefab
        m_Scene = Scene::Create();
        m_Entity = CreatePrefabFromEntity(entity);

        // TODO: Implement serialization when supported
        (void)serialize;
    }

    std::pair<std::unordered_set<AssetHandle>, std::unordered_set<AssetHandle>> 
        Prefab::GetAssetList(bool recursive)
    {
        std::unordered_set<AssetHandle> assets;
        std::unordered_set<AssetHandle> missingAssets;

        if (!m_Scene)
        {
            return { assets, missingAssets };
        }

        // TODO: Implement asset dependency scanning
        (void)recursive;
        
        return { assets, missingAssets };
    }

    Entity Prefab::Instantiate(Scene& targetScene, UUID uuid) const
    {
        if (!m_Scene || !m_Entity)
        {
            OLO_CORE_ERROR("Prefab::Instantiate - Prefab has no valid scene or entity");
            return {};
        }

        // Generate UUID if not provided
        if (!uuid)
            uuid = UUID();

        // Get the name from the prefab entity
        std::string entityName = "Prefab Instance";
        if (m_Entity.HasComponent<TagComponent>())
        {
            entityName = m_Entity.GetComponent<TagComponent>().Tag;
        }

        // Create new entity in target scene
        Entity targetEntity = targetScene.CreateEntityWithUUID(uuid, entityName);
        if (!targetEntity)
        {
            OLO_CORE_ERROR("Prefab::Instantiate - Failed to create entity in target scene");
            return {};
        }

        // Copy all components from prefab entity to target entity
        CopyEntityComponents(m_Entity, targetEntity);

        return targetEntity;
    }

    void Prefab::CopyEntityComponents(Entity sourceEntity, Entity targetEntity) const
    {
        // Skip IDComponent and TagComponent as they're already set in CreateEntityWithUUID

        // Copy TransformComponent (always present, but copy to preserve transform data)
        if (sourceEntity.HasComponent<TransformComponent>())
        {
            auto& sourceTransform = sourceEntity.GetComponent<TransformComponent>();
            auto& targetTransform = targetEntity.GetComponent<TransformComponent>();
            targetTransform = sourceTransform;
        }

        // Copy all other component types using the same logic as CreatePrefabFromEntity
        if (sourceEntity.HasComponent<CameraComponent>())
        {
            targetEntity.AddOrReplaceComponent<CameraComponent>(sourceEntity.GetComponent<CameraComponent>());
        }

        if (sourceEntity.HasComponent<ScriptComponent>())
        {
            targetEntity.AddOrReplaceComponent<ScriptComponent>(sourceEntity.GetComponent<ScriptComponent>());
        }

        if (sourceEntity.HasComponent<SpriteRendererComponent>())
        {
            targetEntity.AddOrReplaceComponent<SpriteRendererComponent>(sourceEntity.GetComponent<SpriteRendererComponent>());
        }

        if (sourceEntity.HasComponent<CircleRendererComponent>())
        {
            targetEntity.AddOrReplaceComponent<CircleRendererComponent>(sourceEntity.GetComponent<CircleRendererComponent>());
        }

        if (sourceEntity.HasComponent<MeshComponent>())
        {
            targetEntity.AddOrReplaceComponent<MeshComponent>(sourceEntity.GetComponent<MeshComponent>());
        }

        if (sourceEntity.HasComponent<SubmeshComponent>())
        {
            targetEntity.AddOrReplaceComponent<SubmeshComponent>(sourceEntity.GetComponent<SubmeshComponent>());
        }

        if (sourceEntity.HasComponent<SkeletonComponent>())
        {
            targetEntity.AddOrReplaceComponent<SkeletonComponent>(sourceEntity.GetComponent<SkeletonComponent>());
        }

        if (sourceEntity.HasComponent<AnimationStateComponent>())
        {
            targetEntity.AddOrReplaceComponent<AnimationStateComponent>(sourceEntity.GetComponent<AnimationStateComponent>());
        }

        if (sourceEntity.HasComponent<MaterialComponent>())
        {
            targetEntity.AddOrReplaceComponent<MaterialComponent>(sourceEntity.GetComponent<MaterialComponent>());
        }

        if (sourceEntity.HasComponent<Rigidbody2DComponent>())
        {
            targetEntity.AddOrReplaceComponent<Rigidbody2DComponent>(sourceEntity.GetComponent<Rigidbody2DComponent>());
        }

        if (sourceEntity.HasComponent<BoxCollider2DComponent>())
        {
            targetEntity.AddOrReplaceComponent<BoxCollider2DComponent>(sourceEntity.GetComponent<BoxCollider2DComponent>());
        }

        if (sourceEntity.HasComponent<CircleCollider2DComponent>())
        {
            targetEntity.AddOrReplaceComponent<CircleCollider2DComponent>(sourceEntity.GetComponent<CircleCollider2DComponent>());
        }

        if (sourceEntity.HasComponent<TextComponent>())
        {
            targetEntity.AddOrReplaceComponent<TextComponent>(sourceEntity.GetComponent<TextComponent>());
        }

        if (sourceEntity.HasComponent<AudioSourceComponent>())
        {
            targetEntity.AddOrReplaceComponent<AudioSourceComponent>(sourceEntity.GetComponent<AudioSourceComponent>());
        }

        if (sourceEntity.HasComponent<AudioListenerComponent>())
        {
            targetEntity.AddOrReplaceComponent<AudioListenerComponent>(sourceEntity.GetComponent<AudioListenerComponent>());
        }

        if (sourceEntity.HasComponent<RelationshipComponent>())
        {
            targetEntity.AddOrReplaceComponent<RelationshipComponent>(sourceEntity.GetComponent<RelationshipComponent>());
        }
    }
}
