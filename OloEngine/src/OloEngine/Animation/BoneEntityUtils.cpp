#include "OloEnginePCH.h"
#include "BoneEntityUtils.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Renderer/MeshSource.h"
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "OloEngine/Threading/UniqueLock.h"

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

        // Build tag-to-entity map once for O(1) lookups
        std::unordered_map<std::string, UUID> tagEntityMap;
        std::unordered_set<UUID> visited;

        // Build tag map with cycle detection
        std::function<void(Entity)> buildTagMap = [&scene, &visited, &tagEntityMap, &buildTagMap](Entity entity)
        {
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
        if (!skeletonComponent.m_Skeleton || !scene || !rootEntity)
            return {};

        const auto& boneNames = skeletonComponent.m_Skeleton->m_BoneNames;

        // The component owns the cache and its mutex; it (re)builds the tag map
        // under its own lock and invokes this callback only when a rebuild is
        // needed. The traversal stays here because it needs the Scene.
        return skeletonComponent.ResolveBoneEntities(
            boneNames,
            [&scene, rootEntity](std::unordered_map<std::string, UUID>& tagEntityCache)
            {
                std::unordered_set<UUID> visited;

                // Build tag map with cycle detection
                std::function<void(Entity)> buildTagMap = [&scene, &visited, &tagEntityCache, &buildTagMap](Entity entity)
                {
                    if (!entity || !scene)
                        return;

                    UUID entityUUID = entity.GetUUID();
                    if (visited.find(entityUUID) != visited.end())
                        return;

                    visited.insert(entityUUID);

                    if (entity.HasComponent<TagComponent>())
                    {
                        const auto& tagComponent = entity.GetComponent<TagComponent>();
                        tagEntityCache[tagComponent.Tag] = entity.GetUUID();
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
            });
    }

    glm::mat4 BoneEntityUtils::FindRootBoneTransform(
        Entity entity,
        const std::vector<UUID>& boneEntityIds,
        const Scene* scene)
    {
        if (boneEntityIds.empty() || !scene)
            return glm::mat4(1.0f);

        glm::mat4 transform = glm::mat4(1.0f);
        if (auto rootBoneEntityOpt = scene->TryGetEntityWithUUID(boneEntityIds.front()))
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

    // Internal helper with cycle detection (iterative DFS to avoid recursion)
    static void BuildMeshBoneEntityIdsImpl(Entity entity, Entity rootEntity, Scene* scene, std::unordered_set<UUID>& visited)
    {
        if (!scene || !entity)
            return;

        std::vector<Entity> stack;
        stack.push_back(entity);

        while (!stack.empty())
        {
            Entity current = stack.back();
            stack.pop_back();
            if (!current)
                continue;

            // Cycle detection — skip if this entity was already visited
            if (!visited.insert(current.GetUUID()).second)
                continue;

            // Process current entity if it has a SubmeshComponent
            if (current.HasComponent<SubmeshComponent>())
            {
                auto& submeshComponent = current.GetComponent<SubmeshComponent>();
                if (submeshComponent.m_Mesh && submeshComponent.m_Mesh->GetMeshSource())
                {
                    const Skeleton* skeleton = submeshComponent.m_Mesh->GetMeshSource()->GetSkeleton();
                    if (skeleton)
                    {
                        submeshComponent.m_BoneEntityIds = BoneEntityUtils::FindBoneEntityIds(rootEntity, skeleton, scene);
                    }
                }
            }

            // Queue children in reverse so the first child is processed next — preserves pre-order DFS
            const auto& children = current.Children();
            for (auto it = children.rbegin(); it != children.rend(); ++it)
            {
                auto childOpt = scene->TryGetEntityWithUUID(*it);
                if (childOpt)
                {
                    stack.push_back(*childOpt);
                }
            }
        }
    }

    void BoneEntityUtils::BuildMeshBoneEntityIds(Entity entity, Entity rootEntity, Scene* scene)
    {
        std::unordered_set<UUID> visited;
        BuildMeshBoneEntityIdsImpl(entity, rootEntity, scene, visited);
    }

    // Internal helper with cycle detection (iterative DFS to avoid recursion)
    static void BuildAnimationBoneEntityIdsImpl(Entity entity, Entity rootEntity, Scene* scene, std::unordered_set<UUID>& visited)
    {
        if (!scene || !entity)
            return;

        std::vector<Entity> stack;
        stack.push_back(entity);

        while (!stack.empty())
        {
            Entity current = stack.back();
            stack.pop_back();
            if (!current)
                continue;

            // Cycle detection — skip if this entity was already visited
            if (!visited.insert(current.GetUUID()).second)
                continue;

            // Process current entity if it has an AnimationStateComponent
            if (current.HasComponent<AnimationStateComponent>() && current.HasComponent<SkeletonComponent>())
            {
                auto& animComponent = current.GetComponent<AnimationStateComponent>();
                const auto& skeletonComponent = current.GetComponent<SkeletonComponent>();

                if (skeletonComponent.m_Skeleton)
                {
                    // Use cached version to avoid repeated hierarchy walks
                    animComponent.m_BoneEntityIds = BoneEntityUtils::FindBoneEntityIds(rootEntity, skeletonComponent, scene);
                    animComponent.m_RootBoneTransform = BoneEntityUtils::FindRootBoneTransform(current, animComponent.m_BoneEntityIds, scene);
                }
            }

            // Queue children in reverse so the first child is processed next — preserves pre-order DFS
            const auto& children = current.Children();
            for (auto it = children.rbegin(); it != children.rend(); ++it)
            {
                auto childOpt = scene->TryGetEntityWithUUID(*it);
                if (childOpt)
                {
                    stack.push_back(*childOpt);
                }
            }
        }
    }

    void BoneEntityUtils::BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity, Scene* scene)
    {
        std::unordered_set<UUID> visited;
        BuildAnimationBoneEntityIdsImpl(entity, rootEntity, scene, visited);
    }

    // Internal helper with cycle detection (iterative pre-order DFS to avoid recursion)
    static Entity FindEntityWithTagImpl(Entity entity, const std::string& tag, const Scene* scene, std::unordered_set<UUID>& visited)
    {
        if (!entity || !scene)
            return Entity();

        std::vector<Entity> stack;
        stack.push_back(entity);

        while (!stack.empty())
        {
            Entity current = stack.back();
            stack.pop_back();
            if (!current)
                continue;

            // Cycle detection — skip if this entity was already visited
            if (!visited.insert(current.GetUUID()).second)
                continue;

            // Check current entity
            if (current.HasComponent<TagComponent>())
            {
                const auto& tagComponent = current.GetComponent<TagComponent>();
                if (tagComponent.Tag == tag)
                    return current;
            }

            // Queue children in reverse so the first child is searched next — preserves pre-order DFS
            const auto& children = current.Children();
            for (auto it = children.rbegin(); it != children.rend(); ++it)
            {
                auto childOpt = scene->TryGetEntityWithUUID(*it);
                if (childOpt)
                {
                    stack.push_back(*childOpt);
                }
            }
        }

        return Entity(); // Not found
    }

    Entity BoneEntityUtils::FindEntityWithTag(Entity entity, const std::string& tag, const Scene* scene)
    {
        std::unordered_set<UUID> visited;
        return FindEntityWithTagImpl(entity, tag, scene, visited);
    }
} // namespace OloEngine
