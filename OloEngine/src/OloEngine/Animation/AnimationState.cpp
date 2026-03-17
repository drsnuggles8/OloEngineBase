#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Renderer/AnimatedModel.h"

namespace OloEngine
{
	void AnimationState::Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
	                              sizet boneCount,
	                              std::vector<BoneTransform>& outBoneTransforms) const
	{
		switch (Type)
		{
		case MotionType::SingleClip:
		{
			if (!Clip)
			{
				outBoneTransforms.resize(boneCount);
				return;
			}
			f32 time = normalizedTime * Clip->Duration;
			outBoneTransforms.resize(boneCount);
			for (sizet i = 0; i < boneCount; ++i)
			{
				if (i < Clip->BoneAnimations.size())
				{
					auto const& boneAnim = Clip->BoneAnimations[i];
					outBoneTransforms[i].Translation = AnimatedModel::SampleBonePosition(boneAnim.PositionKeys, time);
					outBoneTransforms[i].Rotation = AnimatedModel::SampleBoneRotation(boneAnim.RotationKeys, time);
					outBoneTransforms[i].Scale = AnimatedModel::SampleBoneScale(boneAnim.ScaleKeys, time);
				}
			}
			break;
		}
		case MotionType::BlendTree:
		{
			if (!Tree)
			{
				outBoneTransforms.resize(boneCount);
				return;
			}
			Tree->Evaluate(normalizedTime, params, boneCount, outBoneTransforms);
			break;
		}
		}
	}

	f32 AnimationState::GetClipDuration() const
	{
		if (Type == MotionType::SingleClip && Clip)
		{
			return Clip->Duration / Speed;
		}
		return 0.0f;
	}

	f32 AnimationState::GetEffectiveDuration(const AnimationParameterSet& params) const
	{
		if (Type == MotionType::SingleClip && Clip)
		{
			return Clip->Duration / Speed;
		}
		if (Type == MotionType::BlendTree && Tree)
		{
			return Tree->GetDuration(params) / Speed;
		}
		return 0.0f;
	}
} // namespace OloEngine
