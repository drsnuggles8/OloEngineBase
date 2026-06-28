#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/Retargeting/SkeletonRetargetMap.h"
#include "OloEngine/Animation/Retargeting/AnimationRetargeter.h"
#include "OloEngine/Animation/Retargeting/HumanoidBone.h"
#include "OloEngine/Animation/Retargeting/HumanoidBoneMap.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/BlendNode.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string_view>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Animation;

// =============================================================================
// AnimationRetargetTest — CPU contract test for the first slice of animation
// retargeting (rotation-based, name-mapped). Pins the bone-name mapping, the
// bind-pose-relative rotation re-basing, target-proportion preservation, and the
// end-to-end "clip authored for skeleton A drives a mesh skinned to skeleton B"
// path through the real AnimationSystem::Update.
//
// Floating-point rule: never compare quats/vecs with ==. Quaternions are
// compared up to the q/-q double cover (same rotation, opposite sign).
// =============================================================================

namespace
{
    void ExpectQuatNear(const glm::quat& a, const glm::quat& b, f32 eps, std::string_view msg)
    {
        // q and -q are the same rotation — align sign before comparing components.
        const glm::quat bb = (glm::dot(a, b) < 0.0f) ? -b : b;
        EXPECT_NEAR(a.w, bb.w, eps) << msg;
        EXPECT_NEAR(a.x, bb.x, eps) << msg;
        EXPECT_NEAR(a.y, bb.y, eps) << msg;
        EXPECT_NEAR(a.z, bb.z, eps) << msg;
    }

    void ExpectVec3Near(const glm::vec3& a, const glm::vec3& b, f32 eps, std::string_view msg)
    {
        EXPECT_NEAR(a.x, b.x, eps) << msg;
        EXPECT_NEAR(a.y, b.y, eps) << msg;
        EXPECT_NEAR(a.z, b.z, eps) << msg;
    }

    // Reference re-base used to compute expected values (mirrors the production
    // formula): express the source's animated rotation as a delta from its rest
    // pose, then apply on top of the target rest rotation.
    glm::quat ExpectedRebase(const glm::quat& srcRest, const glm::quat& srcAnim, const glm::quat& tgtRest)
    {
        return glm::normalize(tgtRest * (glm::inverse(srcRest) * srcAnim));
    }

    BoneTransform MakeBone(const glm::vec3& t, const glm::quat& r = glm::identity<glm::quat>(),
                           const glm::vec3& s = glm::vec3(1.0f))
    {
        return BoneTransform{ t, r, s };
    }

    // Build a full Skeleton from per-bone local TRS, compute its global transforms
    // via forward kinematics, and capture the bind pose (what the loaders do).
    Ref<Skeleton> BuildSkeleton(const std::vector<std::string>& names,
                                const std::vector<int>& parents,
                                const std::vector<BoneTransform>& localTRS)
    {
        const sizet n = names.size();
        auto skeleton = Ref<Skeleton>::Create(n);
        skeleton->m_BoneNames = names;
        skeleton->m_ParentIndices = parents;
        for (sizet i = 0; i < n; ++i)
        {
            skeleton->m_LocalTransforms[i] = glm::translate(glm::mat4(1.0f), localTRS[i].Translation) *
                                             glm::mat4_cast(localTRS[i].Rotation) *
                                             glm::scale(glm::mat4(1.0f), localTRS[i].Scale);
            skeleton->m_BonePreTransforms[i] = glm::mat4(1.0f);
        }
        for (sizet i = 0; i < n; ++i)
        {
            const int p = parents[i];
            skeleton->m_GlobalTransforms[i] = (p >= 0)
                                                  ? skeleton->m_GlobalTransforms[p] * skeleton->m_LocalTransforms[i]
                                                  : skeleton->m_LocalTransforms[i];
        }
        skeleton->SetBindPose();
        return skeleton;
    }
} // namespace

