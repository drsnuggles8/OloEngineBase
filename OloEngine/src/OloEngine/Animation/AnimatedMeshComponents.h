#pragma once

#include "OloEngine/Core/Base.h"
#include "SkeletonData.h"

namespace OloEngine
{
	// Forward declarations
	class Mesh;
	class SkinnedMesh;
	class Skeleton;
	class AnimationClip;

	// Holds mesh, skeleton, and skinning data for an entity
	struct AnimatedMeshComponent
	{
		Ref<SkinnedMesh> m_Mesh;
		Ref<Skeleton> m_Skeleton;
		// Skinning data (bone weights/indices) is part of SkinnedMesh

		AnimatedMeshComponent() = default;
		AnimatedMeshComponent(const Ref<SkinnedMesh>& mesh, const Ref<Skeleton>& skeleton)
			: m_Mesh(mesh), m_Skeleton(skeleton) {}
	};

	// Holds current animation state (clip, time, blend info)


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
		float CurrentTime = 0.0f;
		float NextTime = 0.0f;
		float BlendFactor = 0.0f; // 0 = current, 1 = next
		bool Blending = false;
		float BlendDuration = 0.3f; // seconds
		float BlendTime = 0.0f;

		AnimationStateComponent() = default;
		AnimationStateComponent(const Ref<AnimationClip>& clip, float time = 0.0f)
			: m_CurrentClip(clip), CurrentTime(time) {}
	};

	// Holds bone hierarchy and transforms for an entity
	struct SkeletonComponent
	{
		SkeletonData skeleton; // Shared skeleton data structure
		
		SkeletonComponent() = default;
		SkeletonComponent(size_t boneCount) : skeleton(boneCount) {}
	};

	/**
	 * @brief Animation Components Usage and Integration Guide
	 * 
	 * These components are designed to work together within the ECS architecture:
	 * 
	 * AnimatedMeshComponent: Contains the mesh and skeleton references
	 * - Add to entities that need skeletal animation
	 * - Links to skinned mesh assets loaded through the asset pipeline
	 * 
	 * AnimationStateComponent: Manages animation playback state
	 * - Handles current animation, blending, and state machine logic
	 * - Updated by AnimationSystem each frame
	 * 
	 * SkeletonComponent: Stores bone hierarchy and transform data
	 * - Contains bone names, transforms, and skinning matrices
	 * - Updated by animation sampling and forwarded to GPU
	 * 
	 * Integration with Asset Pipeline:
	 * - Load skeletal meshes and animations through AssetManager
	 * - Attach components to entities during scene creation
	 * - Use AnimationSystem to update all animated entities
	 */
}
