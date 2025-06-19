#pragma once

#include "OloEngine/Core/Base.h"
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine
{
	// Forward declarations
	class Mesh;
	class Skeleton;
	class AnimationClip;

	// Holds mesh, skeleton, and skinning data for an entity
	struct AnimatedMeshComponent
	{
		Ref<Mesh> m_Mesh;
		Ref<Skeleton> m_Skeleton;
		// Skinning data (bone weights/indices) is assumed to be part of Mesh

		AnimatedMeshComponent() = default;
		AnimatedMeshComponent(const Ref<Mesh>& mesh, const Ref<Skeleton>& skeleton)
			: m_Mesh(mesh), m_Skeleton(skeleton) {}
	};

	// Holds current animation state (clip, time, blend info)
	struct AnimationStateComponent
	{
		Ref<AnimationClip> m_CurrentClip;
		float m_CurrentTime = 0.0f;
		float m_BlendFactor = 0.0f;
		// TODO: Add blend target, state machine info, etc.

		AnimationStateComponent() = default;
		AnimationStateComponent(const Ref<AnimationClip>& clip, float time = 0.0f)
			: m_CurrentClip(clip), m_CurrentTime(time) {}
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

		SkeletonComponent() = default;
		SkeletonComponent(size_t boneCount)
		{
			m_ParentIndices.resize(boneCount);
			m_BoneNames.resize(boneCount);
			m_LocalTransforms.resize(boneCount, glm::mat4(1.0f));
			m_GlobalTransforms.resize(boneCount, glm::mat4(1.0f));
			m_FinalBoneMatrices.resize(boneCount, glm::mat4(1.0f));
		}
	};

	// TODO: Document usage and integration with ECS/asset pipeline
}
