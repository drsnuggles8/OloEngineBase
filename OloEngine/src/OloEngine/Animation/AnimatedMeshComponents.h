#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Assert.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "SkeletonData.h"
#include "Skeleton.h"
#include "AnimationClip.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

namespace OloEngine
{
    // Forward declarations
    class Mesh;
    class MeshSource;
    class Skeleton;

    /**
     * @brief Component for entities that represent individual submeshes
     *
     * This component is attached to entities that represent individual submeshes within a mesh hierarchy.
     * For rigged meshes, the m_BoneEntityIds field maps skeleton bones to scene entities.
     */
    struct SubmeshComponent
    {
        Ref<Mesh> m_Mesh;
        std::vector<UUID> m_BoneEntityIds; // Maps skeleton bones to scene entities
        u32 m_SubmeshIndex = 0;
        bool m_Visible = true;

        SubmeshComponent() = default;
        SubmeshComponent(const SubmeshComponent& other) = default;
        explicit SubmeshComponent(const Ref<OloEngine::Mesh>& mesh)
            : SubmeshComponent(mesh, 0) {}
        explicit SubmeshComponent(const Ref<OloEngine::Mesh>& mesh, u32 submeshIndex)
            : m_Mesh(mesh), m_SubmeshIndex(submeshIndex)
        {
            // Validate mesh reference
            OLO_CORE_ASSERT(m_Mesh, "Mesh reference cannot be null!");

            // Validate that mesh has a valid MeshSource
            OLO_CORE_ASSERT(m_Mesh->GetMeshSource(), "Mesh MeshSource is null!");
        }

        // Manual operator== — UUID elements would trigger C2666 with the
        // default vector== implementation. Compare UUIDs via u64.
        auto operator==(const SubmeshComponent& other) const -> bool
        {
            auto boneCount = m_BoneEntityIds.size();
            if (m_Mesh != other.m_Mesh || m_SubmeshIndex != other.m_SubmeshIndex || m_Visible != other.m_Visible || boneCount != other.m_BoneEntityIds.size())
                return false;
            for (sizet i = 0; i < boneCount; ++i)
            {
                if (static_cast<u64>(m_BoneEntityIds[i]) != static_cast<u64>(other.m_BoneEntityIds[i]))
                    return false;
            }
            return true;
        }
    };

    enum class MeshPrimitive : i32
    {
        None = 0,
        Cube = 1,
        Sphere = 2,
        Plane = 3,
        Cylinder = 4,
        Cone = 5,
        Icosphere = 6,
        Torus = 7
    };

    /**
     * @brief Component for the root entity of a dynamic mesh
     *
     * This tags the root entity of a mesh hierarchy.
     * Child entities with SubmeshComponent represent the individual submeshes.
     */
    struct MeshComponent
    {
        Ref<MeshSource> m_MeshSource;
        MeshPrimitive m_Primitive = MeshPrimitive::None;

        MeshComponent() = default;
        explicit MeshComponent(const Ref<OloEngine::MeshSource>& meshSource) noexcept : m_MeshSource(meshSource) {}

        auto operator==(const MeshComponent&) const -> bool = default;
    };

    /**
     * @brief Component for entities with a fully loaded 3D model
     *
     * This component stores a complete Model with all its meshes, materials,
     * and textures loaded from a file. Use this for importing external 3D
     * model files (OBJ, FBX, GLTF, etc.) with their materials intact.
     *
     * Unlike MeshComponent which only stores raw mesh data, ModelComponent
     * provides full material and texture support from the source file.
     */
    struct ModelComponent
    {
        Ref<Model> m_Model;
        std::string m_FilePath; // Original file path for serialization/reload
        bool m_Visible = true;

        ModelComponent() = default;
        explicit ModelComponent(const std::string& filePath)
            : m_FilePath(filePath)
        {
            if (!filePath.empty())
            {
                m_Model = Ref<Model>::Create(filePath);
            }
        }
        explicit ModelComponent(const Ref<Model>& model, const std::string& filePath = "")
            : m_Model(model), m_FilePath(filePath) {}

        // Reload the model from the stored file path
        void Reload()
        {
            if (!m_FilePath.empty())
            {
                m_Model = Ref<Model>::Create(m_FilePath);
            }
        }

        [[nodiscard("load state must be checked before rendering")]] bool IsLoaded() const
        {
            return m_Model != nullptr && m_Model->GetMeshCount() > 0;
        }

        auto operator==(const ModelComponent&) const -> bool = default;
    };

    /**
     * @brief Animation state component for managing animation playback
     *
     * This component manages the current animation state, including blending
     * between animations and the animation state machine.
     */
    struct AnimationStateComponent
    {
        // Animation state machine (expand as needed)
        enum class State
        {
            Idle,
            Bounce,
            Custom
        };