// -----------------------------------------------------------------------------
// NormalizeBoneName — namespace/rig prefix stripped, separators dropped, folded.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, NormalizeBoneNameCollapsesCommonVariants)
{
    EXPECT_EQ(SkeletonRetargetMap::NormalizeBoneName("mixamorig:LeftArm"), "leftarm");
    EXPECT_EQ(SkeletonRetargetMap::NormalizeBoneName("Armature|Left_Arm"), "leftarm");
    EXPECT_EQ(SkeletonRetargetMap::NormalizeBoneName("Left_Arm"), "leftarm");
    EXPECT_EQ(SkeletonRetargetMap::NormalizeBoneName("left arm"), "leftarm");
    EXPECT_EQ(SkeletonRetargetMap::NormalizeBoneName("RightHand"), "righthand");
    // Digits survive; biped-style names normalize but won't match a "thigh_l" rig
    // (that's the documented humanoid-enum follow-up, not name normalization).
    EXPECT_EQ(SkeletonRetargetMap::NormalizeBoneName("Bip01 L Thigh"), "bip01lthigh");
}

// -----------------------------------------------------------------------------
// BuildByName — exact match preferred, normalized fallback, unmatched left blank.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, BuildByNameResolvesExactAndNormalized)
{
    SkeletonData source;
    source.m_BoneNames = { "mixamorig:Hips", "mixamorig:Spine", "mixamorig:LeftArm", "mixamorig:RightArm" };
    SkeletonData target;
    target.m_BoneNames = { "Hips", "Spine", "Left_Arm", "Tail" };

    const SkeletonRetargetMap map = SkeletonRetargetMap::BuildByName(source, target);

    EXPECT_EQ(map.GetSourceBone(0), 0) << "Hips should normalize-match mixamorig:Hips";
    EXPECT_EQ(map.GetSourceBone(1), 1) << "Spine should normalize-match mixamorig:Spine";
    EXPECT_EQ(map.GetSourceBone(2), 2) << "Left_Arm should normalize-match mixamorig:LeftArm";
    EXPECT_EQ(map.GetSourceBone(3), SkeletonRetargetMap::kUnmapped) << "Tail has no source bone";
    EXPECT_FALSE(map.HasMapping(3));
    EXPECT_EQ(map.GetMappedBoneCount(), 3u);
    EXPECT_EQ(map.GetTargetBoneCount(), 4u);
}

TEST(AnimationRetarget, BuildByNamePrefersExactOverNormalizedCollision)
{
    // Two source bones collide on the normalized key "root"; an exact target match
    // must win over the first normalized occurrence.
    SkeletonData source;
    source.m_BoneNames = { "root", "Root" };
    SkeletonData target;
    target.m_BoneNames = { "Root" };

    const SkeletonRetargetMap map = SkeletonRetargetMap::BuildByName(source, target);
    EXPECT_EQ(map.GetSourceBone(0), 1) << "exact 'Root' must beat the normalized 'root' collision";
}

TEST(AnimationRetarget, BuildByNameIgnoresEmptyNormalizedNames)
{
    // Names that are only a namespace/rig prefix or separators normalize to "".
    // Such names must NOT collide on a shared empty bucket and produce a spurious
    // match between unrelated bones.
    SkeletonData source;
    source.m_BoneNames = { "Hips", "mixamorig:" }; // index 1 normalizes to ""
    SkeletonData target;
    target.m_BoneNames = { "Pelvis", "Armature|" }; // index 1 also normalizes to ""

    const SkeletonRetargetMap map = SkeletonRetargetMap::BuildByName(source, target);
    EXPECT_EQ(map.GetSourceBone(0), SkeletonRetargetMap::kUnmapped) << "Pelvis has no Hips match";
    EXPECT_EQ(map.GetSourceBone(1), SkeletonRetargetMap::kUnmapped)
        << "an empty-normalized target must not match an empty-normalized source";
    EXPECT_EQ(map.GetMappedBoneCount(), 0u);
}

TEST(AnimationRetarget, SetBoneMappingNormalizesInvalidNegative)
{
    // An out-of-range negative source index must collapse to kUnmapped so the
    // mapping metadata (HasMapping / GetMappedBoneCount) stays consistent.
    SkeletonRetargetMap map;
    map.SetBoneMapping(0, 2);  // valid
    map.SetBoneMapping(1, -5); // invalid negative -> kUnmapped

    EXPECT_EQ(map.GetSourceBone(0), 2);
    EXPECT_EQ(map.GetSourceBone(1), SkeletonRetargetMap::kUnmapped);
    EXPECT_FALSE(map.HasMapping(1));
    EXPECT_EQ(map.GetMappedBoneCount(), 1u) << "an invalid negative must not be counted as mapped";

    // kUnmapped itself clears a previously-set mapping.
    map.SetBoneMapping(0, SkeletonRetargetMap::kUnmapped);
    EXPECT_FALSE(map.HasMapping(0));
    EXPECT_EQ(map.GetMappedBoneCount(), 0u);
}

