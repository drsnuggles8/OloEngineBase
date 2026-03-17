#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Animation/BlendNode.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Animation/AnimationClip.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace OloEngine
{
	class BlendTree : public RefCounted
	{
	public:
		enum class BlendType : u8
		{
			Simple1D,
			SimpleDirectional2D,
			FreeformDirectional2D,
			FreeformCartesian2D
		};

		BlendType Type = BlendType::Simple1D;
		std::string BlendParameterX;
		std::string BlendParameterY; // For 2D blending

		struct BlendChild
		{
			Ref<AnimationClip> Clip;
			f32 Threshold = 0.0f;          // Position on 1D axis
			glm::vec2 Position = { 0, 0 }; // Position on 2D blend space
			f32 Speed = 1.0f;
		};

		std::vector<BlendChild> Children;

		void Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
		              sizet boneCount,
		              std::vector<BoneTransform>& outBoneTransforms) const;

		[[nodiscard]] f32 GetDuration(const AnimationParameterSet& params) const;

		// Static blend utilities - used by BlendTree and AnimationStateMachine
		static void SampleClipBoneTransforms(const Ref<AnimationClip>& clip, f32 time,
		                                     sizet boneCount,
		                                     std::vector<BoneTransform>& out);
		static void BlendBoneTransforms(const std::vector<BoneTransform>& a,
		                                const std::vector<BoneTransform>& b,
		                                f32 weight, std::vector<BoneTransform>& out);

	private:
		void Evaluate1D(f32 paramValue, f32 normalizedTime, sizet boneCount,
		                std::vector<BoneTransform>& out) const;
		void Evaluate2D(f32 paramX, f32 paramY, f32 normalizedTime, sizet boneCount,
		                std::vector<BoneTransform>& out) const;
	};
} // namespace OloEngine