        State m_State = State::Idle;
        Ref<AnimationClip> m_CurrentClip;
        Ref<AnimationClip> m_NextClip;                    // For blending
        std::vector<Ref<AnimationClip>> m_AvailableClips; // All available animation clips from the model
        int m_CurrentClipIndex = 0;                       // Index into m_AvailableClips
        float m_CurrentTime = 0.0f;
        float m_NextTime = 0.0f;
        float m_BlendFactor = 0.0f; // 0 = current, 1 = next
        bool m_Blending = false;
        float m_BlendDuration = 0.3f; // seconds
        float m_BlendTime = 0.0f;
        bool m_IsPlaying = false;     // Whether animation is currently playing
        std::string m_SourceFilePath; // Path to the animated model file for serialization/reload

        // Bone entity management
        /**
         * @brief Global skeleton-to-entity mapping used across all submeshes
         *
         * This vector holds the complete mapping from skeleton bones to scene entities,
         * populated during mesh loading. Each index corresponds to a bone in the skeleton,
         * and the UUID value represents the entity that visualizes that bone in the scene.
         *
         * Note: Individual SubmeshComponent instances contain submesh-local bone indices
         * that reference this global list, set up during submesh initialization.
         *
         * Warning: Any modifications to bones require synchronized updates to both this
         * vector and the corresponding SubmeshComponent::m_BoneEntityIds to maintain consistency.
         */
        std::vector<UUID> m_BoneEntityIds;               // Maps skeleton bones to scene entities
        glm::mat4 m_RootBoneTransform = glm::mat4(1.0f); // Transform of animated root bone relative to entity

        // Runtime, per-tick (not serialized): the masked root-motion delta the
        // last animation update extracted, in entity/model space. Overwritten
        // every update; consumed and cleared by Scene::UpdateRootMotion on the
        // runtime path (issue #631). In editor preview nothing consumes it — the
        // pose is pinned in place and the entity stays put.
        glm::vec3 m_RootMotionTranslation = glm::vec3(0.0f);
        glm::quat m_RootMotionRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        bool m_HasRootMotion = false;

        AnimationStateComponent() = default;
        explicit AnimationStateComponent(const Ref<AnimationClip>& clip, float timeSeconds = 0.0f)
            : m_CurrentClip(clip), m_CurrentTime(timeSeconds) {}
    };

    /**
     * @brief Component that holds skeleton reference for an entity
     *
     * This component links an entity to a skeleton. Unlike the old approach where
     * the skeleton was part of the mesh, this allows for skeleton sharing and
     * better entity-based bone management.
     */
    struct SkeletonComponent
    {
        Ref<Skeleton> m_Skeleton; // Shared skeleton reference

        SkeletonComponent() = default;
        explicit SkeletonComponent(const Ref<Skeleton>& skeleton) : m_Skeleton(skeleton) {}
        ~SkeletonComponent() = default;

        // Custom copy constructor - mutex cannot be copied
        SkeletonComponent(const SkeletonComponent& other)
            : m_Skeleton(other.m_Skeleton)
        {
            // Thread-safe copy of cache data
            TUniqueLock lock(other.m_CacheMutex);
            m_TagEntityCache = other.m_TagEntityCache;
            m_CacheValid = other.m_CacheValid;
            // Each component gets its own mutex
        }

        // Custom assignment operator - mutex cannot be assigned
        SkeletonComponent& operator=(const SkeletonComponent& other)
        {
            if (this != &other)
            {
                // Thread-safe assignment with deadlock avoidance using address-based ordering
                // Always lock the mutex at the lower address first to prevent ABBA deadlocks
                FMutex* first = &m_CacheMutex;
                FMutex* second = &other.m_CacheMutex;
                if (second < first)
                {
                    std::swap(first, second);
                }
                TUniqueLock lock1(*first);
                TUniqueLock lock2(*second);
                m_Skeleton = other.m_Skeleton;
                m_TagEntityCache = other.m_TagEntityCache;
                m_CacheValid = other.m_CacheValid;
                // Keep existing mutex
            }
            return *this;
        }

        // Move constructor - transfer ownership efficiently
        SkeletonComponent(SkeletonComponent&& other) noexcept
            : m_Skeleton(std::move(other.m_Skeleton))
        {
            // Transfer cache data under lock from the source
            TUniqueLock lock(other.m_CacheMutex);
            m_TagEntityCache = std::move(other.m_TagEntityCache);
            m_CacheValid = other.m_CacheValid;
            other.m_CacheValid = false; // Invalidate source cache
                                        // Note: mutex is not moved - each component gets its own mutex
        }

