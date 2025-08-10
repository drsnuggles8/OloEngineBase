#include "OloEnginePCH.h"
#include "BoneEntityUtils.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/MeshSource.h"
#include <unordered_map>

namespace OloEngine
{
    std::vector<glm::mat4> BoneEntityUtils::GetModelSpaceBoneTransforms(
        const std::vector<UUID>& boneEntityIds, 
        const MeshSource* meshSource,
        const Scene* scene)
    {
        OLO_CORE_ASSERT(meshSource, "MeshSource pointer cannot be null");
        OLO_CORE_ASSERT(scene, "Scene pointer cannot be null");
        
        std::vector<glm::mat4> boneTransforms(boneEntityIds.size(), glm::mat4(1.0f));

        const Skeleton* skeleton = meshSource->GetSkeleton();
        OLO_CORE_ASSERT(skeleton, "Skeleton pointer cannot be null");

    // Calculate hierarchical bone transforms
    // Use sizet to avoid narrowing issues if sizes exceed u32 range
    const sizet count = std::min(skeleton->m_BoneNames.size(), boneEntityIds.size());
    for (sizet i = 0; i < count; ++i)
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
            if (parentIndex < 0 || static_cast<size_t>(parentIndex) >= boneTransforms.size())
            {
                boneTransforms[i] = localTransform;
            }
            else
            {
                boneTransforms[i] = boneTransforms[static_cast<size_t>(parentIndex)] * localTransform;
            }        }

        return boneTransforms;
    }

    // Helper function to build a tag-to-entity map for O(1) lookups
    static void BuildTagEntityMap(Entity entity, const Scene* scene, std::unordered_map<std::string, Entity>& tagMap)
    {
        if (!entity || !scene)
            return;

        // Check current entity
        if (entity.HasComponent<TagComponent>())
        {
            const auto& tagComponent = entity.GetComponent<TagComponent>();
            tagMap[tagComponent.Tag] = entity;
        }

        // Recursively process children
        for (const auto& childId : entity.Children())
        {
            Entity child = scene->TryGetEntityWithUUID(childId);
            if (child)
            {
                BuildTagEntityMap(child, scene, tagMap);
            }
        }
    }

    // Helper function to build a tag-to-entity UUID map for O(1) lookups
    static void BuildTagEntityMap(Entity entity, const Scene* scene, std::unordered_map<std::string, UUID>& tagMap)
    {
        if (!entity || !scene)
            return;

        // Check current entity
        if (entity.HasComponent<TagComponent>())
        {
            const auto& tagComponent = entity.GetComponent<TagComponent>();
            tagMap[tagComponent.Tag] = entity.GetUUID();
        }

        // Recursively process children
        for (const auto& childId : entity.Children())
        {
            Entity child = scene->TryGetEntityWithUUID(childId);
            if (child)
            {
                BuildTagEntityMap(child, scene, tagMap);
            }
        }
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

        // Build tag-to-entity map once for O(1) lookups
        std::unordered_map<std::string, UUID> tagEntityMap;
        BuildTagEntityMap(rootEntity, scene, tagEntityMap);

        bool foundAtLeastOne = false;
        for (const auto& boneName : boneNames)
        {
            auto it = tagEntityMap.find(boneName);
            if (it != tagEntityMap.end() && it->second != UUID{})
            {
                boneEntityIds.emplace_back(it->second);
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

    std::vector<UUID> BoneEntityUtils::FindBoneEntityIds(
        Entity rootEntity,
        const SkeletonComponent& skeletonComponent,
        const Scene* scene)
    {
        std::vector<UUID> boneEntityIds;

        if (!skeletonComponent.m_Skeleton || !scene || !rootEntity)
            return boneEntityIds;

        const auto& boneNames = skeletonComponent.m_Skeleton->m_BoneNames;
        boneEntityIds.reserve(boneNames.size());

        // Single lock for cache validation, potential rebuild, and reading to ensure atomicity
        bool foundAtLeastOne = false;
        {
            std::lock_guard<std::mutex> lock(skeletonComponent.m_CacheMutex);
            
            // Check if cache is valid, if not rebuild it
            if (!skeletonComponent.m_CacheValid)
            {
                skeletonComponent.m_TagEntityCache.clear();
                BuildTagEntityMap(rootEntity, scene, skeletonComponent.m_TagEntityCache);
                skeletonComponent.m_CacheValid = true;
            }
            
            // Use cached map for O(1) lookups
            for (const auto& boneName : boneNames)
            {
                auto it = skeletonComponent.m_TagEntityCache.find(boneName);
                if (it != skeletonComponent.m_TagEntityCache.end() && it->second != UUID{})
                {
                    boneEntityIds.emplace_back(it->second);
                    foundAtLeastOne = true;
                }
                else
                {
                    boneEntityIds.emplace_back(UUID{}); // Invalid/null UUID as placeholder
                }
            }
        }

        // If no bones were found, clear the array (consistent with other overload)
        if (!foundAtLeastOne)
        {
            boneEntityIds.clear();
        }

        return boneEntityIds;
    }

    glm::mat4 BoneEntityUtils::FindRootBoneTransform(
        Entity entity,
        const std::vector<UUID>& boneEntityIds,
        const Scene* scene)
    {
        if (boneEntityIds.empty() || !scene)
            return glm::mat4(1.0f);

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

        // Return full 4x4 transform matrix
        return transform;
    }

    void BoneEntityUtils::BuildMeshBoneEntityIds(Entity entity, Entity rootEntity, const Scene* scene)
    {
        if (!scene)
            return;
        if (!entity)
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
        for (const auto& childId : entity.Children())
        {
            Entity child = scene->TryGetEntityWithUUID(childId);
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
            const auto& skeletonComponent = entity.GetComponent<SkeletonComponent>();
            
            if (skeletonComponent.m_Skeleton)
            {
                // Use cached version to avoid repeated hierarchy walks
                animComponent.m_BoneEntityIds = FindBoneEntityIds(rootEntity, skeletonComponent, scene);
                animComponent.m_RootBoneTransform = FindRootBoneTransform(entity, animComponent.m_BoneEntityIds, scene);
            }
        }

        // Recursively process children
        for (const auto& childId : entity.Children())
        {
            Entity child = scene->TryGetEntityWithUUID(childId);
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
        for (const auto& childId : entity.Children())
        {
            Entity child = scene->TryGetEntityWithUUID(childId);
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
