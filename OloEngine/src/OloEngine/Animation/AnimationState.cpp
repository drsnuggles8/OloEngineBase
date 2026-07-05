#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationState.h"

namespace OloEngine
{
    void AnimationState::Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
                                  sizet boneCount, const PoseEvalContext& ctx,
                                  std::vector<BoneTransform>& outBoneTransforms) const
    {
        OLO_PROFILE_FUNCTION();

        switch (Type)
        {
            case MotionType::SingleClip:
            {
                if (!Clip)
                {
                    BlendTree::FillBindPose(ctx, boneCount, outBoneTransforms);
                    return;
                }
                // Sample by bone NAME with a bind-pose fallback (issue #543);
                // shares BlendTree's sampler so both graph paths behave identically.
                BlendTree::SampleClipBoneTransforms(Clip, normalizedTime * Clip->Duration, boneCount, ctx, outBoneTransforms);
                break;
            }
            case MotionType::BlendTree:
            {
                if (!Tree)
                {
                    BlendTree::FillBindPose(ctx, boneCount, outBoneTransforms);
                    return;
                }
                Tree->Evaluate(normalizedTime, params, boneCount, ctx, outBoneTransforms);
                break;
            }
            default:
            {
                // Unknown motion type — emit bind-pose transforms so the output is
                // always fully initialised for the caller.
                BlendTree::FillBindPose(ctx, boneCount, outBoneTransforms);
                break;
            }
        }
    }

    f32 AnimationState::GetClipDuration() const
    {
        if (Type == MotionType::SingleClip && Clip && Speed > 0.0f)
        {
            return Clip->Duration / Speed;
        }
        return 0.0f;
    }

    f32 AnimationState::GetEffectiveDuration(const AnimationParameterSet& params) const
    {
        if (Type == MotionType::SingleClip && Clip && Speed > 0.0f)
        {
            return Clip->Duration / Speed;
        }
        if (Type == MotionType::BlendTree && Tree && Speed > 0.0f)
        {
            return Tree->GetDuration(params) / Speed;
        }
        return 0.0f;
    }
} // namespace OloEngine
