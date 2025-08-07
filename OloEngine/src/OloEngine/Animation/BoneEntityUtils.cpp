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
        Scene* scene)
    {
        std::vector<glm::mat4> boneTransforms(boneEntityIds.size());
        
        if (!meshSource || !scene)
            return boneTransforms;

        const Skeleton* skeleton = meshSource->GetSkeleton();
        if (!skeleton)
            return boneTransforms;

        // Calculate hierarchical bone transforms (similar to Hazel's approach)
        for (u32 i = 0; i < std::min(static_cast<u32>(skeleton->m_BoneNames.size()), static_cast<u32>(boneEntityIds.size())); ++i)
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
        Scene* scene)
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
                boneEntityIds.emplace_back(UUID(0)); // Invalid UUID as placeholder
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
        Scene* scene)
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

    void BoneEntityUtils::BuildMeshBoneEntityIds(Entity entity, Entity rootEntity, Scene* scene)
    {
        if (!scene)
            return;

        // Process current entity if it has a SubmeshComponent
        if (entity.HasComponent<SubmeshComponent>())
        {
            auto& submeshComponent = entity.GetComponent<SubmeshComponent>();
            if (submeshComponent.Mesh && submeshComponent.Mesh->GetMeshSource())
            {
                const Skeleton* skeleton = submeshComponent.Mesh->GetMeshSource()->GetSkeleton();
                if (skeleton)
                {
                    submeshComponent.BoneEntityIds = FindBoneEntityIds(rootEntity, skeleton, scene);
                }
            }
        }

        // Recursively process children
        for (auto childId : entity.Children())
        {
            Entity child = scene->GetEntityWithUUID(childId);
            BuildMeshBoneEntityIds(child, rootEntity, scene);
        }
    }

    void BoneEntityUtils::BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity, Scene* scene)
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
                animComponent.BoneEntityIds = FindBoneEntityIds(rootEntity, skeletonComponent.m_Skeleton.get(), scene);
                animComponent.RootBoneTransform = FindRootBoneTransform(entity, animComponent.BoneEntityIds, scene);
            }
        }

        // Recursively process children
        for (auto childId : entity.Children())
        {
            Entity child = scene->GetEntityWithUUID(childId);
            BuildAnimationBoneEntityIds(child, rootEntity, scene);
        }
    }

    Entity BoneEntityUtils::FindEntityWithTag(Entity entity, const std::string& tag, Scene* scene)
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
            Entity found = FindEntityWithTag(child, tag, scene);
            if (found)
                return found;
        }

        return Entity(); // Not found
    }
}
