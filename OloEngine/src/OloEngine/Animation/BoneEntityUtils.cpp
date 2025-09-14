#include "OloEnginePCH.h"
#include "BoneEntityUtils.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/MeshSource.h"
#include <algorithm>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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
            auto boneEntityOpt = scene->TryGetEntityWithUUID(boneEntityIds[i]);
            
            // Get transform from entity or use rest pose as fallback
            glm::mat4 localTransform;
            if (boneEntityOpt && boneEntityOpt->HasComponent<TransformComponent>())
            {
                localTransform = boneEntityOpt->GetComponent<TransformComponent>().GetTransform();
            }
            else
            {
                // Fallback to skeleton rest pose - with bounds checking
                if (i < skeleton->m_LocalTransforms.size())
                {
                    localTransform = skeleton->m_LocalTransforms[i];
                }
                else
                {
                    OLO_CORE_WARN("BoneEntityUtils::GetModelSpaceBoneTransforms: Bone index {} exceeds skeleton local transforms size {}", i, skeleton->m_LocalTransforms.size());
                    localTransform = glm::mat4(1.0f); // Identity matrix fallback
                }
            }

            // Calculate model space transform by multiplying with parent - with bounds checking
            int parentIndex = (i < skeleton->m_ParentIndices.size()) ? skeleton->m_ParentIndices[i] : -1;
            if (parentIndex < 0 || static_cast<sizet>(parentIndex) >= boneTransforms.size())
            {
                boneTransforms[i] = localTransform;
            }
            else
            {
                boneTransforms[i] = boneTransforms[static_cast<sizet>(parentIndex)] * localTransform;
            }        }

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

        // Build tag-to-entity map once for O(1) lookups
        std::unordered_map<std::string, UUID> tagEntityMap;
        std::unordered_set<UUID> visited;
        
        // Build tag map with cycle detection
        std::function<void(Entity)> buildTagMap = [&](Entity entity) {
            if (!entity || !scene)
                return;

            UUID entityUUID = entity.GetUUID();
            if (visited.find(entityUUID) != visited.end())
                return;

            visited.insert(entityUUID);

            if (entity.HasComponent<TagComponent>())
            {
                const auto& tagComponent = entity.GetComponent<TagComponent>();
                tagEntityMap[tagComponent.Tag] = entity.GetUUID();
            }

            for (const auto& childId : entity.Children())
            {
                auto childOpt = scene->TryGetEntityWithUUID(childId);
                if (childOpt)
                {
                    buildTagMap(*childOpt);
                }
            }
        };
        
        buildTagMap(rootEntity);

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
                std::unordered_set<UUID> visited;
                
                // Build tag map with cycle detection
                std::function<void(Entity)> buildTagMap = [&](Entity entity) {
                    if (!entity || !scene)
                        return;

                    UUID entityUUID = entity.GetUUID();
                    if (visited.find(entityUUID) != visited.end())
                        return;

                    visited.insert(entityUUID);

                    if (entity.HasComponent<TagComponent>())
                    {
                        const auto& tagComponent = entity.GetComponent<TagComponent>();
                        skeletonComponent.m_TagEntityCache[tagComponent.Tag] = entity.GetUUID();
                    }

                    for (const auto& childId : entity.Children())
                    {
                        auto childOpt = scene->TryGetEntityWithUUID(childId);
                        if (childOpt)
                        {
                            buildTagMap(*childOpt);
                        }
                    }
                };
                
                buildTagMap(rootEntity);
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
        auto rootBoneEntityOpt = scene->TryGetEntityWithUUID(boneEntityIds.front());
        
        if (rootBoneEntityOpt)
        {
            std::unordered_set<UUID> visitedParents; // Track visited entities to prevent cycles
            Entity parentEntity = rootBoneEntityOpt->GetParent();
            
            while (parentEntity && parentEntity != entity)
            {
                // Check for cycles - if this parent was already visited, break to prevent infinite loop
                UUID parentUUID = parentEntity.GetUUID();
                if (visitedParents.find(parentUUID) != visitedParents.end())
                    break;
                
                // Mark this parent as visited
                visitedParents.insert(parentUUID);
                
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

    // Internal helper with cycle detection
    static void BuildMeshBoneEntityIdsImpl(Entity entity, Entity rootEntity, Scene* scene, std::unordered_set<UUID>& visited)
    {
        if (!scene)
            return;
        if (!entity)
            return;

        // Check for cycles - if this entity was already visited, skip to prevent infinite recursion
        UUID entityUUID = entity.GetUUID();
        if (visited.find(entityUUID) != visited.end())
            return;

        // Mark this entity as visited
        visited.insert(entityUUID);

        // Process current entity if it has a SubmeshComponent
        if (entity.HasComponent<SubmeshComponent>())
        {
            auto& submeshComponent = entity.GetComponent<SubmeshComponent>();
            if (submeshComponent.m_Mesh && submeshComponent.m_Mesh->GetMeshSource())
            {
                const Skeleton* skeleton = submeshComponent.m_Mesh->GetMeshSource()->GetSkeleton();
                if (skeleton)
                {
                    submeshComponent.m_BoneEntityIds = BoneEntityUtils::FindBoneEntityIds(rootEntity, skeleton, scene);
                }
            }
        }

        // Recursively process children
        for (const auto& childId : entity.Children())
        {
            auto childOpt = scene->TryGetEntityWithUUID(childId);
            if (childOpt) // Check if entity is valid before recursive call
            {
                BuildMeshBoneEntityIdsImpl(*childOpt, rootEntity, scene, visited);
            }
        }
    }

    void BoneEntityUtils::BuildMeshBoneEntityIds(Entity entity, Entity rootEntity, Scene* scene)
    {
        std::unordered_set<UUID> visited;
        BuildMeshBoneEntityIdsImpl(entity, rootEntity, scene, visited);
    }

    // Internal helper with cycle detection
    static void BuildAnimationBoneEntityIdsImpl(Entity entity, Entity rootEntity, Scene* scene, std::unordered_set<UUID>& visited)
    {
        if (!scene)
            return;
        if (!entity)
            return;

        // Check for cycles - if this entity was already visited, skip to prevent infinite recursion
        UUID entityUUID = entity.GetUUID();
        if (visited.find(entityUUID) != visited.end())
            return;

        // Mark this entity as visited
        visited.insert(entityUUID);

        // Process current entity if it has an AnimationStateComponent
        if (entity.HasComponent<AnimationStateComponent>() && entity.HasComponent<SkeletonComponent>())
        {
            auto& animComponent = entity.GetComponent<AnimationStateComponent>();
            const auto& skeletonComponent = entity.GetComponent<SkeletonComponent>();
            
            if (skeletonComponent.m_Skeleton)
            {
                // Use cached version to avoid repeated hierarchy walks
                animComponent.m_BoneEntityIds = BoneEntityUtils::FindBoneEntityIds(rootEntity, skeletonComponent, scene);
                animComponent.m_RootBoneTransform = BoneEntityUtils::FindRootBoneTransform(entity, animComponent.m_BoneEntityIds, scene);
            }
        }

        // Recursively process children
        for (const auto& childId : entity.Children())
        {
            auto childOpt = scene->TryGetEntityWithUUID(childId);
            if (childOpt) // Check if entity is valid before recursive call
            {
                BuildAnimationBoneEntityIdsImpl(*childOpt, rootEntity, scene, visited);
            }
        }
    }

    void BoneEntityUtils::BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity, Scene* scene)
    {
        std::unordered_set<UUID> visited;
        BuildAnimationBoneEntityIdsImpl(entity, rootEntity, scene, visited);
    }

    // Internal helper with cycle detection
    static Entity FindEntityWithTagImpl(Entity entity, const std::string& tag, const Scene* scene, std::unordered_set<UUID>& visited)
    {
        if (!entity || !scene)
            return Entity();

        // Check for cycles - if this entity was already visited, skip to prevent infinite recursion
        UUID entityUUID = entity.GetUUID();
        if (visited.find(entityUUID) != visited.end())
            return Entity();

        // Mark this entity as visited
        visited.insert(entityUUID);

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
            auto childOpt = scene->TryGetEntityWithUUID(childId);
            if (childOpt) // Check if entity is valid before recursive call
            {
                Entity found = FindEntityWithTagImpl(*childOpt, tag, scene, visited);
                if (found)
                    return found;
            }
        }

        return Entity(); // Not found
    }

    Entity BoneEntityUtils::FindEntityWithTag(Entity entity, const std::string& tag, const Scene* scene)
    {
        std::unordered_set<UUID> visited;
        return FindEntityWithTagImpl(entity, tag, scene, visited);
    }
}
