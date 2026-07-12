// OLO_TEST_LAYER: unit
// =============================================================================
// FootIKTest — CPU contract tests for the ground-adaptation foot/hand IK
// post-pass (issue #631 part 3). The pass consumes an injected ground cache
// (in production filled Scene-side from Jolt raycasts), which makes every
// geometric contract testable without physics:
//   * ground conformance (foot pulled to ground + FootHeight, clip lift kept),
//   * pelvis lowering toward the lowest reachable target (clamped, smoothed),
//   * foot planting (lock engage on slow grounded feet, release on lift,
//     world-space persistence while locked),
//   * slope alignment of the foot bone (clamped by MaxSlopeAngle),
//   * hand IK onto a resolved target,
//   * no-ops (disabled component, no ground).
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Animation/BlendUtils.h"
#include "OloEngine/Animation/FootIKComponent.h"
#include "OloEngine/Animation/IK/FootIKPostPass.h"
#include "OloEngine/Animation/Skeleton.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::Animation
{
    namespace
    {
        // Leg rig: Hips (root, model y = 1.1) → Knee → Foot, with a slight
        // forward knee bend so the two-bone solver has a bend direction. The
        // foot rests at model y ≈ 0.1 (= FootHeight when planted on y = 0
        // ground) and the leg is nearly fully extended.
        //   bone 0 Hips  local (0, 1.1, 0)
        //   bone 1 Knee  local (0, -0.5, 0.02)
        //   bone 2 Foot  local (0, -0.5, -0.02)
        // Plus an arm for the hand tests: Shoulder (child of Hips) → Elbow → Hand.
        //   bone 3 Shoulder local (0.2, -0.1, 0)
        //   bone 4 Elbow    local (0.3, 0, 0.02)
        //   bone 5 Hand     local (0.3, 0, -0.02)
        Ref<Skeleton> BuildRig()
        {
            auto skeleton = Ref<Skeleton>::Create(static_cast<sizet>(6));
            skeleton->m_BoneNames = { "Hips", "Knee", "Foot", "Shoulder", "Elbow", "Hand" };
            skeleton->m_ParentIndices = { -1, 0, 1, 0, 3, 4 };
            const glm::vec3 locals[6] = {
                { 0.0f, 1.1f, 0.0f },
                { 0.0f, -0.5f, 0.02f },
                { 0.0f, -0.5f, -0.02f },
                { 0.2f, -0.1f, 0.0f },
                { 0.3f, 0.0f, 0.02f },
                { 0.3f, 0.0f, -0.02f },
            };
            for (sizet i = 0; i < 6; ++i)
            {
                skeleton->m_LocalTransforms[i] = glm::translate(glm::mat4(1.0f), locals[i]);
            }
            for (sizet i = 0; i < 6; ++i)
            {
                const int p = skeleton->m_ParentIndices[i];
                skeleton->m_GlobalTransforms[i] = (p >= 0)
                                                      ? skeleton->m_GlobalTransforms[static_cast<sizet>(p)] * skeleton->m_LocalTransforms[i]
                                                      : skeleton->m_LocalTransforms[i];
            }
            skeleton->SetBindPose();
            return skeleton;
        }

        constexpr u32 kHips = 0;
        constexpr u32 kFoot = 2;
        constexpr u32 kHand = 5;

        FootIKComponent MakeFootIK()
        {
            FootIKComponent footIK;
            footIK.Enabled = true;
            footIK.LeftFootBone = kFoot;
            // The test rig has one leg: park the right slot on an out-of-range
            // bone so it stays inert (aliasing both slots onto the same bone
            // would double-apply the slope alignment).
            footIK.RightFootBone = 99;
            footIK.ChainLength = 3;
            footIK.FootHeight = 0.1f;
            footIK.PelvisBone = kHips;
            footIK.MaxPelvisDrop = 0.4f;
            footIK.PelvisLerpSpeed = 1000.0f; // effectively instant for tests
            footIK.FootLock = false;          // opt in per test
            footIK.AlignFootToSlope = false;  // opt in per test
            return footIK;
        }

        glm::vec3 FootModelPos(const Ref<Skeleton>& skeleton, u32 bone)
        {
            std::vector<BoneTransform> pose(skeleton->m_LocalTransforms.size());
            for (sizet i = 0; i < pose.size(); ++i)
            {
                pose[i] = BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[i]);
            }
            return BlendUtils::ComputeModelSpaceTransform(bone, pose, skeleton->m_ParentIndices, skeleton->m_BonePreTransforms).Translation;
        }

        void InjectGround(FootIKFootState& foot, f32 groundY, const glm::vec3& normal = { 0.0f, 1.0f, 0.0f })
        {
            foot.HasGround = true;
            foot.GroundPoint = { 0.0f, groundY, 0.0f };
            foot.GroundNormal = normal;
        }
    } // namespace

    TEST(FootIK, DisabledComponentLeavesPoseUntouched)
    {
        auto skeleton = BuildRig();
        const glm::vec3 before = FootModelPos(skeleton, kFoot);

        FootIKComponent footIK = MakeFootIK();
        footIK.Enabled = false;
        FootIKStateComponent state;
        InjectGround(state.Left, -0.3f);
        InjectGround(state.Right, -0.3f);

        ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);
        const glm::vec3 after = FootModelPos(skeleton, kFoot);
        EXPECT_NEAR(before.y, after.y, 1e-5f);
    }

    TEST(FootIK, NoGroundLeavesFootAnimated)
    {
        auto skeleton = BuildRig();
        const glm::vec3 before = FootModelPos(skeleton, kFoot);

        FootIKComponent footIK = MakeFootIK();
        FootIKStateComponent state; // HasGround stays false

        ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);
        const glm::vec3 after = FootModelPos(skeleton, kFoot);
        EXPECT_NEAR(before.y, after.y, 1e-4f);
        EXPECT_NEAR(state.PelvisOffset, 0.0f, 1e-4f);
    }

    TEST(FootIK, PelvisLowersSoFootReachesLowerGround)
    {
        auto skeleton = BuildRig();
        FootIKComponent footIK = MakeFootIK();
        FootIKStateComponent state;
        InjectGround(state.Left, -0.2f);
        InjectGround(state.Right, -0.2f);

        // Several ticks let the (fast) pelvis smoothing converge.
        for (int i = 0; i < 5; ++i)
        {
            ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);
            // Re-pose the skeleton to the bind pose each tick, as the animation
            // sampler would; the pass reapplies its adaptation on top.
            skeleton->m_LocalTransforms = skeleton->m_BindPoseLocalTransforms;
        }
        // Ground at -0.2 → desired foot y = -0.1; animated foot y ≈ 0.1 → the
        // pelvis must give ~0.2 of drop.
        EXPECT_LT(state.PelvisOffset, -0.15f);
        EXPECT_GT(state.PelvisOffset, -footIK.MaxPelvisDrop - 1e-4f);
    }

    TEST(FootIK, PelvisDropClampedAtMax)
    {
        auto skeleton = BuildRig();
        FootIKComponent footIK = MakeFootIK();
        footIK.MaxPelvisDrop = 0.15f;
        FootIKStateComponent state;
        InjectGround(state.Left, -1.0f); // far below anything reachable
        InjectGround(state.Right, -1.0f);

        for (int i = 0; i < 5; ++i)
        {
            ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);
            skeleton->m_LocalTransforms = skeleton->m_BindPoseLocalTransforms;
        }
        EXPECT_GT(state.PelvisOffset, -0.15f - 1e-3f);
        EXPECT_LT(state.PelvisOffset, -0.15f + 1e-3f);
    }

    TEST(FootIK, FootPullsUpOntoRaisedGround)
    {
        auto skeleton = BuildRig();
        FootIKComponent footIK = MakeFootIK();
        footIK.AdjustPelvis = false;
        FootIKStateComponent state;
        InjectGround(state.Left, 0.25f); // a step 0.25 above the origin plane
        InjectGround(state.Right, 0.25f);

        ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);

        // Desired foot y = 0.25 + FootHeight = 0.35; the bent-knee solve should
        // get close (small solver tolerance allowed).
        const glm::vec3 foot = FootModelPos(skeleton, kFoot);
        EXPECT_GT(foot.y, 0.25f);
        EXPECT_NEAR(foot.y, 0.35f, 0.05f);
    }

    TEST(FootIK, SlowGroundedFootPlantsAndPersistsWorldPosition)
    {
        auto skeleton = BuildRig();
        FootIKComponent footIK = MakeFootIK();
        footIK.FootLock = true;
        footIK.AdjustPelvis = false;
        FootIKStateComponent state;
        InjectGround(state.Left, 0.0f);
        InjectGround(state.Right, 0.0f);

        // Tick 1: foot stationary + grounded + low lift → the plant engages.
        glm::mat4 entityWorld(1.0f);
        ApplyFootIKPostPass(*skeleton, footIK, state, entityWorld, 1.0f / 60.0f);
        ASSERT_TRUE(state.Left.Locked);
        const glm::vec3 locked = state.Left.LockedWorldPos;

        // The entity creeps forward a little (below the plant-release speed);
        // the locked foot must stay at its world position, so its model-space
        // position shifts backward relative to the entity.
        skeleton->m_LocalTransforms = skeleton->m_BindPoseLocalTransforms;
        entityWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.001f));
        ApplyFootIKPostPass(*skeleton, footIK, state, entityWorld, 1.0f / 60.0f);
        EXPECT_TRUE(state.Left.Locked);
        EXPECT_NEAR(state.Left.LockedWorldPos.z, locked.z, 1e-4f);

        const glm::vec3 footModel = FootModelPos(skeleton, kFoot);
        const glm::vec3 footWorld = glm::vec3(entityWorld * glm::vec4(footModel, 1.0f));
        EXPECT_NEAR(footWorld.z, locked.z, 0.02f) << "planted foot drifted in world space";
    }

    TEST(FootIK, AnimatedLiftReleasesThePlant)
    {
        auto skeleton = BuildRig();
        FootIKComponent footIK = MakeFootIK();
        footIK.FootLock = true;
        footIK.AdjustPelvis = false;
        FootIKStateComponent state;
        InjectGround(state.Left, 0.0f);
        InjectGround(state.Right, 0.0f);

        ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);
        ASSERT_TRUE(state.Left.Locked);

        // The clip lifts the foot well above the release threshold: raise the
        // hips so the whole leg (and foot) rises.
        skeleton->m_LocalTransforms = skeleton->m_BindPoseLocalTransforms;
        skeleton->m_LocalTransforms[kHips] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.4f, 0.0f));
        ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);
        EXPECT_FALSE(state.Left.Locked);
    }

    TEST(FootIK, SlopeAlignmentTiltsFootTowardGroundNormal)
    {
        auto skeleton = BuildRig();
        FootIKComponent footIK = MakeFootIK();
        footIK.AdjustPelvis = false;
        footIK.AlignFootToSlope = true;
        FootIKStateComponent state;
        const f32 slopeDeg = 20.0f;
        const glm::vec3 normal = glm::normalize(glm::vec3(0.0f, std::cos(glm::radians(slopeDeg)), std::sin(glm::radians(slopeDeg))));
        InjectGround(state.Left, 0.0f, normal);
        InjectGround(state.Right, 0.0f, normal);

        const glm::quat beforeRot = BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[kFoot]).Rotation;
        ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);
        const glm::quat afterRot = BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[kFoot]).Rotation;

        const f32 deltaDeg = glm::degrees(glm::angle(glm::normalize(afterRot * glm::inverse(beforeRot))));
        EXPECT_NEAR(deltaDeg, slopeDeg, 2.0f) << "foot did not tilt to the slope";
    }

    TEST(FootIK, SlopeAlignmentClampedByMaxSlopeAngle)
    {
        auto skeleton = BuildRig();
        FootIKComponent footIK = MakeFootIK();
        footIK.AdjustPelvis = false;
        footIK.AlignFootToSlope = true;
        footIK.MaxSlopeAngle = 10.0f;
        FootIKStateComponent state;
        const glm::vec3 normal = glm::normalize(glm::vec3(0.0f, std::cos(glm::radians(45.0f)), std::sin(glm::radians(45.0f))));
        InjectGround(state.Left, 0.0f, normal);
        InjectGround(state.Right, 0.0f, normal);

        const glm::quat beforeRot = BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[kFoot]).Rotation;
        ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);
        const glm::quat afterRot = BlendUtils::DecomposeMatrix(skeleton->m_LocalTransforms[kFoot]).Rotation;

        const f32 deltaDeg = glm::degrees(glm::angle(glm::normalize(afterRot * glm::inverse(beforeRot))));
        EXPECT_LT(deltaDeg, 10.0f + 1.0f) << "slope alignment exceeded MaxSlopeAngle";
    }

    TEST(FootIK, HandIKReachesResolvedTarget)
    {
        auto skeleton = BuildRig();
        FootIKComponent footIK = MakeFootIK();
        footIK.AdjustPelvis = false;
        footIK.LeftHandEnabled = true;
        footIK.LeftHandBone = kHand;
        footIK.HandChainLength = 3;

        FootIKStateComponent state;
        state.LeftHandActive = true;
        // Bind hand sits at model (0.8, 1.0, 0); pull it to a reachable target.
        state.LeftHandResolvedTarget = glm::vec3(0.6f, 1.2f, 0.1f);

        ApplyFootIKPostPass(*skeleton, footIK, state, glm::mat4(1.0f), 1.0f / 60.0f);

        const glm::vec3 hand = FootModelPos(skeleton, kHand);
        EXPECT_NEAR(glm::length(hand - state.LeftHandResolvedTarget), 0.0f, 0.06f)
            << "hand did not reach the resolved prop/ledge target";
    }
} // namespace OloEngine::Animation
