// OLO_TEST_LAYER: unit
//
// Regression tests for issue #543: the animation-graph pose pipeline
// (AnimationGraphComponent -> state machine -> state -> blend tree) used to map
// a clip's channels to skeleton bones BY ARRAY INDEX (out[i] = clip.BoneAnimations[i]).
// That invariant does not hold for imported rigs — a clip's channel order is
// exporter-dependent and covers only the animated subset of nodes, while skeleton
// bone indices come from a depth-first traversal — so every channel landed on the
// wrong bone (head rotation on a leg) and un-keyed bones snapped to identity.
//
// The fix maps by bone NAME (via PoseEvalContext) and falls back to the bind-pose
// local transform for bones a clip does not animate. The fixtures below build a
// clip whose channel order deliberately DIFFERS from the skeleton bone order — the
// case the old single-"Bone0" fixtures (BlendTreeTest / AnimationStateMachineTest)
// could never catch because there index trivially equalled name.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/BlendTree.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendNode.h"
#include "OloEngine/Core/Ref.h"
#include "Animation/AnimationTestHelpers.h"

#include <string>
#include <vector>

using namespace OloEngine;
using OloEngine::AnimTest::MakeConstantChannel;

namespace
{
    // Skeleton bone order (depth-first): Hips(0), Spine(1), Head(2).
    const std::vector<std::string> kBoneNames = { "Hips", "Spine", "Head" };

    // Distinctive bind-pose translations so an un-keyed bone (Spine) is
    // distinguishable from both identity and any animated channel.
    std::vector<BoneTransform> MakeBindPose()
    {
        std::vector<BoneTransform> bind(3);
        bind[0].Translation = glm::vec3(0.1f, 0.0f, 0.0f); // Hips
        bind[1].Translation = glm::vec3(0.2f, 0.0f, 0.0f); // Spine
        bind[2].Translation = glm::vec3(0.3f, 0.0f, 0.0f); // Head
        return bind;
    }

    // A clip whose channels are stored in a DIFFERENT order than the skeleton
    // (Head first, then Hips) and which omits Spine entirely. Under the old
    // index-based mapping this put Head's anim on Hips, Hips' anim on Spine, and
    // left Head at identity.
    Ref<AnimationClip> MakeScrambledOrderClip(const std::string& name = "Clip")
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = name;
        clip->Duration = 1.0f;
        clip->BoneAnimations.push_back(MakeConstantChannel("Head", glm::vec3(30.0f, 0.0f, 0.0f)));
        clip->BoneAnimations.push_back(MakeConstantChannel("Hips", glm::vec3(10.0f, 0.0f, 0.0f)));
        clip->InitializeBoneCache();
        return clip;
    }

    // The expectation shared by every path: each channel lands on its NAMED bone,
    // and the un-keyed Spine keeps its bind pose (not identity).
    void ExpectByNameMapping(const std::vector<BoneTransform>& pose)
    {
        ASSERT_EQ(pose.size(), 3u);
        EXPECT_NEAR(pose[0].Translation.x, 10.0f, 1e-4f) << "Hips must sample the Hips channel";
        EXPECT_NEAR(pose[1].Translation.x, 0.2f, 1e-4f) << "Un-keyed Spine must fall back to bind pose";
        EXPECT_NEAR(pose[2].Translation.x, 30.0f, 1e-4f) << "Head must sample the Head channel";
    }
} // namespace

// The lowest level: the shared static sampler. This is the exact site the bug
// lived in (was clip.BoneAnimations[i] -> bone i).
TEST(AnimationGraphBoneMappingTest, SampleClipBoneTransforms_MapsByName_NotIndex)
{
    auto clip = MakeScrambledOrderClip();
    auto bind = MakeBindPose();
    const PoseEvalContext ctx{ kBoneNames, bind };

    std::vector<BoneTransform> pose;
    BlendTree::SampleClipBoneTransforms(clip, 0.0f, kBoneNames.size(), ctx, pose);

    ExpectByNameMapping(pose);
}

// With no bind pose supplied, un-keyed bones degrade to identity (not garbage).
TEST(AnimationGraphBoneMappingTest, SampleClipBoneTransforms_NoBindPose_UnkeyedBoneIsIdentity)
{
    auto clip = MakeScrambledOrderClip();
    const PoseEvalContext ctx{ kBoneNames, {} };

    std::vector<BoneTransform> pose;
    BlendTree::SampleClipBoneTransforms(clip, 0.0f, kBoneNames.size(), ctx, pose);

    ASSERT_EQ(pose.size(), 3u);
    EXPECT_NEAR(pose[0].Translation.x, 10.0f, 1e-4f); // Hips channel
    EXPECT_NEAR(pose[1].Translation.x, 0.0f, 1e-4f);  // Spine -> identity fallback
    EXPECT_NEAR(pose[2].Translation.x, 30.0f, 1e-4f); // Head channel
}

