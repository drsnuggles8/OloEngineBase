#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Renderer/AnimatedModel.h"

namespace OloEngine
{
    namespace
    {
        // Sample a single clip into the output pose: clear to bind pose, then
        // per-bone TRS for every bone that has an animation channel.
        void SampleClipIntoPose(const AnimationClip& clip, f32 normalizedTime, sizet boneCount,
                                std::vector<BoneTransform>& out)
        {
            f32 timeSeconds = normalizedTime * clip.Duration;
            out.assign(boneCount, BoneTransform{});
            auto boneAnimCount = clip.BoneAnimations.size();
            for (sizet i = 0; i < boneCount; ++i)
            {
                if (i < boneAnimCount)
                {
                    auto const& boneAnim = clip.BoneAnimations[i];
                    out[i].Translation = AnimatedModel::SampleBonePosition(boneAnim.PositionKeys, timeSeconds);
                    out[i].Rotation = AnimatedModel::SampleBoneRotation(boneAnim.RotationKeys, timeSeconds);
                    out[i].Scale = AnimatedModel::SampleBoneScale(boneAnim.ScaleKeys, timeSeconds);
                }
            }
        }
    } // namespace

    void AnimationState::Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
                                  sizet boneCount,
                                  std::vector<BoneTransform>& outBoneTransforms) const
    {
        OLO_PROFILE_FUNCTION();

        switch (Type)
        {
            case MotionType::SingleClip:
            {
                if (!Clip)
                {
                    outBoneTransforms.assign(boneCount, BoneTransform{});
                    return;
                }
                SampleClipIntoPose(*Clip, normalizedTime, boneCount, outBoneTransforms);
                break;
            }
            case MotionType::BlendTree:
            {
                if (!Tree)
                {
                    outBoneTransforms.assign(boneCount, BoneTransform{});
                    return;
                }
                Tree->Evaluate(normalizedTime, params, boneCount, outBoneTransforms);
                break;
            }
            default:
            {
                // Unknown motion type — emit identity transforms so the output is
                // always fully initialised for the caller.
                outBoneTransforms.assign(boneCount, BoneTransform{});
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
