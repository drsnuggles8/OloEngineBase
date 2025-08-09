#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <glm/glm.hpp>
#include "SkeletonData.h"
#include "Skeleton.h"
#include "AnimationClip.h"

#include <vector>
#include <unordered_map>

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
	 * For rigged meshes, the BoneEntityIds field maps skeleton bones to scene entities.
	 */
	struct SubmeshComponent
	{
		Ref<Mesh> m_Mesh;
		std::vector<UUID> m_BoneEntityIds; // Maps skeleton bones to scene entities
		u32 m_SubmeshIndex = 0;
		bool m_Visible = true;

		SubmeshComponent() = default;
		SubmeshComponent(const SubmeshComponent& other) = default;
		explicit SubmeshComponent(Ref<OloEngine::Mesh> mesh, u32 submeshIndex = 0) : m_Mesh(mesh), m_SubmeshIndex(submeshIndex) {}
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
		
		MeshComponent() = default;
		explicit MeshComponent(Ref<OloEngine::MeshSource> meshSource) : m_MeshSource(meshSource) {}
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
		Ref<AnimationClip> m_NextClip; // For blending
		float m_CurrentTime = 0.0f;
		float m_NextTime = 0.0f;
		float m_BlendFactor = 0.0f; // 0 = current, 1 = next
		bool m_Blending = false;
		float m_BlendDuration = 0.3f; // seconds
		float m_BlendTime = 0.0f;
		
		// Bone entity management
		std::vector<UUID> m_BoneEntityIds; // Maps skeleton bones to scene entities
		glm::mat3 m_RootBoneTransform = glm::mat3(1.0f); // Transform of animated root bone relative to entity

		AnimationStateComponent() = default;
		AnimationStateComponent(const Ref<AnimationClip>& clip, float time = 0.0f)
			: m_CurrentClip(clip), m_CurrentTime(time) {}
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
		mutable std::unordered_map<std::string, UUID> m_TagEntityCache; // Cache for tag-to-entity UUID mapping
		mutable bool m_CacheValid = false; // Whether the cache is still valid
		
		SkeletonComponent() = default;
		SkeletonComponent(const Ref<Skeleton>& skeleton) : m_Skeleton(skeleton) {}
		
		// Invalidate cache when skeleton changes
		void InvalidateCache() const { 
			m_CacheValid = false; 
			m_TagEntityCache.clear(); 
		}
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
	 * - For rigged meshes, BoneEntityIds maps skeleton bones to scene entities
	 * - This allows direct manipulation of bones as scene entities
	 * 
	 * AnimationStateComponent: Animation playback and state
	 * - Manages current animation clip, blending, and timing
	 * - Also contains BoneEntityIds for cases where animation affects multiple submeshes
	 * 
	 * SkeletonComponent: Skeleton reference
	 * - Links an entity to its skeleton
	 * - Allows for skeleton sharing between entities
	 * 
	 * Entity Hierarchy Example:
	 * CharacterEntity (AnimationStateComponent, SkeletonComponent, MeshComponent)
	 *   ├── Body (SubmeshComponent with BoneEntityIds)
	 *   ├── Head (SubmeshComponent with BoneEntityIds)
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
}
