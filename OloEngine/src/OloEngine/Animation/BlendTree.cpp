#include "OloEnginePCH.h"
#include "OloEngine/Animation/BlendTree.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Core/Log.h"
#include <algorithm>
#include <numeric>

namespace OloEngine
{
    void BlendTree::Evaluate(f32 normalizedTime, const AnimationParameterSet& params,
                             sizet boneCount,
                             std::vector<BoneTransform>& outBoneTransforms) const
    {
        OLO_PROFILE_FUNCTION();

        if (Children.empty())
        {
            outBoneTransforms.resize(boneCount);
            return;
        }

        switch (Type)
        {
            case BlendType::Simple1D:
            {
                f32 paramValue = params.GetFloat(BlendParameterX);
                Evaluate1D(paramValue, normalizedTime, boneCount, outBoneTransforms);
                break;
            }
            case BlendType::SimpleDirectional2D:
            case BlendType::FreeformDirectional2D:
            case BlendType::FreeformCartesian2D:
            {
                f32 paramX = params.GetFloat(BlendParameterX);
                f32 paramY = params.GetFloat(BlendParameterY);
                Evaluate2D(paramX, paramY, normalizedTime, boneCount, outBoneTransforms);
                break;
            }
        }
    }

    f32 BlendTree::GetDuration(const AnimationParameterSet& params) const
    {
        if (Children.empty())
        {
            return 0.0f;
        }

        // For 1D: weighted average of durations between two neighboring children
        if (Type == BlendType::Simple1D && !BlendParameterX.empty())
        {
            f32 paramValue = params.GetFloat(BlendParameterX);

            if (Children.size() == 1)
            {
                return Children[0].Clip ? Children[0].Clip->Duration / Children[0].Speed : 0.0f;
            }

            // Find the two neighbors
            for (sizet i = 0; i < Children.size() - 1; ++i)
            {
                if (paramValue <= Children[i + 1].Threshold)
                {
                    f32 range = Children[i + 1].Threshold - Children[i].Threshold;
                    f32 t = (range > 0.0f) ? (paramValue - Children[i].Threshold) / range : 0.0f;
                    t = glm::clamp(t, 0.0f, 1.0f);
                    f32 durA = Children[i].Clip ? Children[i].Clip->Duration / Children[i].Speed : 0.0f;
                    f32 durB = Children[i + 1].Clip ? Children[i + 1].Clip->Duration / Children[i + 1].Speed : 0.0f;
                    return glm::mix(durA, durB, t);
                }
            }
            auto& last = Children.back();
            return last.Clip ? last.Clip->Duration / last.Speed : 0.0f;
        }

        // Fallback: average duration
        f32 totalDuration = 0.0f;
        i32 count = 0;
        for (auto const& child : Children)
        {
            if (child.Clip)
            {
                totalDuration += child.Clip->Duration / child.Speed;
                ++count;
            }
        }
        return count > 0 ? totalDuration / static_cast<f32>(count) : 0.0f;
    }

    void BlendTree::Evaluate1D(f32 paramValue, f32 normalizedTime, sizet boneCount,
                               std::vector<BoneTransform>& out) const
    {
        if (Children.size() == 1)
        {
            f32 time = normalizedTime * (Children[0].Clip ? Children[0].Clip->Duration : 0.0f);
            SampleClipBoneTransforms(Children[0].Clip, time, boneCount, out);
            return;
        }

        // Clamp param to child range
        f32 minThreshold = Children.front().Threshold;
        f32 maxThreshold = Children.back().Threshold;
        paramValue = glm::clamp(paramValue, minThreshold, maxThreshold);

        // Find the two neighboring children
        for (sizet i = 0; i < Children.size() - 1; ++i)
        {
            if (paramValue <= Children[i + 1].Threshold)
            {
                f32 range = Children[i + 1].Threshold - Children[i].Threshold;
                f32 t = (range > 0.0f) ? (paramValue - Children[i].Threshold) / range : 0.0f;

                std::vector<BoneTransform> transformsA;
                std::vector<BoneTransform> transformsB;

                // Speed is already factored into GetDuration (and thus normalizedTime)
                f32 durA = Children[i].Clip ? Children[i].Clip->Duration : 0.0f;
                f32 durB = Children[i + 1].Clip ? Children[i + 1].Clip->Duration : 0.0f;
                f32 timeA = normalizedTime * durA;
                f32 timeB = normalizedTime * durB;

                SampleClipBoneTransforms(Children[i].Clip, timeA, boneCount, transformsA);
                SampleClipBoneTransforms(Children[i + 1].Clip, timeB, boneCount, transformsB);

                BlendBoneTransforms(transformsA, transformsB, t, out);
                return;
            }
        }

        // Past the last child
        f32 lastDur = Children.back().Clip ? Children.back().Clip->Duration : 0.0f;
        f32 time = normalizedTime * lastDur;
        SampleClipBoneTransforms(Children.back().Clip, time, boneCount, out);
    }

