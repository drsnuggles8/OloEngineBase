#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/BlendNode.h" // BoneTransform
#include "OloEngine/Animation/SkeletonData.h"
#include "OloEngine/Animation/Retargeting/SkeletonRetargetMap.h"

#include <span>

namespace OloEngine
{
    class AnimationClip;
} // namespace OloEngine

namespace OloEngine::Animation
{
    /**
     * @brief Tunables for a retarget operation.
     */
    struct RetargetOptions
    {
        // Transfer the root bone's local translation (overall locomotion / hip
        // height) from source to target, scaled by RootTranslationScale. Every
        // OTHER bone's translation and scale is taken from the target's rest pose,
        // so the target keeps its own proportions. Full per-bone translation
        // retargeting is deferred (see docs/design/animation-retargeting.md).
        bool RetargetRootTranslation = true;

        // Uniform scale applied to the transferred root translation delta. Use
        // AnimationRetargeter::ComputeRootTranslationScale() to derive it from the
        // two rest poses when the rigs differ in overall scale; defaults to 1:1.
        f32 RootTranslationScale = 1.0f;

        // Target-skeleton index of the bone treated as the root for translation
        // transfer. kUseFirstRoot selects the first bone whose parent index is -1
        // (or bone 0 for the parent-index-free RetargetLocalPose overload).
        static constexpr int kUseFirstRoot = -1;
        int RootBoneIndex = kUseFirstRoot;
    };

    /**
     * @brief Rotation-based, name-mapped animation retargeting (first slice).
     *
     * Applies an AnimationClip authored for one skeleton onto a differently-rigged
     * skeleton. Each mapped target bone receives the source bone's animated rotation
     * expressed as a delta from the source rest pose, re-based onto the target rest
     * pose — so differing rest orientations are handled, and naive rest poses (both
     * identity) degenerate to a direct rotation copy. Translations and scales come
     * from the target rest pose, preserving the target's proportions; only the root
     * translation is optionally transferred.
     *
     * @see docs/design/animation-retargeting.md for what ships in this slice vs. deferred.
     */
    class AnimationRetargeter
    {
      public:
        AnimationRetargeter() = delete;

        /**
         * @brief Retarget a single LOCAL pose.
         *
         * For every target bone, @p outTargetPose is filled: a mapped bone gets the
         * re-based rotation (plus the optional root translation), an unmapped bone
         * gets its target rest transform. All four pose spans are indexed by their
         * own skeleton's bone index; @p outTargetPose must be sized to the target
         * bone count. With RootBoneIndex == kUseFirstRoot, bone 0 is treated as the
         * root (this overload has no parent indices).
         */
        static void RetargetLocalPose(
            const SkeletonRetargetMap& map,
            std::span<const BoneTransform> sourcePose,
            std::span<const BoneTransform> sourceRestLocal,
            std::span<const BoneTransform> targetRestLocal,
            const RetargetOptions& options,
            std::span<BoneTransform> outTargetPose);

        /**
         * @brief Bake a clip authored for @p sourceSkeleton into a NEW clip that
         *        plays correctly on @p targetSkeleton through the normal
         *        AnimationSystem path.
         *
         * The returned clip's tracks are named for the target's bones; rotation keys
         * are re-based per the rest-pose delta and translation/scale keys hold the
         * target rest values (the root's translation optionally transferred). Target
         * bones with no source mapping — or whose source bone has no track in the
         * clip — get no track and keep their rest pose at play time. Returns nullptr
         * if @p sourceClip is null.
         *
         * Rest poses are read from each skeleton's m_BindPoseLocalTransforms, so the
         * skeletons must have had SetBindPose() called.
         */
        [[nodiscard]] static Ref<AnimationClip> RetargetClip(
            const Ref<AnimationClip>& sourceClip,
            const SkeletonData& sourceSkeleton,
            const SkeletonData& targetSkeleton,
            const SkeletonRetargetMap& map,
            const RetargetOptions& options = {});

        /**
         * @brief Derive a uniform root-translation scale from two rest poses.
         *
         * Returns the ratio of target-to-source rig extent, where "extent" is the
         * largest bind-pose global bone offset from the origin (an orientation-
         * independent proxy for character size). Returns 1.0 when the source extent
         * is ~0. Use to populate RetargetOptions::RootTranslationScale.
         */
        [[nodiscard]] static f32 ComputeRootTranslationScale(
            const SkeletonData& sourceSkeleton,
            const SkeletonData& targetSkeleton);
    };
} // namespace OloEngine::Animation
