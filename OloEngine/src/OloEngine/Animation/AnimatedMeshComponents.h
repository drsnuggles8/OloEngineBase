#pragma once

#include "OloEngine/Core/Base.h"

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "OloEngine/Animation/AnimationClip.h"

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
		// Bone hierarchy (indices, parent indices, names)
		std::vector<int> m_ParentIndices;
		std::vector<std::string> m_BoneNames;
		// Local and global transforms for each bone
		std::vector<glm::mat4> m_LocalTransforms;
		std::vector<glm::mat4> m_GlobalTransforms;
		// Final matrices for skinning (to be sent to GPU)
		std::vector<glm::mat4> m_FinalBoneMatrices;
		
		// Bind pose data for proper skinning
		std::vector<glm::mat4> m_BindPoseMatrices;      // Original bind pose global transforms
		std::vector<glm::mat4> m_InverseBindPoses;      // Inverse bind pose matrices for skinning

		SkeletonComponent() = default;
		SkeletonComponent(size_t boneCount)
		{
			m_ParentIndices.resize(boneCount);
			m_BoneNames.resize(boneCount);
			m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
			m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
			m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
			m_BindPoseMatrices.resize(boneCount, glm::mat4(1.0f));
			m_InverseBindPoses.resize(boneCount, glm::mat4(1.0f));
		}
		
		// Initialize bind pose from current global transforms
		void SetBindPose()
		{
			for (size_t i = 0; i < m_GlobalTransforms.size(); ++i)
			{
				m_BindPoseMatrices[i] = m_GlobalTransforms[i];
				m_InverseBindPoses[i] = glm::inverse(m_GlobalTransforms[i]);
			}
		}
	};

	// TODO: Document usage and integration with ECS/asset pipeline
}