// -----------------------------------------------------------------------------
// RetargetLocalPose — rotation transferred, target proportions preserved.
// Source bone lengths are 1, target bone lengths are 2; after retargeting the
// target keeps its OWN lengths (rotation only is transferred).
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, RetargetLocalPosePreservesTargetProportions)
{
    const std::vector<BoneTransform> sourceRest = {
        MakeBone({ 0.0f, 0.0f, 0.0f }), MakeBone({ 0.0f, 1.0f, 0.0f }), MakeBone({ 0.0f, 1.0f, 0.0f })
    };
    const std::vector<BoneTransform> targetRest = {
        MakeBone({ 0.0f, 0.0f, 0.0f }), MakeBone({ 0.0f, 2.0f, 0.0f }), MakeBone({ 0.0f, 2.0f, 0.0f })
    };

    const glm::quat bend = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    std::vector<BoneTransform> sourcePose = sourceRest;
    sourcePose[1].Rotation = bend; // animate the middle bone

    SkeletonRetargetMap map; // identity map 0->0, 1->1, 2->2
    map.SetBoneMapping(0, 0);
    map.SetBoneMapping(1, 1);
    map.SetBoneMapping(2, 2);

    RetargetOptions options;
    options.RootBoneIndex = 0;

    std::vector<BoneTransform> out(targetRest.size());
    AnimationRetargeter::RetargetLocalPose(map, sourcePose, sourceRest, targetRest, options, out);

    // Rotations: identity rest on both sides => direct copy.
    ExpectQuatNear(out[1].Rotation, bend, 1e-4f, "mapped bone should receive the source rotation");
    ExpectQuatNear(out[0].Rotation, glm::identity<glm::quat>(), 1e-4f, "unanimated bone stays at rest rotation");

    // Translations: target keeps its own (length 2), NOT the source's (length 1).
    ExpectVec3Near(out[1].Translation, glm::vec3(0.0f, 2.0f, 0.0f), 1e-5f, "target proportions must be preserved");
    ExpectVec3Near(out[2].Translation, glm::vec3(0.0f, 2.0f, 0.0f), 1e-5f, "target proportions must be preserved");
    // Root has no source motion => stays at target rest translation.
    ExpectVec3Near(out[0].Translation, glm::vec3(0.0f, 0.0f, 0.0f), 1e-5f, "root with no motion stays at rest");
}