// The AnimationState.cpp path (MotionType::SingleClip) — previously its own
// index-based SampleClipIntoPose helper.
TEST(AnimationGraphBoneMappingTest, AnimationState_SingleClip_MapsByName)
{
    AnimationState state;
    state.Name = "Clip";
    state.Type = AnimationState::MotionType::SingleClip;
    state.Clip = MakeScrambledOrderClip();

    auto bind = MakeBindPose();
    const PoseEvalContext ctx{ kBoneNames, bind };

    AnimationParameterSet params;
    std::vector<BoneTransform> pose;
    state.Evaluate(0.0f, params, kBoneNames.size(), ctx, pose);

    ExpectByNameMapping(pose);
}

// A SingleClip state with no clip must rest at bind pose, not collapse to identity.
TEST(AnimationGraphBoneMappingTest, AnimationState_NullClip_FallsBackToBindPose)
{
    AnimationState state;
    state.Type = AnimationState::MotionType::SingleClip;
    // Clip intentionally left null.

    auto bind = MakeBindPose();
    const PoseEvalContext ctx{ kBoneNames, bind };

    AnimationParameterSet params;
    std::vector<BoneTransform> pose;
    state.Evaluate(0.0f, params, kBoneNames.size(), ctx, pose);

    ASSERT_EQ(pose.size(), 3u);
    EXPECT_NEAR(pose[0].Translation.x, 0.1f, 1e-4f);
    EXPECT_NEAR(pose[1].Translation.x, 0.2f, 1e-4f);
    EXPECT_NEAR(pose[2].Translation.x, 0.3f, 1e-4f);
}

// The BlendTree.cpp path — a 1D tree with a single child routes through
// Evaluate1D -> SampleClipBoneTransforms.
TEST(AnimationGraphBoneMappingTest, BlendTree_SingleChild_MapsByName)
{
    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";
    tree.Children.push_back({ MakeScrambledOrderClip(), 0.0f, {}, 1.0f });

    auto bind = MakeBindPose();
    const PoseEvalContext ctx{ kBoneNames, bind };

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.0f);

    std::vector<BoneTransform> pose;
    tree.Evaluate(0.0f, params, kBoneNames.size(), ctx, pose);

    ExpectByNameMapping(pose);
}

// A 1D blend between two clips: both children carry the SAME per-name channels,
// so the by-name-blended result equals the per-bone values (and the un-keyed
// Spine stays at bind pose through the blend). A blend of two scrambled-order
// clips would still scramble under the old index mapping.
TEST(AnimationGraphBoneMappingTest, BlendTree_BlendedChildren_MapByName)
{
    auto clipA = MakeScrambledOrderClip("A");
    // clipB stores channels in yet another order (Hips first) to prove neither
    // child's array order leaks into the mapping.
    auto clipB = Ref<AnimationClip>::Create();
    clipB->Name = "B";
    clipB->Duration = 1.0f;
    clipB->BoneAnimations.push_back(MakeConstantChannel("Hips", glm::vec3(10.0f, 0.0f, 0.0f)));
    clipB->BoneAnimations.push_back(MakeConstantChannel("Head", glm::vec3(30.0f, 0.0f, 0.0f)));
    clipB->InitializeBoneCache();

    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";
    tree.Children.push_back({ clipA, 0.0f, {}, 1.0f });
    tree.Children.push_back({ clipB, 1.0f, {}, 1.0f });

    auto bind = MakeBindPose();
    const PoseEvalContext ctx{ kBoneNames, bind };

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.5f); // 50/50 blend of two identical-by-name poses

    std::vector<BoneTransform> pose;
    tree.Evaluate(0.0f, params, kBoneNames.size(), ctx, pose);

    ExpectByNameMapping(pose);
}

// End-to-end through the state machine (the object AnimationGraph drives per
// layer): the ctx must be threaded all the way from Update down to the sampler.
TEST(AnimationGraphBoneMappingTest, StateMachine_ThreadsContext_MapsByName)
{
    AnimationStateMachine sm;

    AnimationState state;
    state.Name = "Clip";
    state.Type = AnimationState::MotionType::SingleClip;
    state.Clip = MakeScrambledOrderClip();
    sm.AddState(state);
    sm.SetDefaultState("Clip");

    auto bind = MakeBindPose();
    const PoseEvalContext ctx{ kBoneNames, bind };

    AnimationParameterSet params;
    sm.Start(params);

    std::vector<BoneTransform> pose;
    sm.Update(0.0f, params, kBoneNames.size(), ctx, pose);

    ExpectByNameMapping(pose);
}
