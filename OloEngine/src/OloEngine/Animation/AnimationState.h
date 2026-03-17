#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendTree.h"
#include "OloEngine/Animation/BlendNode.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include <string>
#include <vector>

namespace OloEngine
{
	class AnimationState
	{
	public:
		std::string Name;
		f32 Speed = 1.0f;
		bool Looping = true;

		enum class MotionType : u8
		{
			SingleClip,
			BlendTree
		};

		MotionType Type = MotionType::SingleClip;

		// For SingleClip
		Ref<AnimationClip> Clip;

		// For BlendTree
		Ref<BlendTree> Tree;

		void Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
		              sizet boneCount,
		              std::vector<BoneTransform>& outBoneTransforms) const;

		[[nodiscard]] f32 GetClipDuration() const;
		[[nodiscard]] f32 GetEffectiveDuration(const AnimationParameterSet& params) const;
	};
} // namespace OloEngine
