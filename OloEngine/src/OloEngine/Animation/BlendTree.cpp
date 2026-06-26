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

        using enum BlendType;
        switch (Type)
        {
            case Simple1D:
            {
                f32 paramValue = params.GetFloat(BlendParameterX);
                Evaluate1D(paramValue, normalizedTime, boneCount, outBoneTransforms);
                break;
            }
            case SimpleDirectional2D:
            case FreeformDirectional2D:
            case FreeformCartesian2D:
            {
                f32 paramX = params.GetFloat(BlendParameterX);
                f32 paramY = params.GetFloat(BlendParameterY);
                Evaluate2D(paramX, paramY, normalizedTime, boneCount, outBoneTransforms);
                break;
            }
            default:
            {
                // Unknown blend type — emit default transforms so the output is
                // always sized for the caller.
                outBoneTransforms.resize(boneCount);
                break;
            }
        }
    }

    namespace
    {
        // Weighted-average duration between the two 1D children bracketing paramValue.
        [[nodiscard("blended duration must be used")]] f32 Compute1DDuration(const std::vector<BlendTree::BlendChild>& children, f32 paramValue)
        {
            if (children.size() == 1)
            {
                return (children[0].Clip && children[0].Speed > 0.0f) ? children[0].Clip->Duration / children[0].Speed : 0.0f;
            }

            // Find the two neighbors
            auto neighborCount = children.size() - 1;
            for (sizet i = 0; i < neighborCount; ++i)
            {
                if (paramValue <= children[i + 1].Threshold)
                {
                    f32 range = children[i + 1].Threshold - children[i].Threshold;
                    f32 t = (range > 0.0f) ? (paramValue - children[i].Threshold) / range : 0.0f;
                    t = glm::clamp(t, 0.0f, 1.0f);
                    f32 durA = (children[i].Clip && children[i].Speed > 0.0f) ? children[i].Clip->Duration / children[i].Speed : 0.0f;
                    f32 durB = (children[i + 1].Clip && children[i + 1].Speed > 0.0f) ? children[i + 1].Clip->Duration / children[i + 1].Speed : 0.0f;
                    return glm::mix(durA, durB, t);
                }
            }
            auto& last = children.back();
            return (last.Clip && last.Speed > 0.0f) ? last.Clip->Duration / last.Speed : 0.0f;
        }

        // Inverse-distance-weighted average duration of the 2D children (matches Evaluate2D).
        f32 Compute2DDuration(const std::vector<BlendTree::BlendChild>& children, const glm::vec2& paramPos)
        {
            f32 weightedDuration = 0.0f;
            f32 totalWeight = 0.0f;

            for (auto const& child : children)
            {
                if (!child.Clip || child.Speed <= 0.0f)
                {
                    continue;
                }
                f32 dist = glm::length(paramPos - child.Position);
                if (dist < 1e-6f)
                {
                    return child.Clip->Duration / child.Speed;
                }
                f32 w = 1.0f / (dist * dist);
                weightedDuration += w * (child.Clip->Duration / child.Speed);
                totalWeight += w;
            }
            return totalWeight > 0.0f ? weightedDuration / totalWeight : 0.0f;
        }

        // Fallback: plain average duration over all playable children.
        [[nodiscard("average duration must be used")]] f32 ComputeAverageDuration(const std::vector<BlendTree::BlendChild>& children)
        {
            f32 totalDuration = 0.0f;
            i32 count = 0;
            for (auto const& child : children)
            {
                if (child.Clip && child.Speed > 0.0f)
                {
                    totalDuration += child.Clip->Duration / child.Speed;
                    ++count;
                }
            }
            return count > 0 ? totalDuration / static_cast<f32>(count) : 0.0f;
        }
    } // namespace

    f32 BlendTree::GetDuration(const AnimationParameterSet& params) const
    {
        OLO_PROFILE_FUNCTION();

        if (Children.empty())
        {
            return 0.0f;
        }

        // 1D: weighted average between neighbors when a blend param is set,
        // otherwise a plain average over all playable children.
        if (Type == BlendType::Simple1D)
        {
            return BlendParameterX.empty()
                       ? ComputeAverageDuration(Children)
                       : Compute1DDuration(Children, params.GetFloat(BlendParameterX));
        }

        // 2D types: inverse-distance weighted average matching Evaluate2D.
        glm::vec2 paramPos(params.GetFloat(BlendParameterX), params.GetFloat(BlendParameterY));
        return Compute2DDuration(Children, paramPos);
    }

    void BlendTree::Evaluate1D(f32 paramValue, f32 normalizedTime, sizet boneCount,
                               std::vector<BoneTransform>& out) const
    {
        OLO_PROFILE_FUNCTION();

        // Collect playable children (valid clip and positive speed)
        std::vector<sizet> playableIndices;
        auto childCount = Children.size();
        playableIndices.reserve(childCount);
        for (sizet i = 0; i < childCount; ++i)
        {
            if (Children[i].Clip && Children[i].Speed > 0.0f)
            {
                playableIndices.push_back(i);
            }
        }

        if (playableIndices.empty())
        {
            out.resize(boneCount);
            return;
        }

        if (playableIndices.size() == 1)
        {
            auto const& child = Children[playableIndices[0]];
            f32 timeSeconds = normalizedTime * child.Clip->Duration;
            SampleClipBoneTransforms(child.Clip, timeSeconds, boneCount, out);
            return;
        }

        // Clamp param to playable child range
        f32 minThreshold = Children[playableIndices.front()].Threshold;
        f32 maxThreshold = Children[playableIndices.back()].Threshold;
        for (sizet idx : playableIndices)
        {
            minThreshold = std::min(minThreshold, Children[idx].Threshold);
            maxThreshold = std::max(maxThreshold, Children[idx].Threshold);
        }
        paramValue = glm::clamp(paramValue, minThreshold, maxThreshold);

        // Find the two neighboring playable children
        auto playablePairs = playableIndices.size() - 1;
        for (sizet pi = 0; pi < playablePairs; ++pi)
        {
            sizet idxA = playableIndices[pi];
            sizet idxB = playableIndices[pi + 1];

            if (paramValue <= Children[idxB].Threshold)
            {
                f32 range = Children[idxB].Threshold - Children[idxA].Threshold;
                f32 t = (range > 0.0f) ? (paramValue - Children[idxA].Threshold) / range : 0.0f;

                std::vector<BoneTransform> transformsA;
                std::vector<BoneTransform> transformsB;

                f32 durA = Children[idxA].Clip->Duration;
                f32 durB = Children[idxB].Clip->Duration;
                f32 timeA = normalizedTime * durA;
                f32 timeB = normalizedTime * durB;

                SampleClipBoneTransforms(Children[idxA].Clip, timeA, boneCount, transformsA);
                SampleClipBoneTransforms(Children[idxB].Clip, timeB, boneCount, transformsB);

                BlendBoneTransforms(transformsA, transformsB, t, out);
                return;
            }
        }

        // Past the last playable child
        auto const& lastChild = Children[playableIndices.back()];
        f32 timeSeconds = normalizedTime * lastChild.Clip->Duration;
        SampleClipBoneTransforms(lastChild.Clip, timeSeconds, boneCount, out);
    }

    void BlendTree::Evaluate2D(f32 paramX, f32 paramY, f32 normalizedTime, sizet boneCount,
                               std::vector<BoneTransform>& out) const
    {
        OLO_PROFILE_FUNCTION();

        // Inverse distance weighting for 2D blend
        glm::vec2 paramPos(paramX, paramY);

        auto childCount = Children.size();
        std::vector<f32> weights(childCount, 0.0f);
        f32 totalWeight = 0.0f;

        for (sizet i = 0; i < childCount; ++i)
        {
            if (!Children[i].Clip || Children[i].Speed <= 0.0f)
            {
                continue;
            }
            f32 dist = glm::length(paramPos - Children[i].Position);
            if (dist < 1e-6f)
            {
                // Exact match - use this child exclusively
                f32 timeSeconds = normalizedTime * Children[i].Clip->Duration;
                SampleClipBoneTransforms(Children[i].Clip, timeSeconds, boneCount, out);
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
        for (sizet i = 0; i < childCount; ++i)
        {
            if (weights[i] < 1e-6f)
            {
                continue;
            }

            std::vector<BoneTransform> childTransforms;
            f32 clipDur = Children[i].Clip ? Children[i].Clip->Duration : 0.0f;
            // Speed is already factored into GetDuration (and thus normalizedTime)
            f32 timeSeconds = normalizedTime * clipDur;
            SampleClipBoneTransforms(Children[i].Clip, timeSeconds, boneCount, childTransforms);

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

    void BlendTree::SampleClipBoneTransforms(const Ref<AnimationClip>& clip, f32 timeSeconds,
                                             sizet boneCount,
                                             std::vector<BoneTransform>& out)
    {
        OLO_PROFILE_FUNCTION();

        out.resize(boneCount);
        if (!clip)
        {
            return;
        }

        auto boneAnimCount = clip->BoneAnimations.size();
        for (sizet i = 0; i < boneCount && i < boneAnimCount; ++i)
        {
            auto const& boneAnim = clip->BoneAnimations[i];
            out[i].Translation = AnimatedModel::SampleBonePosition(boneAnim.PositionKeys, timeSeconds);
            out[i].Rotation = AnimatedModel::SampleBoneRotation(boneAnim.RotationKeys, timeSeconds);
            out[i].Scale = AnimatedModel::SampleBoneScale(boneAnim.ScaleKeys, timeSeconds);
        }
    }

    void BlendTree::BlendBoneTransforms(const std::vector<BoneTransform>& a,
                                        const std::vector<BoneTransform>& b,
                                        f32 weight, std::vector<BoneTransform>& out)
    {
        OLO_PROFILE_FUNCTION();

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
