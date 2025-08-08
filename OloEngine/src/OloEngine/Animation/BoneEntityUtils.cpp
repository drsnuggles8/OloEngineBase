#include "OloEnginePCH.h"
#include "BoneEntityUtils.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/MeshSource.h"

namespace OloEngine
{
    std::vector<glm::mat4> BoneEntityUtils::GetModelSpaceBoneTransforms(
        const std::vector<UUID>& boneEntityIds, 
        MeshSource* meshSource,
        const Scene* scene)
    {
        OLO_CORE_ASSERT(meshSource, "MeshSource pointer cannot be null");
        OLO_CORE_ASSERT(scene, "Scene pointer cannot be null");
        
        std::vector<glm::mat4> boneTransforms(boneEntityIds.size());

        const Skeleton* skeleton = meshSource->GetSkeleton();
        OLO_CORE_ASSERT(skeleton, "Skeleton pointer cannot be null");

    // Calculate hierarchical bone transforms
    // Use size_t to avoid narrowing issues if sizes exceed u32 range
    const size_t count = std::min(skeleton->m_BoneNames.size(), boneEntityIds.size());
    for (size_t i = 0; i < count; ++i)
        {
            Entity boneEntity = scene->TryGetEntityWithUUID(boneEntityIds[i]);
            
            // Get transform from entity or use rest pose as fallback
            glm::mat4 localTransform;
            if (boneEntity && boneEntity.HasComponent<TransformComponent>())
            {
                localTransform = boneEntity.GetComponent<TransformComponent>().GetTransform();
            }
            else
            {
                // Fallback to skeleton rest pose
                localTransform = skeleton->m_LocalTransforms[i];
            }

            // Calculate model space transform by multiplying with parent
            int parentIndex = skeleton->m_ParentIndices[i];
            if (parentIndex == -1)
            {
                boneTransforms[i] = localTransform;
            }
            else
            {
                boneTransforms[i] = boneTransforms[parentIndex] * localTransform;
            }
        }

        return boneTransforms;
    }

    std::vector<UUID> BoneEntityUtils::FindBoneEntityIds(
        Entity rootEntity,
        const Skeleton* skeleton,
        const Scene* scene)
    {
        std::vector<UUID> boneEntityIds;

        if (!skeleton || !scene || !rootEntity)
            return boneEntityIds;

        const auto& boneNames = skeleton->m_BoneNames;
        boneEntityIds.reserve(boneNames.size());

        bool foundAtLeastOne = false;
        for (const auto& boneName : boneNames)
        {
            Entity boneEntity = FindEntityWithTag(rootEntity, boneName, scene);
            if (boneEntity)
            {
                boneEntityIds.emplace_back(boneEntity.GetUUID());
                foundAtLeastOne = true;
            }
            else
            {
                boneEntityIds.emplace_back(UUID{}); // Invalid/null UUID as placeholder
            }
        }

        // If no bones were found, clear the array
        if (!foundAtLeastOne)
        {
            boneEntityIds.clear();
        }

        return boneEntityIds;
    }

    glm::mat3 BoneEntityUtils::FindRootBoneTransform(
        Entity entity,
        const std::vector<UUID>& boneEntityIds,
        const Scene* scene)
    {
        if (boneEntityIds.empty() || !scene)
            return glm::mat3(1.0f);

        glm::mat4 transform = glm::mat4(1.0f);
        Entity rootBoneEntity = scene->TryGetEntityWithUUID(boneEntityIds.front());
        
        if (rootBoneEntity)
        {
            Entity parentEntity = rootBoneEntity.GetParent();
            while (parentEntity && parentEntity != entity)
            {
                if (parentEntity.HasComponent<TransformComponent>())
                {
                    transform = parentEntity.GetComponent<TransformComponent>().GetTransform() * transform;
                }
                parentEntity = parentEntity.GetParent();
            }
        }

        // Return only rotation and scale components
        return glm::mat3(transform);
    }

    void BoneEntityUtils::BuildMeshBoneEntityIds(Entity entity, Entity rootEntity, const Scene* scene)
    {
        if (!scene)
            return;

        // Process current entity if it has a SubmeshComponent
        if (entity.HasComponent<SubmeshComponent>())
        {
            auto& submeshComponent = entity.GetComponent<SubmeshComponent>();
            if (submeshComponent.m_Mesh && submeshComponent.m_Mesh->GetMeshSource())
            {
                const Skeleton* skeleton = submeshComponent.m_Mesh->GetMeshSource()->GetSkeleton();
                if (skeleton)
                {
                    submeshComponent.m_BoneEntityIds = FindBoneEntityIds(rootEntity, skeleton, scene);
                }
            }
        }

        // Recursively process children
        for (auto childId : entity.Children())
        {
            Entity child = scene->GetEntityWithUUID(childId);
            if (child) // Check if entity is valid before recursive call
            {
                BuildMeshBoneEntityIds(child, rootEntity, scene);
            }
        }
    }

    void BoneEntityUtils::BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity, const Scene* scene)
    {
        if (!scene)
            return;

        // Process current entity if it has an AnimationStateComponent
        if (entity.HasComponent<AnimationStateComponent>() && entity.HasComponent<SkeletonComponent>())
        {
            auto& animComponent = entity.GetComponent<AnimationStateComponent>();
            auto& skeletonComponent = entity.GetComponent<SkeletonComponent>();
            
            if (skeletonComponent.m_Skeleton)
            {
                animComponent.m_BoneEntityIds = FindBoneEntityIds(rootEntity, skeletonComponent.m_Skeleton.get(), scene);
                animComponent.m_RootBoneTransform = FindRootBoneTransform(entity, animComponent.m_BoneEntityIds, scene);
            }
        }

        // Recursively process children
        for (auto childId : entity.Children())
        {
            Entity child = scene->GetEntityWithUUID(childId);
            if (child) // Check if entity is valid before recursive call
            {
                BuildAnimationBoneEntityIds(child, rootEntity, scene);
            }
        }
    }

    Entity BoneEntityUtils::FindEntityWithTag(Entity entity, const std::string& tag, const Scene* scene)
    {
        if (!entity || !scene)
            return Entity();

        // Check current entity
        if (entity.HasComponent<TagComponent>())
        {
            const auto& tagComponent = entity.GetComponent<TagComponent>();
            if (tagComponent.Tag == tag)
                return entity;
        }

        // Recursively search children
        for (auto childId : entity.Children())
        {
            Entity child = scene->GetEntityWithUUID(childId);
            if (child) // Check if entity is valid before recursive call
            {
                Entity found = FindEntityWithTag(child, tag, scene);
                if (found)
                    return found;
            }
        }

        return Entity(); // Not found
    }
}
