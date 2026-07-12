#include "OloEnginePCH.h"
#include "OloEngine/Animation/RootMotion.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Renderer/AnimatedModel.h"

#include <algorithm>
#include <cmath>

namespace OloEngine::Animation::RootMotionUtils
{
    namespace
    {
        constexpr f32 kMaskEpsilon = 1e-4f;

        [[nodiscard("mask classification must be used")]] bool MaskIsAllOnes(const glm::vec3& mask)
        {
            return mask.x >= 1.0f - kMaskEpsilon && mask.y >= 1.0f - kMaskEpsilon && mask.z >= 1.0f - kMaskEpsilon;
        }

        [[nodiscard("mask classification must be used")]] bool MaskIsAllZeros(const glm::vec3& mask)
        {
            return mask.x <= kMaskEpsilon && mask.y <= kMaskEpsilon && mask.z <= kMaskEpsilon;
        }

        // Select the masked euler components of a rotation. Exact for
        // all-ones/all-zeros masks; small-angle approximate otherwise (euler
        // components do not commute), which is fine for per-tick deltas.
        [[nodiscard("masked rotation must be used")]] glm::quat MaskRotation(const glm::quat& rotation, const glm::vec3& mask)
        {
            if (MaskIsAllOnes(mask))
            {
                return rotation;
            }
            if (MaskIsAllZeros(mask))
            {
                return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            glm::vec3 euler = glm::eulerAngles(rotation) * glm::clamp(mask, glm::vec3(0.0f), glm::vec3(1.0f));
            return glm::quat(euler);
        }

        // The complement of MaskRotation: the euler components the mask does NOT
        // extract (what stays in the pose).
        [[nodiscard("residual rotation must be used")]] glm::quat ResidualRotation(const glm::quat& rotation, const glm::vec3& mask)
        {
            return MaskRotation(rotation, glm::vec3(1.0f) - glm::clamp(mask, glm::vec3(0.0f), glm::vec3(1.0f)));
        }
    } // namespace

