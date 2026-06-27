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
        // Selection of the playable 1D children bracketing paramValue. Evaluate1D
        // (pose) and Compute1DDuration (duration) both run through this, so the
        // rendered pose and the duration used to normalize time always blend the
        // SAME pair of clips with the SAME weight — the core invariant behind #410.
        // A child is "playable" when it has a valid clip and a positive speed;
        // non-playable children are skipped and the param is clamped to the playable
        // threshold range.
        struct Blend1DSelection
        {
            sizet IndexA = 0;         // lower bracketing playable child (or the sole source)
            sizet IndexB = 0;         // upper bracketing playable child (unused when Single)
            f32 Weight = 0.0f;        // blend factor: IndexA at 0, IndexB at 1
            bool HasPlayable = false; // false when no child has a valid clip + positive speed
            bool Single = false;      // true when one clip is sampled fully (only IndexA valid)
        };

        [[nodiscard("blend selection must be used")]] Blend1DSelection SelectBlend1DNeighbors(const std::vector<BlendTree::BlendChild>& children, f32 paramValue)
        {
            Blend1DSelection selection;

            // Collect playable children (valid clip and positive speed).
            std::vector<sizet> playableIndices;
            auto childCount = children.size();
            playableIndices.reserve(childCount);
            for (sizet i = 0; i < childCount; ++i)
            {
                if (children[i].Clip && children[i].Speed > 0.0f)
                {
                    playableIndices.push_back(i);
                }
            }

            if (playableIndices.empty())
            {
                return selection; // HasPlayable stays false
            }
            selection.HasPlayable = true;

            if (playableIndices.size() == 1)
            {
                selection.Single = true;
                selection.IndexA = playableIndices.front();
                return selection;
            }

            // Clamp param to the playable child threshold range.
            f32 minThreshold = children[playableIndices.front()].Threshold;
            f32 maxThreshold = children[playableIndices.back()].Threshold;
            for (sizet idx : playableIndices)
            {
                minThreshold = std::min(minThreshold, children[idx].Threshold);
                maxThreshold = std::max(maxThreshold, children[idx].Threshold);
            }
            paramValue = glm::clamp(paramValue, minThreshold, maxThreshold);

            // Find the two neighboring playable children bracketing paramValue.
            auto playablePairs = playableIndices.size() - 1;
            for (sizet pi = 0; pi < playablePairs; ++pi)
            {
                sizet idxA = playableIndices[pi];
                sizet idxB = playableIndices[pi + 1];

                if (paramValue <= children[idxB].Threshold)
                {
                    f32 range = children[idxB].Threshold - children[idxA].Threshold;
                    selection.IndexA = idxA;
                    selection.IndexB = idxB;
                    selection.Weight = (range > 0.0f) ? (paramValue - children[idxA].Threshold) / range : 0.0f;
                    return selection;
                }
            }

            // Past the last playable child: sample it fully.
            selection.Single = true;
            selection.IndexA = playableIndices.back();
            return selection;
        }

        // Weighted-average effective duration of the playable 1D children bracketing
        // paramValue. Shares SelectBlend1DNeighbors with Evaluate1D so duration and
        // pose can never bracket different clips (issue #410). Speed is folded in so
        // time normalization plays each clip at its true rate.
        [[nodiscard("blended duration must be used")]] f32 Compute1DDuration(const std::vector<BlendTree::BlendChild>& children, f32 paramValue)
        {
            Blend1DSelection selection = SelectBlend1DNeighbors(children, paramValue);
            if (!selection.HasPlayable)
            {
                return 0.0f;
            }

            auto effectiveDuration = [&children](sizet idx)
            { return children[idx].Clip->Duration / children[idx].Speed; };
            if (selection.Single)
            {
                return effectiveDuration(selection.IndexA);
            }
            return glm::mix(effectiveDuration(selection.IndexA), effectiveDuration(selection.IndexB), selection.Weight);
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
    } // namespace

    f32 BlendTree::GetDuration(const AnimationParameterSet& params) const
    {
        OLO_PROFILE_FUNCTION();

        if (Children.empty())
        {
            return 0.0f;
        }

        // 1D: weighted average between the neighbors bracketing the blend param.
        // An unbound (empty) BlendParameterX resolves to GetFloat("") == 0.0f, the
        // same value Evaluate() feeds Evaluate1D -- so duration and pose stay
        // consistent for an unconfigured tree (issue #410).
        if (Type == BlendType::Simple1D)
        {
            return Compute1DDuration(Children, params.GetFloat(BlendParameterX));
        }

        // 2D types: inverse-distance weighted average matching Evaluate2D.
        glm::vec2 paramPos(params.GetFloat(BlendParameterX), params.GetFloat(BlendParameterY));
        return Compute2DDuration(Children, paramPos);
    }

    void BlendTree::Evaluate1D(f32 paramValue, f32 normalizedTime, sizet boneCount,
                               std::vector<BoneTransform>& out) const
    {
        OLO_PROFILE_FUNCTION();

        // Same selection as Compute1DDuration, so the sampled pose and the
        // normalization duration always blend the same clip pair (issue #410).
        Blend1DSelection selection = SelectBlend1DNeighbors(Children, paramValue);

        if (!selection.HasPlayable)
        {
            out.resize(boneCount);
            return;
        }

        if (selection.Single)
        {
            auto const& child = Children[selection.IndexA];
            f32 timeSeconds = normalizedTime * child.Clip->Duration;
            SampleClipBoneTransforms(child.Clip, timeSeconds, boneCount, out);
            return;
        }

        // Blend the two bracketing playable children at the selected weight.
        auto const& childA = Children[selection.IndexA];
        auto const& childB = Children[selection.IndexB];

        std::vector<BoneTransform> transformsA;
        std::vector<BoneTransform> transformsB;
        f32 timeA = normalizedTime * childA.Clip->Duration;
        f32 timeB = normalizedTime * childB.Clip->Duration;

        SampleClipBoneTransforms(childA.Clip, timeA, boneCount, transformsA);
        SampleClipBoneTransforms(childB.Clip, timeB, boneCount, transformsB);

        BlendBoneTransforms(transformsA, transformsB, selection.Weight, out);
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
