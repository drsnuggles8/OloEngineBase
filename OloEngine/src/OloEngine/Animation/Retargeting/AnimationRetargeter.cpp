#include "OloEnginePCH.h"
#include "OloEngine/Animation/Retargeting/AnimationRetargeter.h"

#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendUtils.h"

#include <algorithm>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/geometric.hpp>

namespace OloEngine::Animation
{
    namespace
    {
        // Express the source bone's animated rotation as a delta from its rest pose
        // (in the bone's own local frame), then re-base that delta onto the target's
        // rest rotation. When both rest rotations are identity this reduces to a
        // direct copy of the source rotation.
        glm::quat RebaseRotation(const glm::quat& sourceRest, const glm::quat& sourceAnim, const glm::quat& targetRest)
        {
            const glm::quat delta = glm::inverse(sourceRest) * sourceAnim;
            return glm::normalize(targetRest * delta);
        }

        // Decompose a skeleton's bind-pose LOCAL transforms into per-bone TRS.
        std::vector<BoneTransform> RestLocalPose(const SkeletonData& skeleton)
        {
            const sizet boneCount = skeleton.m_BoneNames.size();
            std::vector<BoneTransform> rest(boneCount);
            for (sizet i = 0; i < boneCount; ++i)
            {
                if (i < skeleton.m_BindPoseLocalTransforms.size())
                    rest[i] = BlendUtils::DecomposeMatrix(skeleton.m_BindPoseLocalTransforms[i]);
            }
            return rest;
        }

        // First bone whose parent index is -1, or 0 when none / no parent table.
        int FirstRootBone(const SkeletonData& skeleton)
        {
            for (sizet i = 0; i < skeleton.m_ParentIndices.size(); ++i)
            {
                if (skeleton.m_ParentIndices[i] < 0)
                    return static_cast<int>(i);
            }
            return 0;
        }
    } // namespace

    void AnimationRetargeter::RetargetLocalPose(
        const SkeletonRetargetMap& map,
        std::span<const BoneTransform> sourcePose,
        std::span<const BoneTransform> sourceRestLocal,
        std::span<const BoneTransform> targetRestLocal,
        const RetargetOptions& options,
        std::span<BoneTransform> outTargetPose)
    {
        OLO_PROFILE_FUNCTION();

        const sizet targetCount = targetRestLocal.size();
        OLO_CORE_ASSERT(outTargetPose.size() == targetCount,
                        "outTargetPose must be sized to the target bone count");

        const int rootBone = (options.RootBoneIndex == RetargetOptions::kUseFirstRoot) ? 0 : options.RootBoneIndex;

        // Clamp the iteration to both spans so a mis-sized outTargetPose can never
        // write out of bounds — OLO_CORE_ASSERT above compiles out in non-debug
        // builds, so the bound must also hold at runtime.
        const sizet count = std::min(targetCount, outTargetPose.size());
        for (sizet t = 0; t < count; ++t)
        {
            const BoneTransform& targetRest = targetRestLocal[t];

            // Default: the target's own rest transform. Covers unmapped bones and,
            // for mapped bones, supplies the preserved translation/scale.
            BoneTransform& out = outTargetPose[t];
            out = targetRest;

            const int s = map.GetSourceBone(static_cast<int>(t));
            if (s == SkeletonRetargetMap::kUnmapped)
                continue;
            if (static_cast<sizet>(s) >= sourcePose.size() || static_cast<sizet>(s) >= sourceRestLocal.size())
                continue; // defensive: map references a source bone outside the pose

            out.Rotation = RebaseRotation(sourceRestLocal[s].Rotation, sourcePose[s].Rotation, targetRest.Rotation);

            if (options.RetargetRootTranslation && static_cast<int>(t) == rootBone)
            {
                const glm::vec3 srcDelta = sourcePose[s].Translation - sourceRestLocal[s].Translation;
                out.Translation = targetRest.Translation + options.RootTranslationScale * srcDelta;
            }
        }
    }