        // Move assignment operator - transfer ownership efficiently
        SkeletonComponent& operator=(SkeletonComponent&& other) noexcept
        {
            if (this != &other)
            {
                // Thread-safe move with deadlock avoidance using address-based ordering
                // Always lock the mutex at the lower address first to prevent ABBA deadlocks
                FMutex* first = &m_CacheMutex;
                FMutex* second = &other.m_CacheMutex;
                if (second < first)
                {
                    std::swap(first, second);
                }
                TUniqueLock lock1(*first);
                TUniqueLock lock2(*second);
                m_Skeleton = std::move(other.m_Skeleton);
                m_TagEntityCache = std::move(other.m_TagEntityCache);
                m_CacheValid = other.m_CacheValid;
                other.m_CacheValid = false; // Invalidate source cache
            }
            return *this;
        }

        // Invalidate cache when skeleton changes
        void InvalidateCache() const noexcept
        {
            TUniqueLock lock(m_CacheMutex);
            m_CacheValid = false;
            m_TagEntityCache.clear();
        }

        // Thread-safe setter for skeleton that automatically invalidates cache
        void SetSkeleton(const Ref<Skeleton>& skeleton) noexcept
        {
            TUniqueLock lock(m_CacheMutex);
            m_Skeleton = skeleton;
            m_CacheValid = false;
            m_TagEntityCache.clear();
        }

        // Resolve the given bone names to scene-entity UUIDs using the tag cache.
        // Cache validation, rebuild, and read all happen under a single lock; when
        // the cache is invalid, rebuildCache(map) is invoked (still under the lock)
        // to repopulate it. Encapsulating the access here lets the mutable cache
        // members stay private and guarded by m_CacheMutex.
        template<typename RebuildFn>
        [[nodiscard("resolved bone entity IDs must be used")]] std::vector<UUID> ResolveBoneEntities(
            const std::vector<std::string>& boneNames, RebuildFn&& rebuildCache) const
        {
            std::vector<UUID> boneEntityIds;
            boneEntityIds.reserve(boneNames.size());

            bool foundAtLeastOne = false;
            {
                TUniqueLock lock(m_CacheMutex);
                if (!m_CacheValid)
                {
                    m_TagEntityCache.clear();
                    std::forward<RebuildFn>(rebuildCache)(m_TagEntityCache);
                    m_CacheValid = true;
                }

                for (const auto& boneName : boneNames)
                {
                    auto it = m_TagEntityCache.find(boneName);
                    if (it != m_TagEntityCache.end() && it->second != UUID{})
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

            // If no bones were found, clear the array (consistent with the other overload)
            if (!foundAtLeastOne)
            {
                boneEntityIds.clear();
            }

            return boneEntityIds;
        }

      private:
        mutable FMutex m_CacheMutex;                                    // Protects cache members from concurrent access
        mutable std::unordered_map<std::string, UUID> m_TagEntityCache; // Cache for tag-to-entity UUID mapping
        mutable bool m_CacheValid = false;                              // Whether the cache is still valid
    };

    /**
     * @brief Component Usage Guide for Animation
     *
     * MeshComponent: Root entity that holds the MeshSource
     * - Attached to the main entity representing the entire mesh
     * - References the MeshSource that contains all submeshes and skeleton data
     *
     * SubmeshComponent: Individual submesh entities
     * - Child entities have this component to represent individual submeshes
     * - For rigged meshes, m_BoneEntityIds maps skeleton bones to scene entities
     * - This allows direct manipulation of bones as scene entities
     *
     * AnimationStateComponent: Animation playback and state
     * - Manages current animation clip, blending, and timing
     * - Also contains m_BoneEntityIds for cases where animation affects multiple submeshes
     *
     * SkeletonComponent: Skeleton reference
     * - Links an entity to its skeleton
     * - Allows for skeleton sharing between entities
     *
     * Entity Hierarchy Example:
     * CharacterEntity (AnimationStateComponent, SkeletonComponent, MeshComponent)
     *   ├── Body (SubmeshComponent with m_BoneEntityIds)
     *   ├── Head (SubmeshComponent with m_BoneEntityIds)
     *   └── BoneRoot
     *       ├── Spine (TransformComponent - represents bone)
     *       ├── LeftArm (TransformComponent - represents bone)
     *       └── RightArm (TransformComponent - represents bone)
     *
     * Key Benefits:
     * - Bones are real scene entities that can be manipulated directly
     * - Editor integration: bones appear in scene hierarchy
     * - Flexible material and rendering system
     * - Same rendering pipeline for static and animated content
     * - Easy bone visualization and debugging
     */
} // namespace OloEngine