    RootMotionDelta ExtractClipRootDelta(
        const AnimationClip& clip,
        const std::string& rootBoneName,
        f32 startTime,
        f32 deltaSeconds,
        bool looping)
    {
        RootMotionDelta result;
        if (!std::isfinite(startTime) || !std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
        {
            return result;
        }

        const BoneAnimation* track = clip.FindBoneAnimation(rootBoneName);
        if (!track || (track->PositionKeys.empty() && track->RotationKeys.empty()))
        {
            return result;
        }

        auto samplePos = [track](f32 t)
        { return AnimatedModel::SampleBonePosition(track->PositionKeys, t); };
        auto sampleRot = [track](f32 t)
        { return AnimatedModel::SampleBoneRotation(track->RotationKeys, t); };

        const f32 duration = clip.Duration;
        if (!looping || duration <= 0.0f)
        {
            // Non-looping: clamp both sample times to the clip window ourselves —
            // the engine's key samplers EXTRAPOLATE linearly past the last key
            // (FindKeyframeIndexForBoneKeys returns the second-to-last index for
            // beyond-end times, so glm::mix runs with t > 1), and a clamped clip
            // must contribute no further motion once it ends.
            f32 t0 = startTime;
            f32 t1 = startTime + deltaSeconds;
            if (duration > 0.0f)
            {
                t0 = glm::clamp(t0, 0.0f, duration);
                t1 = glm::clamp(t1, 0.0f, duration);
            }
            result.Translation = samplePos(t1) - samplePos(t0);
            result.Rotation = glm::normalize(glm::inverse(sampleRot(t0)) * sampleRot(t1));
            result.HasMotion = true;
            return result;
        }

        // Wrap startTime into [0, duration).
        f32 t0 = startTime;
        while (t0 >= duration)
            t0 -= duration;
        while (t0 < 0.0f)
            t0 += duration;

        const f32 rawEnd = t0 + deltaSeconds;
        if (rawEnd < duration)
        {
            // No wrap this tick.
            result.Translation = samplePos(rawEnd) - samplePos(t0);
            result.Rotation = glm::normalize(glm::inverse(sampleRot(t0)) * sampleRot(rawEnd));
            result.HasMotion = true;
            return result;
        }

        // Wrapped: accumulate tail-of-clip + (wraps - 1) full loops + head. The
        // wrap jump S(0) - S(D) is a teleport back to clip start, not motion.
        const u32 wraps = static_cast<u32>(rawEnd / duration);
        f32 t1 = rawEnd - static_cast<f32>(wraps) * duration;
        t1 = glm::clamp(t1, 0.0f, duration);

        const glm::vec3 tailT = samplePos(duration) - samplePos(t0);
        const glm::vec3 fullT = samplePos(duration) - samplePos(0.0f);
        const glm::vec3 headT = samplePos(t1) - samplePos(0.0f);
        result.Translation = tailT + static_cast<f32>(wraps - 1) * fullT + headT;

        const glm::quat tailR = glm::inverse(sampleRot(t0)) * sampleRot(duration);
        const glm::quat fullR = glm::inverse(sampleRot(0.0f)) * sampleRot(duration);
        const glm::quat headR = glm::inverse(sampleRot(0.0f)) * sampleRot(t1);
        glm::quat rotation = tailR;
        // Bound the loop for pathological duration/dt ratios; a clip short enough
        // to wrap >4096 times in one tick has no meaningful rotation delta anyway.
        const u32 fullLoops = std::min(wraps - 1, 4096u);
        for (u32 i = 0; i < fullLoops; ++i)
        {
            rotation = rotation * fullR;
        }
        result.Rotation = glm::normalize(rotation * headR);
        result.HasMotion = true;
        return result;
    }

    RootMotionDelta ApplyMasks(
        const RootMotionDelta& delta,
        const glm::vec3& translationMask,
        const glm::vec3& rotationMask)
    {
        if (!delta.HasMotion)
        {
            return delta;
        }
        RootMotionDelta result = delta;
        result.Translation = delta.Translation * glm::clamp(translationMask, glm::vec3(0.0f), glm::vec3(1.0f));
        result.Rotation = MaskRotation(delta.Rotation, rotationMask);
        return result;
    }

    RootMotionDelta Blend(const RootMotionDelta& a, const RootMotionDelta& b, f32 alpha)
    {
        const f32 t = glm::clamp(alpha, 0.0f, 1.0f);
        // A non-contributing side blends as zero motion (identity), matching how
        // a clip without root motion participates in the pose blend.
        const glm::vec3 ta = a.HasMotion ? a.Translation : glm::vec3(0.0f);
        const glm::vec3 tb = b.HasMotion ? b.Translation : glm::vec3(0.0f);
        const glm::quat ra = a.HasMotion ? a.Rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const glm::quat rb = b.HasMotion ? b.Rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        RootMotionDelta result;
        result.Translation = glm::mix(ta, tb, t);
        result.Rotation = glm::normalize(glm::slerp(ra, rb, t));
        result.HasMotion = a.HasMotion || b.HasMotion;
        return result;
    }

    RootMotionDelta ToModelSpace(
        const RootMotionDelta& delta,
        u32 rootBoneIndex,
        std::span<const int> parentIndices,
        std::span<const glm::mat4> bindPoseGlobals,
        std::span<const glm::mat4> preTransforms)
    {
        if (!delta.HasMotion)
        {
            return delta;
        }

        // C maps the root bone's parent space into model space. A root-motion
        // bone's ancestors are static (that is what makes it the root), so the
        // bind pose is authoritative for them.
        glm::mat4 conversion(1.0f);
        bool hasConversion = false;
        if (rootBoneIndex < parentIndices.size())
        {
            if (const int parent = parentIndices[rootBoneIndex]; parent >= 0 && static_cast<sizet>(parent) < bindPoseGlobals.size())
            {
                conversion = bindPoseGlobals[static_cast<sizet>(parent)];
                hasConversion = true;
            }
        }
        if (rootBoneIndex < preTransforms.size())
        {
            conversion = conversion * preTransforms[rootBoneIndex];
            hasConversion = true;
        }
        if (!hasConversion)
        {
            return delta;
        }

        const glm::mat3 linear(conversion);
        // Orthonormalize (strip scale) for the rotation conjugation; translation
        // keeps the full linear part so ancestor scale carries into stride length.
        glm::mat3 rotationOnly = linear;
        for (int c = 0; c < 3; ++c)
        {
            const f32 len = glm::length(rotationOnly[c]);
            if (len > 1e-6f)
            {
                rotationOnly[c] /= len;
            }
        }
        const glm::quat frame = glm::normalize(glm::quat_cast(rotationOnly));

        RootMotionDelta result = delta;
        result.Translation = linear * delta.Translation;
        result.Rotation = glm::normalize(frame * delta.Rotation * glm::inverse(frame));
        return result;
    }

    RootMotionDelta ExtractConfiguredDelta(
        const AnimationClip& clip,
        f32 startTime,
        f32 deltaSeconds,
        bool looping,
        const PoseEvalContext& ctx)
    {
        const AnimationRootMotionSettings& settings = clip.RootMotion;
        if (!settings.ExtractRootMotion || settings.DiscardRootMotion)
        {
            // Discard: the pose is still pinned in place by the samplers, but the
            // motion is thrown away instead of delivered to the entity.
            return {};
        }
        if (settings.RootBoneIndex >= ctx.BoneNames.size())
        {
            return {};
        }

        RootMotionDelta raw = ExtractClipRootDelta(
            clip, ctx.BoneNames[settings.RootBoneIndex], startTime, deltaSeconds, looping);
        if (!raw.HasMotion)
        {
            return raw;
        }
        RootMotionDelta masked = ApplyMasks(raw, settings.RootTranslationMask, settings.RootRotationMask);
        return ToModelSpace(masked, settings.RootBoneIndex, ctx.ParentIndices, ctx.BindPoseGlobals, ctx.PreTransforms);
    }

    BoneTransform MakeInPlaceRootPose(
        const BoneTransform& sampled,
        const BoneTransform& reference,
        const glm::vec3& translationMask,
        const glm::vec3& rotationMask)
    {
        BoneTransform result = sampled;

        const glm::vec3 tMask = glm::clamp(translationMask, glm::vec3(0.0f), glm::vec3(1.0f));
        result.Translation = reference.Translation + (glm::vec3(1.0f) - tMask) * (sampled.Translation - reference.Translation);

        if (MaskIsAllZeros(rotationMask))
        {
            return result; // nothing extracted — pose keeps the sampled rotation
        }
        if (MaskIsAllOnes(rotationMask))
        {
            result.Rotation = reference.Rotation;
            return result;
        }
        // Partial mask: keep only the non-extracted euler components of the
        // cumulative reference→sampled rotation.
        const glm::quat cumulative = glm::normalize(glm::inverse(reference.Rotation) * sampled.Rotation);
        result.Rotation = glm::normalize(reference.Rotation * ResidualRotation(cumulative, rotationMask));
        return result;
    }
} // namespace OloEngine::Animation::RootMotionUtils
