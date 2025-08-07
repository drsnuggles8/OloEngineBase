#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Animation/Skeleton.h"
#include <vector>
#include <glm/glm.hpp>

namespace OloEngine
{
    /**
     * @brief Helper utilities for managing bone entities in Hazel-style animation
     * 
     * These utilities help with creating and managing the bone entity hierarchies
     * that represent skeleton bones as actual scene entities.
     */
    class BoneEntityUtils
    {
    public:
        /**
         * @brief Calculate model-space bone transforms from scene entities
         * 
         * Similar to Hazel's GetModelSpaceBoneTransforms, this function takes
         * a list of bone entity IDs and calculates their model-space transforms
         * for use in skeletal animation.
         * 
         * @param boneEntityIds List of entity UUIDs representing bones
         * @param meshSource The mesh source containing skeleton data
         * @param scene The scene containing the entities
         * @return Vector of model-space bone transform matrices
         */
        static std::vector<glm::mat4> GetModelSpaceBoneTransforms(
            const std::vector<UUID>& boneEntityIds, 
            Ref<MeshSource> meshSource,
            class Scene* scene);

        /**
         * @brief Find bone entities by traversing the entity hierarchy
         * 
         * Similar to Hazel's FindBoneEntityIds, this function searches the entity
         * hierarchy to find entities with tags that match the skeleton bone names.
         * 
         * @param rootEntity The root entity to start searching from
         * @param skeleton The skeleton containing bone names to search for
         * @param scene The scene containing the entities
         * @return Vector of entity UUIDs matching the skeleton bones
         */
        static std::vector<UUID> FindBoneEntityIds(
            Entity rootEntity,
            const Skeleton* skeleton,
            class Scene* scene);

        /**
         * @brief Calculate the transform of the root bone relative to the entity
         * 
         * Similar to Hazel's FindRootBoneTransform, this calculates the transform
         * of the animated root bone relative to the entity that owns the animation component.
         * 
         * @param entity The entity owning the animation component
         * @param boneEntityIds The bone entity IDs
         * @param scene The scene containing the entities
         * @return 3x3 matrix representing rotation and scale of root bone
         */
        static glm::mat3 FindRootBoneTransform(
            Entity entity,
            const std::vector<UUID>& boneEntityIds,
            class Scene* scene);

        /**
         * @brief Build bone entity IDs for all submeshes in a hierarchy
         * 
         * Recursively traverses an entity hierarchy and builds bone entity mappings
         * for all SubmeshComponents found. Similar to Hazel's BuildMeshBoneEntityIds.
         * 
         * @param entity The root entity to start from
         * @param rootEntity The root entity for bone searching
         * @param scene The scene containing the entities
         */
        static void BuildMeshBoneEntityIds(Entity entity, Entity rootEntity, class Scene* scene);

        /**
         * @brief Build bone entity IDs for animation components
         * 
         * Recursively traverses an entity hierarchy and builds bone entity mappings
         * for all AnimationStateComponents found. Similar to Hazel's BuildAnimationBoneEntityIds.
         * 
         * @param entity The root entity to start from
         * @param rootEntity The root entity for bone searching
         * @param scene The scene containing the entities
         */
        static void BuildAnimationBoneEntityIds(Entity entity, Entity rootEntity, class Scene* scene);

    private:
        /**
         * @brief Helper function to search for entities with specific tags
         * 
         * @param entity The entity to search within
         * @param tag The tag to search for
         * @param scene The scene containing the entities
         * @return The found entity or an invalid entity if not found
         */
        static Entity FindEntityWithTag(Entity entity, const std::string& tag, class Scene* scene);
    };
}