    void BlendTree::Evaluate2D(f32 paramX, f32 paramY, f32 normalizedTime, sizet boneCount,
                               std::vector<BoneTransform>& out) const
    {
        // Inverse distance weighting for 2D blend
        glm::vec2 paramPos(paramX, paramY);

        std::vector<f32> weights(Children.size(), 0.0f);
        f32 totalWeight = 0.0f;

        for (sizet i = 0; i < Children.size(); ++i)
        {
            f32 dist = glm::length(paramPos - Children[i].Position);
            if (dist < 1e-6f)
            {
                // Exact match - use this child exclusively
                f32 time = normalizedTime * (Children[i].Clip ? Children[i].Clip->Duration : 0.0f);
                SampleClipBoneTransforms(Children[i].Clip, time, boneCount, out);
                return;
            }
            weights[i] = 1.0f / (dist * dist);
            totalWeight += weights[i];
        }

        // Normalize weights
        if (totalWeight > 0.0f)
        {
            for (auto& w : weights)
            {
                w /= totalWeight;
            }
        }

        // Blend all children by weight using cumulative-weight slerp for rotations
        out.resize(boneCount);
        f32 accumulatedWeight = 0.0f;
        bool first = true;
        for (sizet i = 0; i < Children.size(); ++i)
        {
            if (weights[i] < 1e-6f)
            {
                continue;
            }

            std::vector<BoneTransform> childTransforms;
            f32 clipDur = Children[i].Clip ? Children[i].Clip->Duration : 0.0f;
            // Speed is already factored into GetDuration (and thus normalizedTime)
            f32 time = normalizedTime * clipDur;
            SampleClipBoneTransforms(Children[i].Clip, time, boneCount, childTransforms);

            accumulatedWeight += weights[i];

            if (first)
            {
                for (sizet b = 0; b < boneCount; ++b)
                {
                    out[b].Translation = childTransforms[b].Translation * weights[i];
                    out[b].Rotation = childTransforms[b].Rotation;
                    out[b].Scale = childTransforms[b].Scale * weights[i];
                }
                first = false;
            }
            else
            {
                // Cumulative-weight slerp: t = w_i / (w_0 + ... + w_i)
                f32 slerpT = weights[i] / accumulatedWeight;
                for (sizet b = 0; b < boneCount; ++b)
                {
                    out[b].Translation += childTransforms[b].Translation * weights[i];
                    out[b].Rotation = glm::slerp(out[b].Rotation, childTransforms[b].Rotation, slerpT);
                    out[b].Scale += childTransforms[b].Scale * weights[i];
                }
            }
        }
    }

    void BlendTree::SampleClipBoneTransforms(const Ref<AnimationClip>& clip, f32 time,
                                             sizet boneCount,
                                             std::vector<BoneTransform>& out)
    {
        out.resize(boneCount);
        if (!clip)
        {
            return;
        }

        for (sizet i = 0; i < boneCount && i < clip->BoneAnimations.size(); ++i)
        {
            auto const& boneAnim = clip->BoneAnimations[i];
            out[i].Translation = AnimatedModel::SampleBonePosition(boneAnim.PositionKeys, time);
            out[i].Rotation = AnimatedModel::SampleBoneRotation(boneAnim.RotationKeys, time);
            out[i].Scale = AnimatedModel::SampleBoneScale(boneAnim.ScaleKeys, time);
        }
    }

    void BlendTree::BlendBoneTransforms(const std::vector<BoneTransform>& a,
                                        const std::vector<BoneTransform>& b,
                                        f32 weight, std::vector<BoneTransform>& out)
    {
        sizet count = std::max(a.size(), b.size());
        out.resize(count);

        for (sizet i = 0; i < count; ++i)
        {
            const auto& boneA = (i < a.size()) ? a[i] : BoneTransform{};
            const auto& boneB = (i < b.size()) ? b[i] : BoneTransform{};

            out[i].Translation = glm::mix(boneA.Translation, boneB.Translation, weight);
            out[i].Rotation = glm::slerp(boneA.Rotation, boneB.Rotation, weight);
            out[i].Scale = glm::mix(boneA.Scale, boneB.Scale, weight);
        }
    }
} // namespace OloEngine
