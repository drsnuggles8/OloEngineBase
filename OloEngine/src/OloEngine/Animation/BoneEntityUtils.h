#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Scene/Components.h"
#include <vector>
#include <string>
#include <glm/glm.hpp>

namespace OloEngine
{
    // Forward declarations
    class MeshSource;
    class Scene;

    /**
     * @brief Helper utilities for managing bone entities
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
         * @param boneEntityIds The list of bone entity IDs
         * @param meshSource The mesh source containing bone info
         * @param scene The scene containing the entities
         * @return Vector of model-space transforms for each bone
         */
        static std::vector<glm::mat4> GetModelSpaceBoneTransforms(
            const std::vector<UUID>& boneEntityIds,
            const MeshSource* meshSource,
            const class Scene* scene);

        /**
         * @brief Find all bone entity IDs for a given root entity
         * 
         * This function traverses the hierarchy starting from the root entity
         * and collects all bone entity IDs that are part of the skeleton.
         * 
         * @param rootEntity The root entity to start searching from
         * @param skeleton The skeleton data to match against
         * @param scene The scene containing the entities
         * @return Vector of bone entity UUIDs
         */
        static std::vector<UUID> FindBoneEntityIds(
            Entity rootEntity,
            const Skeleton* skeleton,
            const class Scene* scene);

        /**
         * @brief Find all bone entity IDs for a given root entity with skeleton component
         * 
         * @param rootEntity The root entity to start searching from
         * @param skeletonComponent The skeleton component data to match against
         * @param scene The scene containing the entities
         * @return Vector of bone entity UUIDs
         */
        static std::vector<UUID> FindBoneEntityIds(
            Entity rootEntity,
            const SkeletonComponent& skeletonComponent,
            const class Scene* scene);

        /**
         * @brief Calculate the root bone transform for an animated entity
         * 
         * Similar to Hazel's FindRootBoneTransform, this calculates the transform
         * of the animated root bone relative to the entity that owns the animation component.
         * 
         * @param entity The entity owning the animation component
         * @param boneEntityIds The bone entity IDs
         * @param scene The scene containing the entities
         * @return 4x4 matrix representing the full transform of root bone
         */
        static glm::mat4 FindRootBoneTransform(
            Entity entity,
            const std::vector<UUID>& boneEntityIds,
            const class Scene* scene);

        /**
         * @brief Build bone entity IDs for all submeshes in a hierarchy
         * 
         * @param entity The entity containing the mesh component
         * @param rootEntity The root entity of the model hierarchy
         * @param scene The scene containing the entities
         */
        static void BuildMeshBoneEntityIds(
            Entity entity,
            Entity rootEntity,
            const class Scene* scene);

        /**
         * @brief Build bone entity IDs for an animation
         * 
         * @param entity The entity containing the animation component
         * @param rootEntity The root entity of the model hierarchy
         * @param scene The scene containing the entities
         */
        static void BuildAnimationBoneEntityIds(
            Entity entity,
            Entity rootEntity,
            const class Scene* scene);

        /**
         * @brief Find the entity with a specific bone name and index
         * 
         * @param rootEntity The root entity to search from
         * @param boneName The name of the bone to find
         * @param boneIndex The index of the bone to find
         * @param scene The scene containing the entities
         * @return The entity containing the bone, or null entity if not found
         */
        static Entity FindBoneEntity(
            Entity rootEntity,
            const std::string& boneName,
            sizet boneIndex,
            const class Scene* scene);

        /**
         * @brief Create bone entities for a skeleton hierarchy
         * 
         * @param rootEntity The root entity to create bone entities under
         * @param skeleton The skeleton data to create entities from
         * @param scene The scene to create entities in
         */
        static void CreateBoneEntities(
            Entity rootEntity,
            const Skeleton* skeleton,
            class Scene* scene);

        /**
         * @brief Find an entity with a specific tag in the hierarchy
         * 
         * @param entity The entity to start searching from
         * @param tag The tag to search for
         * @param scene The scene containing the entities
         * @return The entity with the tag, or null entity if not found
         */
        static Entity FindEntityWithTag(
            Entity entity,
            const std::string& tag,
            const class Scene* scene);
    };
}
