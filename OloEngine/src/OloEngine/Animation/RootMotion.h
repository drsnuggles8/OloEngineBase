#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/BlendNode.h" // BoneTransform, PoseEvalContext

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <span>
#include <string>

namespace OloEngine
{
    class AnimationClip;

    // Root-motion extraction / baking configuration. Bundled into one parameter
    // object so the AnimationAsset constructor does not take multiple bool
    // arguments (which are easy to transpose at a call site). Defined at namespace
    // scope rather than nested so it can be used as a defaulted (`= {}`)
    // constructor argument — Clang rejects that for a nested aggregate.
    //
    // Lives here (not AnimationAsset.h) so AnimationClip can carry a per-clip copy
    // the runtime consumes: AnimationAsset stamps its authored settings onto the
    // clip it wraps (SetAnimationClip), and every play path — AnimationSystem,
    // AnimationStateMachine, BlendTree — reads the clip's copy (issue #631).
    //
    // Mask semantics: a component of 1 extracts that channel (removed from the
    // sampled pose, delivered to the entity / character controller); 0 leaves the
    // channel in the pose. Rotation masks select X/Y/Z euler components of the
    // rotation delta — exact for the common all-or-nothing masks, small-angle
    // approximate for partial masks on composite rotations.
    struct AnimationRootMotionSettings
    {
        bool ExtractRootMotion = false;
        u32 RootBoneIndex = 0;
        glm::vec3 RootTranslationMask = glm::vec3(1.0f);
        glm::vec3 RootRotationMask = glm::vec3(1.0f);
        bool DiscardRootMotion = false;
    };
} // namespace OloEngine

namespace OloEngine::Animation
{
    // Per-tick root-motion delta. Translation/Rotation express how the root bone
    // moved over the tick. Space depends on the producer: the low-level extractor
    // returns the delta in the root bone's LOCAL (parent-relative) space;
    // ToModelSpace / ExtractConfiguredDelta convert it into entity/model space.
    // HasMotion distinguishes "clip contributed a (possibly zero) delta" from
    // "nothing extracted" so blend weights can renormalize correctly.
    struct RootMotionDelta
    {
        glm::vec3 Translation{ 0.0f };
        glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        bool HasMotion = false;
    };

    namespace RootMotionUtils
    {
        // Sample the root track of `clip` from startTime over deltaSeconds and
        // return the (unmasked) TRS delta in the bone's local space.
        // Looping clips accumulate across the wrap boundary — the wrap jump
        // S(0) - S(Duration) is deliberately NOT motion; the delta is
        // [S(D) - S(t0)] + fullLoops + [S(t1) - S(0)]. Non-looping clips clamp at
        // the clip end (the key samplers clamp past the last key).
        // startTime may lie outside [0, Duration) — it is wrapped first when
        // looping. deltaSeconds <= 0 or a missing root track yields no motion.
        [[nodiscard("extracted delta must be used")]] RootMotionDelta ExtractClipRootDelta(
            const AnimationClip& clip,
            const std::string& rootBoneName,
            f32 startTime,
            f32 deltaSeconds,
            bool looping);

        // Apply per-channel extraction masks (see AnimationRootMotionSettings) to
        // a delta. Translation is masked exactly; rotation masks select euler
        // components of the delta (exact for all-ones / all-zeros masks).
        [[nodiscard("masked delta must be used")]] RootMotionDelta ApplyMasks(
            const RootMotionDelta& delta,
            const glm::vec3& translationMask,
            const glm::vec3& rotationMask);

        // Linear blend of two deltas (mix translation, slerp rotation) at
        // alpha in [0,1]: 0 = a, 1 = b. A side with HasMotion == false
        // contributes zero motion (identity), matching how a non-extracting clip
        // participates in a pose blend.
        [[nodiscard("blended delta must be used")]] RootMotionDelta Blend(
            const RootMotionDelta& a,
            const RootMotionDelta& b,
            f32 alpha);

        // Convert a root-local delta into entity/model space by conjugating with
        // the bind-pose transform of the root bone's ancestors (a root-motion
        // bone's ancestors are static by definition, so bind pose is
        // authoritative): C = BindPoseGlobal[parent(root)] * PreTransform[root].
        // Empty spans degrade the conversion to identity.
        [[nodiscard("converted delta must be used")]] RootMotionDelta ToModelSpace(
            const RootMotionDelta& delta,
            u32 rootBoneIndex,
            std::span<const int> parentIndices,
            std::span<const glm::mat4> bindPoseGlobals,
            std::span<const glm::mat4> preTransforms);

        // High-level per-clip extraction honoring the clip's own
        // AnimationRootMotionSettings: returns zero motion unless
        // ExtractRootMotion is set (and DiscardRootMotion is not), resolves the
        // root bone name via ctx.BoneNames[RootBoneIndex], applies the masks and
        // converts to model space via ctx's skeleton spans.
        [[nodiscard("extracted delta must be used")]] RootMotionDelta ExtractConfiguredDelta(
            const AnimationClip& clip,
            f32 startTime,
            f32 deltaSeconds,
            bool looping,
            const PoseEvalContext& ctx);

        // In-place-ification of the sampled root pose: returns `sampled` with the
        // extracted (masked) motion removed, so the mesh no longer double-moves
        // when the delta is applied to the entity. reference is the clip's t=0
        // root sample. Masked translation channels are pinned to the reference;
        // rotation keeps only the non-extracted euler components of the
        // cumulative reference→sampled rotation.
        [[nodiscard("pinned pose must be used")]] BoneTransform MakeInPlaceRootPose(
            const BoneTransform& sampled,
            const BoneTransform& reference,
            const glm::vec3& translationMask,
            const glm::vec3& rotationMask);
    } // namespace RootMotionUtils
} // namespace OloEngine::Animation