// -----------------------------------------------------------------------------
// RetargetLocalPose — differing rest orientations exercise the delta re-base,
// proving this is NOT a naive local-rotation copy.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, RetargetLocalPoseRebasesDifferingRestOrientations)
{
    const glm::quat srcRestRot = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat tgtRestRot = glm::angleAxis(glm::radians(-50.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat delta = glm::angleAxis(glm::radians(20.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    const std::vector<BoneTransform> sourceRest = { MakeBone({}, srcRestRot) };
    const std::vector<BoneTransform> targetRest = { MakeBone({}, tgtRestRot) };
    std::vector<BoneTransform> sourcePose = { MakeBone({}, glm::normalize(srcRestRot * delta)) };

    SkeletonRetargetMap map;
    map.SetBoneMapping(0, 0);

    RetargetOptions options;
    options.RetargetRootTranslation = false;

    std::vector<BoneTransform> out(1);
    AnimationRetargeter::RetargetLocalPose(map, sourcePose, sourceRest, targetRest, options, out);

    const glm::quat expected = ExpectedRebase(srcRestRot, sourcePose[0].Rotation, tgtRestRot);
    ExpectQuatNear(out[0].Rotation, expected, 1e-4f, "rotation must be re-based onto the target rest pose");

    // Sanity: a naive copy (out == source animated rotation) would be wrong here.
    EXPECT_LT(std::abs(glm::dot(out[0].Rotation, sourcePose[0].Rotation)), 0.999f)
        << "re-based rotation must differ from a naive source-rotation copy";
}

// -----------------------------------------------------------------------------
// RetargetLocalPose — root translation transferred with the configured scale.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, RetargetLocalPoseTransfersScaledRootTranslation)
{
    const std::vector<BoneTransform> sourceRest = { MakeBone({ 0.0f, 1.0f, 0.0f }) };
    const std::vector<BoneTransform> targetRest = { MakeBone({ 0.0f, 3.0f, 0.0f }) };
    std::vector<BoneTransform> sourcePose = { MakeBone({ 0.0f, 1.5f, 0.4f }) }; // delta (0, .5, .4)

    SkeletonRetargetMap map;
    map.SetBoneMapping(0, 0);

    RetargetOptions options;
    options.RetargetRootTranslation = true;
    options.RootTranslationScale = 2.0f;
    options.RootBoneIndex = 0;

    std::vector<BoneTransform> out(1);
    AnimationRetargeter::RetargetLocalPose(map, sourcePose, sourceRest, targetRest, options, out);

    // target rest (0,3,0) + 2 * delta(0,.5,.4) = (0, 4, 0.8)
    ExpectVec3Near(out[0].Translation, glm::vec3(0.0f, 4.0f, 0.8f), 1e-5f, "scaled root translation delta");
}

// -----------------------------------------------------------------------------
// End-to-end: a clip authored for the SOURCE skeleton, baked via RetargetClip,
// drives the TARGET skeleton through the real AnimationSystem::Update. Verifies
// the mapped bone's re-based rotation, the root translation transfer, preserved
// proportions, and that unmapped/untracked bones keep their rest pose.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, RetargetClipDrivesTargetSkeletonViaAnimationSystem)
{
    const glm::quat srcArmRest = glm::angleAxis(glm::radians(15.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat tgtArmRest = glm::angleAxis(glm::radians(-25.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::quat armDelta = glm::angleAxis(glm::radians(40.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // Source rig: Hips -> Spine -> LeftArm, mixamo-style names, bone length 1.
    auto source = BuildSkeleton(
        { "mixamorig:Hips", "mixamorig:Spine", "mixamorig:LeftArm" },
        { -1, 0, 1 },
        { MakeBone({ 0.0f, 1.0f, 0.0f }), MakeBone({ 0.0f, 1.0f, 0.0f }), MakeBone({ 0.0f, 1.0f, 0.0f }, srcArmRest) });

    // Target rig: differing names + proportions (bone length 2) + arm rest pose.
    auto target = BuildSkeleton(
        { "Hips", "Spine", "Left_Arm" },
        { -1, 0, 1 },
        { MakeBone({ 0.0f, 2.0f, 0.0f }), MakeBone({ 0.0f, 2.0f, 0.0f }), MakeBone({ 0.0f, 2.0f, 0.0f }, tgtArmRest) });

    // Source clip: arm rotated by srcArmRest*armDelta (single key); hips translated
    // (single key) to exercise the root-translation transfer.
    auto clip = Ref<AnimationClip>::Create();
    clip->Name = "Walk";
    clip->Duration = 1.0f;
    {
        BoneAnimation arm;
        arm.BoneName = "mixamorig:LeftArm";
        arm.RotationKeys.push_back({ 0.0, glm::normalize(srcArmRest * armDelta) });
        clip->BoneAnimations.push_back(std::move(arm));

        BoneAnimation hips;
        hips.BoneName = "mixamorig:Hips";
        hips.PositionKeys.push_back({ 0.0, glm::vec3(0.0f, 1.5f, 0.3f) }); // rest is (0,1,0) => delta (0,.5,.3)
        clip->BoneAnimations.push_back(std::move(hips));
        clip->InitializeBoneCache();
    }

    const SkeletonRetargetMap map = SkeletonRetargetMap::BuildByName(*source, *target);
    ASSERT_EQ(map.GetMappedBoneCount(), 3u);

    RetargetOptions options;
    options.RetargetRootTranslation = true;
    options.RootTranslationScale = 2.0f;

    Ref<AnimationClip> baked = AnimationRetargeter::RetargetClip(clip, *source, *target, map, options);
    ASSERT_TRUE(baked);
    // Only Hips (position track) and LeftArm (rotation track) bake; Spine has no
    // source track and is skipped.
    ASSERT_EQ(baked->BoneAnimations.size(), 2u);
    EXPECT_TRUE(baked->FindBoneAnimation("Hips"));
    EXPECT_TRUE(baked->FindBoneAnimation("Left_Arm"));
    EXPECT_FALSE(baked->FindBoneAnimation("Spine"));

    // Play the baked clip on the TARGET skeleton through the real system.
    AnimationStateComponent animState;
    animState.m_CurrentClip = baked;
    animState.m_CurrentTime = 0.0f;
    animState.m_IsPlaying = true;
    AnimationSystem::Update(animState, *target, 0.1f);

    // LeftArm (index 2): rotation re-based onto the target rest pose; translation
    // preserved at the target rest (proportions kept).
    const BoneTransform arm = BlendUtils::DecomposeMatrix(target->m_LocalTransforms[2]);
    const glm::quat expectedArm = ExpectedRebase(srcArmRest, glm::normalize(srcArmRest * armDelta), tgtArmRest);
    ExpectQuatNear(arm.Rotation, expectedArm, 2e-4f, "retargeted arm rotation through AnimationSystem");
    ExpectVec3Near(arm.Translation, glm::vec3(0.0f, 2.0f, 0.0f), 1e-4f, "arm keeps target rest translation");

    // Hips (index 0, root): translation = target rest (0,2,0) + 2 * delta(0,.5,.3).
    const BoneTransform hips = BlendUtils::DecomposeMatrix(target->m_LocalTransforms[0]);
    ExpectVec3Near(hips.Translation, glm::vec3(0.0f, 3.0f, 0.6f), 1e-4f, "retargeted scaled root translation");

    // Spine (index 1): no baked track => keeps its bind-pose rest local transform.
    const BoneTransform spine = BlendUtils::DecomposeMatrix(target->m_LocalTransforms[1]);
    ExpectVec3Near(spine.Translation, glm::vec3(0.0f, 2.0f, 0.0f), 1e-5f, "untargeted bone keeps rest translation");
    ExpectQuatNear(spine.Rotation, glm::identity<glm::quat>(), 1e-5f, "untargeted bone keeps rest rotation");
}

// -----------------------------------------------------------------------------
// RetargetClip is a no-op-safe on a null source clip.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, RetargetClipReturnsNullForNullSource)
{
    SkeletonData source;
    SkeletonData target;
    const SkeletonRetargetMap map = SkeletonRetargetMap::BuildByName(source, target);
    EXPECT_EQ(AnimationRetargeter::RetargetClip(nullptr, source, target, map), nullptr);
}

// -----------------------------------------------------------------------------
// ComputeRootTranslationScale — ratio of rig extents, with a safe degenerate path.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, ComputeRootTranslationScaleFromRigExtents)
{
    // Source tallest bind-pose offset = 3, target = 6 => scale 2.
    auto source = BuildSkeleton({ "Root", "Tip" }, { -1, 0 },
                                { MakeBone({ 0.0f, 0.0f, 0.0f }), MakeBone({ 0.0f, 3.0f, 0.0f }) });
    auto target = BuildSkeleton({ "Root", "Tip" }, { -1, 0 },
                                { MakeBone({ 0.0f, 0.0f, 0.0f }), MakeBone({ 0.0f, 6.0f, 0.0f }) });

    EXPECT_NEAR(AnimationRetargeter::ComputeRootTranslationScale(*source, *target), 2.0f, 1e-4f);

    // Degenerate source (all bones at origin) => safe 1.0 fallback.
    auto degenerate = BuildSkeleton({ "Root" }, { -1 }, { MakeBone({ 0.0f, 0.0f, 0.0f }) });
    EXPECT_NEAR(AnimationRetargeter::ComputeRootTranslationScale(*degenerate, *target), 1.0f, 1e-4f);
}

// =============================================================================
// Humanoid bone-enum mapping — relate anatomically-equivalent bones across rigs
// whose names share nothing (the deferred-item-#1 follow-up to name mapping).
// =============================================================================

// -----------------------------------------------------------------------------
// ClassifyBoneName — one anatomical role recognised across four rig naming
// conventions, with helper/finger/sideless bones rejected.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, HumanoidClassifyAcrossNamingConventions)
{
    using HB = HumanoidBone;
    const auto classify = [](std::string_view n)
    { return HumanoidBoneMap::ClassifyBoneName(n); };

    // Mixamo (bare "Arm"/"Leg" are the UPPER arm / LOWER leg by convention).
    EXPECT_EQ(classify("mixamorig:Hips"), HB::Hips);
    EXPECT_EQ(classify("mixamorig:LeftArm"), HB::LeftUpperArm);
    EXPECT_EQ(classify("mixamorig:LeftForeArm"), HB::LeftLowerArm);
    EXPECT_EQ(classify("mixamorig:RightHand"), HB::RightHand);
    EXPECT_EQ(classify("mixamorig:LeftUpLeg"), HB::LeftUpperLeg);
    EXPECT_EQ(classify("mixamorig:LeftLeg"), HB::LeftLowerLeg);
    EXPECT_EQ(classify("mixamorig:RightToeBase"), HB::RightToes);

    // Unreal Engine mannequin.
    EXPECT_EQ(classify("pelvis"), HB::Hips);
    EXPECT_EQ(classify("clavicle_l"), HB::LeftShoulder);
    EXPECT_EQ(classify("upperarm_l"), HB::LeftUpperArm);
    EXPECT_EQ(classify("lowerarm_r"), HB::RightLowerArm);
    EXPECT_EQ(classify("thigh_r"), HB::RightUpperLeg);
    EXPECT_EQ(classify("calf_l"), HB::LeftLowerLeg);
    EXPECT_EQ(classify("foot_l"), HB::LeftFoot);
    EXPECT_EQ(classify("ball_r"), HB::RightToes);

    // 3ds Max Biped (space-separated, "Bip01 <Side> <Part>").
    EXPECT_EQ(classify("Bip01 Pelvis"), HB::Hips);
    EXPECT_EQ(classify("Bip01 L UpperArm"), HB::LeftUpperArm);
    EXPECT_EQ(classify("Bip01 R Forearm"), HB::RightLowerArm);
    EXPECT_EQ(classify("Bip01 L Thigh"), HB::LeftUpperLeg);
    EXPECT_EQ(classify("Bip01 R Calf"), HB::RightLowerLeg);
    EXPECT_EQ(classify("Bip01 L Foot"), HB::LeftFoot);

    // Blender / Rigify (".L"/".R" suffix).
    EXPECT_EQ(classify("upper_arm.L"), HB::LeftUpperArm);
    EXPECT_EQ(classify("forearm.R"), HB::RightLowerArm);
    EXPECT_EQ(classify("shin.L"), HB::LeftLowerLeg);
    EXPECT_EQ(classify("foot.R"), HB::RightFoot);

    // All four spell LeftUpperArm identically — the whole point of the role enum.
    EXPECT_EQ(classify("mixamorig:LeftArm"), classify("upperarm_l"));
    EXPECT_EQ(classify("upperarm_l"), classify("Bip01 L UpperArm"));
    EXPECT_EQ(classify("Bip01 L UpperArm"), classify("upper_arm.L"));

    // Helper / finger / sideless bones carry no role.
    EXPECT_EQ(classify("mixamorig:LeftHandIndex1"), HB::None) << "finger bone is not the hand";
    EXPECT_EQ(classify("upperarm_twist_01_l"), HB::None) << "twist bone is not the arm";
    EXPECT_EQ(classify("ik_hand_l"), HB::None) << "IK helper is not the hand";
    EXPECT_EQ(classify("Tail"), HB::None) << "non-humanoid bone has no role";
    EXPECT_EQ(classify("UpperArm"), HB::None) << "a limb with no detectable side can't be placed";

    EXPECT_EQ(ToString(HB::LeftUpperArm), "leftUpperArm");
    EXPECT_EQ(ToString(HB::None), "none");
}

// -----------------------------------------------------------------------------
// AutoDetect — full skeleton classified; the multi-bone spine collapses onto
// Spine (lowest) + Chest (highest), and helper / middle bones get no role.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, HumanoidAutoDetectAssignsRolesAndResolvesSpine)
{
    SkeletonData skel;
    skel.m_BoneNames = {
        "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "head", // 0..5
        "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",              // 6..9
        "thigh_r", "calf_r", "foot_r", "ball_r",                         // 10..13
        "ik_foot_root"                                                   // 14 (helper)
    };

    const HumanoidBoneMap map = HumanoidBoneMap::AutoDetect(skel);

    EXPECT_EQ(map.GetBone(HumanoidBone::Hips), 0);
    EXPECT_EQ(map.GetBone(HumanoidBone::Spine), 1) << "lowest-numbered spine -> Spine";
    EXPECT_EQ(map.GetBone(HumanoidBone::Chest), 3) << "highest spine -> Chest (no explicit chest bone)";
    EXPECT_EQ(map.GetBone(HumanoidBone::Neck), 4);
    EXPECT_EQ(map.GetBone(HumanoidBone::Head), 5);
    EXPECT_EQ(map.GetBone(HumanoidBone::LeftShoulder), 6);
    EXPECT_EQ(map.GetBone(HumanoidBone::LeftUpperArm), 7);
    EXPECT_EQ(map.GetBone(HumanoidBone::LeftLowerArm), 8);
    EXPECT_EQ(map.GetBone(HumanoidBone::LeftHand), 9);
    EXPECT_EQ(map.GetBone(HumanoidBone::RightUpperLeg), 10);
    EXPECT_EQ(map.GetBone(HumanoidBone::RightLowerLeg), 11);
    EXPECT_EQ(map.GetBone(HumanoidBone::RightFoot), 12);
    EXPECT_EQ(map.GetBone(HumanoidBone::RightToes), 13);

    EXPECT_EQ(map.GetRole(2), HumanoidBone::None) << "the middle spine bone is left unassigned";
    EXPECT_EQ(map.GetRole(14), HumanoidBone::None) << "the IK helper bone gets no role";
    EXPECT_EQ(map.GetRole(7), HumanoidBone::LeftUpperArm) << "reverse lookup agrees";

    // 6 center/spine roles resolved (Hips, Spine, Chest, Neck, Head) — UpperChest
    // absent — plus 8 limb roles = 13 assigned.
    EXPECT_EQ(map.GetAssignedRoleCount(), 13u);
    EXPECT_FALSE(map.HasRole(HumanoidBone::UpperChest));
}

// -----------------------------------------------------------------------------
// BuildByHumanoidRole — two rigs with ZERO name overlap map anatomically where
// name matching finds nothing.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, BuildByHumanoidRoleMapsDisjointlyNamedRigs)
{
    SkeletonData source; // Unreal naming
    source.m_BoneNames = { "pelvis", "upperarm_l", "lowerarm_l" };
    SkeletonData target; // Mixamo naming — shares no normalized name with the source
    target.m_BoneNames = { "mixamorig:Hips", "mixamorig:LeftArm", "mixamorig:LeftForeArm" };

    EXPECT_EQ(SkeletonRetargetMap::BuildByName(source, target).GetMappedBoneCount(), 0u)
        << "the rigs share no name — name matching must find nothing";

    const SkeletonRetargetMap byRole = SkeletonRetargetMap::BuildByHumanoidRole(source, target);
    EXPECT_EQ(byRole.GetMappedBoneCount(), 3u);
    EXPECT_EQ(byRole.GetSourceBone(0), 0) << "Hips <- pelvis";
    EXPECT_EQ(byRole.GetSourceBone(1), 1) << "LeftArm (upper) <- upperarm_l";
    EXPECT_EQ(byRole.GetSourceBone(2), 2) << "LeftForeArm (lower) <- lowerarm_l";
}

// -----------------------------------------------------------------------------
// Explicit override + name fallback compose: a custom-named source bone the
// heuristic can't classify is supplied by hand, and a same-named non-humanoid
// bone is picked up by FillUnmappedFrom(BuildByName(...)).
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, HumanoidExplicitOverrideAndNameFallbackCompose)
{
    SkeletonData source;
    source.m_BoneNames = { "pelvis", "CustomBone1", "Tail" }; // index 1 unclassifiable
    SkeletonData target;
    target.m_BoneNames = { "mixamorig:Hips", "mixamorig:LeftArm", "Tail" };

    HumanoidBoneMap sourceRoles = HumanoidBoneMap::AutoDetect(source);
    const HumanoidBoneMap targetRoles = HumanoidBoneMap::AutoDetect(target);

    ASSERT_FALSE(sourceRoles.HasRole(HumanoidBone::LeftUpperArm)) << "the custom name isn't auto-detected";
    sourceRoles.SetBone(HumanoidBone::LeftUpperArm, 1); // hand-authored override

    SkeletonRetargetMap map = SkeletonRetargetMap::BuildByHumanoidRole(source, target, sourceRoles, targetRoles);
    EXPECT_EQ(map.GetSourceBone(0), 0) << "Hips by role";
    EXPECT_EQ(map.GetSourceBone(1), 1) << "LeftArm by overridden role";
    EXPECT_EQ(map.GetSourceBone(2), SkeletonRetargetMap::kUnmapped) << "Tail has no humanoid role";

    map.FillUnmappedFrom(SkeletonRetargetMap::BuildByName(source, target));
    EXPECT_EQ(map.GetSourceBone(2), 2) << "Tail resolved by the name fallback";
    EXPECT_EQ(map.GetMappedBoneCount(), 3u);
}

// -----------------------------------------------------------------------------
// End-to-end: a clip authored for an Unreal-named SOURCE retargets, via the
// humanoid ROLE map, onto a disjointly Mixamo-named TARGET and drives it through
// the real AnimationSystem::Update — names never matched, roles did.
// -----------------------------------------------------------------------------
TEST(AnimationRetarget, RetargetClipViaHumanoidRoleDrivesDisjointTarget)
{
    const glm::quat srcArmRest = glm::angleAxis(glm::radians(15.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat tgtArmRest = glm::angleAxis(glm::radians(-25.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::quat armDelta = glm::angleAxis(glm::radians(40.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // Source: Unreal names, bone length 1.
    auto source = BuildSkeleton(
        { "pelvis", "spine_01", "upperarm_l" }, { -1, 0, 1 },
        { MakeBone({ 0.0f, 1.0f, 0.0f }), MakeBone({ 0.0f, 1.0f, 0.0f }), MakeBone({ 0.0f, 1.0f, 0.0f }, srcArmRest) });

    // Target: Mixamo names (zero overlap with the source), bone length 2 + arm rest.
    auto target = BuildSkeleton(
        { "mixamorig:Hips", "mixamorig:Spine", "mixamorig:LeftArm" }, { -1, 0, 1 },
        { MakeBone({ 0.0f, 2.0f, 0.0f }), MakeBone({ 0.0f, 2.0f, 0.0f }), MakeBone({ 0.0f, 2.0f, 0.0f }, tgtArmRest) });

    ASSERT_EQ(SkeletonRetargetMap::BuildByName(*source, *target).GetMappedBoneCount(), 0u)
        << "names must not match — this proves the role path";
    const SkeletonRetargetMap map = SkeletonRetargetMap::BuildByHumanoidRole(*source, *target);
    ASSERT_EQ(map.GetMappedBoneCount(), 3u);

    auto clip = Ref<AnimationClip>::Create();
    clip->Name = "Walk";
    clip->Duration = 1.0f;
    {
        BoneAnimation arm;
        arm.BoneName = "upperarm_l";
        arm.RotationKeys.push_back({ 0.0, glm::normalize(srcArmRest * armDelta) });
        clip->BoneAnimations.push_back(std::move(arm));

        BoneAnimation hips;
        hips.BoneName = "pelvis";
        hips.PositionKeys.push_back({ 0.0, glm::vec3(0.0f, 1.5f, 0.3f) }); // rest (0,1,0) => delta (0,.5,.3)
        clip->BoneAnimations.push_back(std::move(hips));
        clip->InitializeBoneCache();
    }

    RetargetOptions options;
    options.RetargetRootTranslation = true;
    options.RootTranslationScale = 2.0f;

    Ref<AnimationClip> baked = AnimationRetargeter::RetargetClip(clip, *source, *target, map, options);
    ASSERT_TRUE(baked);
    EXPECT_TRUE(baked->FindBoneAnimation("mixamorig:LeftArm")) << "baked track named for the target bone";
    EXPECT_TRUE(baked->FindBoneAnimation("mixamorig:Hips"));

    AnimationStateComponent animState;
    animState.m_CurrentClip = baked;
    animState.m_CurrentTime = 0.0f;
    animState.m_IsPlaying = true;
    AnimationSystem::Update(animState, *target, 0.1f);

    // LeftArm (index 2): rotation re-based onto the target rest; proportions kept.
    const BoneTransform arm = BlendUtils::DecomposeMatrix(target->m_LocalTransforms[2]);
    const glm::quat expectedArm = ExpectedRebase(srcArmRest, glm::normalize(srcArmRest * armDelta), tgtArmRest);
    ExpectQuatNear(arm.Rotation, expectedArm, 2e-4f, "role-retargeted arm rotation through AnimationSystem");
    ExpectVec3Near(arm.Translation, glm::vec3(0.0f, 2.0f, 0.0f), 1e-4f, "arm keeps target rest translation");

    // Hips (index 0, root): target rest (0,2,0) + 2 * delta(0,.5,.3) = (0,3,0.6).
    const BoneTransform hips = BlendUtils::DecomposeMatrix(target->m_LocalTransforms[0]);
    ExpectVec3Near(hips.Translation, glm::vec3(0.0f, 3.0f, 0.6f), 1e-4f, "role-retargeted scaled root translation");
}