    Ref<AnimationClip> AnimationRetargeter::RetargetClip(
        const Ref<AnimationClip>& sourceClip,
        const SkeletonData& sourceSkeleton,
        const SkeletonData& targetSkeleton,
        const SkeletonRetargetMap& map,
        const RetargetOptions& options)
    {
        if (!sourceClip)
            return nullptr;

        const std::vector<BoneTransform> srcRest = RestLocalPose(sourceSkeleton);
        const std::vector<BoneTransform> tgtRest = RestLocalPose(targetSkeleton);

        const int rootBone = (options.RootBoneIndex == RetargetOptions::kUseFirstRoot)
                                 ? FirstRootBone(targetSkeleton)
                                 : options.RootBoneIndex;

        auto out = Ref<AnimationClip>::Create();
        out->Name = sourceClip->Name + "_Retargeted";
        out->Duration = sourceClip->Duration;

        for (sizet t = 0; t < targetSkeleton.m_BoneNames.size(); ++t)
        {
            const int s = map.GetSourceBone(static_cast<int>(t));
            if (s == SkeletonRetargetMap::kUnmapped || static_cast<sizet>(s) >= sourceSkeleton.m_BoneNames.size())
                continue;

            const BoneAnimation* srcAnim = sourceClip->FindBoneAnimation(sourceSkeleton.m_BoneNames[s]);
            if (!srcAnim)
                continue; // source has no track for this bone — target keeps its rest pose

            const BoneTransform& srcRestBone = srcRest[s];
            const BoneTransform& tgtRestBone = tgtRest[t];

            BoneAnimation dst;
            dst.BoneName = targetSkeleton.m_BoneNames[t];

            // Rotation: re-base each source key onto the target rest pose.
            if (srcAnim->RotationKeys.empty())
            {
                dst.RotationKeys.push_back({ 0.0, tgtRestBone.Rotation });
            }
            else
            {
                dst.RotationKeys.reserve(srcAnim->RotationKeys.size());
                for (const auto& key : srcAnim->RotationKeys)
                {
                    dst.RotationKeys.push_back(
                        { key.Time, RebaseRotation(srcRestBone.Rotation, key.Rotation, tgtRestBone.Rotation) });
                }
            }

            // Translation: target rest by default; transfer the scaled root delta for
            // the root bone so locomotion / hip motion carries over.
            const bool transferTranslation =
                options.RetargetRootTranslation && static_cast<int>(t) == rootBone && !srcAnim->PositionKeys.empty();
            if (transferTranslation)
            {
                dst.PositionKeys.reserve(srcAnim->PositionKeys.size());
                for (const auto& key : srcAnim->PositionKeys)
                {
                    const glm::vec3 delta = key.Position - srcRestBone.Translation;
                    dst.PositionKeys.push_back({ key.Time, tgtRestBone.Translation + options.RootTranslationScale * delta });
                }
            }
            else
            {
                // A single constant key keeps the bone at its target rest translation
                // (an empty PositionKeys list would otherwise sample to the origin).
                dst.PositionKeys.push_back({ 0.0, tgtRestBone.Translation });
            }

            // Scale: keep the target's rest scale (proportions preserved).
            dst.ScaleKeys.push_back({ 0.0, tgtRestBone.Scale });

            out->BoneAnimations.push_back(std::move(dst));
        }

        out->InitializeBoneCache();
        return out;
    }

    f32 AnimationRetargeter::ComputeRootTranslationScale(
        const SkeletonData& sourceSkeleton,
        const SkeletonData& targetSkeleton)
    {
        auto rigExtent = [](const SkeletonData& skeleton) -> f32
        {
            f32 maxLen = 0.0f;
            for (const glm::mat4& m : skeleton.m_BindPoseMatrices)
                maxLen = std::max(maxLen, glm::length(glm::vec3(m[3])));
            return maxLen;
        };

        const f32 srcExtent = rigExtent(sourceSkeleton);
        const f32 tgtExtent = rigExtent(targetSkeleton);
        constexpr f32 epsilon = 1e-6f;
        if (srcExtent < epsilon)
            return 1.0f;
        return tgtExtent / srcExtent;
    }
} // namespace OloEngine::Animation
