#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/OneShotBlend.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Core/Ref.h"

using namespace OloEngine;
using namespace OloEngine::Animation;

//==============================================================================
// Helpers
//==============================================================================

namespace
{
    // Creates a simple animation clip with a single bone moving along X
    Ref<AnimationClip> MakeTestClip(f32 duration, const std::string& boneName = "bone0")
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "TestClip";
        clip->Duration = duration;

        BoneAnimation boneAnim;
        boneAnim.BoneName = boneName;
        boneAnim.PositionKeys.push_back({ 0.0f, glm::vec3(0.0f, 0.0f, 0.0f) });
        boneAnim.PositionKeys.push_back({ duration, glm::vec3(10.0f, 0.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ 0.0f, glm::quat(1, 0, 0, 0) });
        boneAnim.RotationKeys.push_back({ duration, glm::quat(1, 0, 0, 0) });
        boneAnim.ScaleKeys.push_back({ 0.0f, glm::vec3(1.0f) });
        boneAnim.ScaleKeys.push_back({ duration, glm::vec3(1.0f) });

        clip->BoneAnimations.push_back(std::move(boneAnim));
        clip->InitializeBoneCache();
        return clip;
    }

    // Sets up a 3-bone chain with identity poses
    struct TestSetup
    {
        std::vector<BoneTransform> basePose;
        std::vector<int> parentIndices;
        std::vector<std::string> boneNames;

        TestSetup()
        {
            basePose.resize(3);
            for (auto& bt : basePose)
            {
                bt.Translation = glm::vec3(0.0f);
                bt.Rotation = glm::quat(1, 0, 0, 0);
                bt.Scale = glm::vec3(1.0f);
            }
            parentIndices = { -1, 0, 1 };
            boneNames = { "bone0", "bone1", "bone2" };
        }
    };
} // namespace

//==============================================================================
// Phase Lifecycle Tests
//==============================================================================

TEST(OneShotBlendTest, StartsIdle)
{
    OneShotBlend oneShot;
    EXPECT_FALSE(oneShot.IsActive());
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Idle);
}

TEST(OneShotBlendTest, TriggerWithoutClipStaysIdle)
{
    OneShotBlend oneShot;
    oneShot.Trigger();
    EXPECT_FALSE(oneShot.IsActive());
}

TEST(OneShotBlendTest, TriggerWithClipStartsBlendIn)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(1.0f);
    oneShot.BlendInDuration = 0.2f;
    oneShot.Trigger();
    EXPECT_TRUE(oneShot.IsActive());
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::BlendIn);
}

TEST(OneShotBlendTest, CompleteLifecycle_BlendIn_Playing_BlendOut_Idle)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(1.0f);
    oneShot.BlendInDuration = 0.2f;
    oneShot.BlendOutDuration = 0.2f;

    TestSetup setup;

    oneShot.Trigger();

    // Advance through blend-in (0.2s)
    oneShot.Update(0.25f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Playing);

    // Advance to near the end (past blend-out start at 0.8s)
    oneShot.Update(0.60f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::BlendOut);

    // Advance past the end
    oneShot.Update(0.30f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Idle);
    EXPECT_FALSE(oneShot.IsActive());
}

TEST(OneShotBlendTest, ZeroBlendDurationsSkipBlendPhases)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(0.5f);
    oneShot.BlendInDuration = 0.0f;
    oneShot.BlendOutDuration = 0.0f;

    TestSetup setup;

    oneShot.Trigger();

    // First update with zero blend-in should go to Playing immediately
    oneShot.Update(0.01f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Playing);

    // Advance past duration — should go to BlendOut then immediately Idle
    oneShot.Update(0.6f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Idle);
}

TEST(OneShotBlendTest, CancelJumpsToBlendOut)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(2.0f);
    oneShot.BlendInDuration = 0.1f;
    oneShot.BlendOutDuration = 0.3f;

    TestSetup setup;

    oneShot.Trigger();
    oneShot.Update(0.15f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Playing);

    oneShot.Cancel();
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::BlendOut);

    // Advance through blend-out
    oneShot.Update(0.35f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Idle);
}

TEST(OneShotBlendTest, CancelWhileIdleDoesNothing)
{
    OneShotBlend oneShot;
    oneShot.Cancel();
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Idle);
}

//==============================================================================
// Callback Tests
//==============================================================================

TEST(OneShotBlendTest, OnFinishedCallbackFires)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(0.5f);
    oneShot.BlendInDuration = 0.0f;
    oneShot.BlendOutDuration = 0.0f;

    TestSetup setup;
    bool finished = false;
    oneShot.OnFinished = [&finished]()
    { finished = true; };

    oneShot.Trigger();
    oneShot.Update(0.6f, setup.basePose, setup.parentIndices, setup.boneNames);

    EXPECT_TRUE(finished);
    EXPECT_FALSE(oneShot.IsActive());
}

TEST(OneShotBlendTest, OnFinishedCallbackNotCalledDuringPlayback)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(1.0f);
    oneShot.BlendInDuration = 0.0f;
    oneShot.BlendOutDuration = 0.0f;

    TestSetup setup;
    bool finished = false;
    oneShot.OnFinished = [&finished]()
    { finished = true; };

    oneShot.Trigger();
    oneShot.Update(0.5f, setup.basePose, setup.parentIndices, setup.boneNames);

    EXPECT_FALSE(finished);
    EXPECT_TRUE(oneShot.IsActive());
}

//==============================================================================
// Blend Weight Tests
//==============================================================================

TEST(OneShotBlendTest, BlendInInterpolatesWeight)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(2.0f);
    oneShot.BlendInDuration = 1.0f;
    oneShot.BlendOutDuration = 0.0f;
    oneShot.Weight = 1.0f;

    TestSetup setup;

    oneShot.Trigger();

    // At t=0.5, blend-in is halfway → effective weight ~0.5
    // The bone0 animation moves from (0,0,0) to (10,0,0) over 2s
    // At playback t=0.5: bone0 translation is (2.5, 0, 0)
    // With weight 0.5: lerp(0, 2.5, 0.5) = (1.25, 0, 0)
    oneShot.Update(0.5f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::BlendIn);

    // Bone0 should have been modified from (0,0,0) toward the clip value
    EXPECT_GT(setup.basePose[0].Translation.x, 0.0f);
}

TEST(OneShotBlendTest, ZeroWeightDoesNotModifyPose)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(1.0f);
    oneShot.BlendInDuration = 0.0f;
    oneShot.BlendOutDuration = 0.0f;
    oneShot.Weight = 0.0f;

    TestSetup setup;
    auto originalPose = setup.basePose;

    oneShot.Trigger();
    oneShot.Update(0.5f, setup.basePose, setup.parentIndices, setup.boneNames);

    // Pose should be unchanged when weight is 0
    for (sizet i = 0; i < setup.basePose.size(); ++i)
    {
        EXPECT_FLOAT_EQ(setup.basePose[i].Translation.x, originalPose[i].Translation.x);
        EXPECT_FLOAT_EQ(setup.basePose[i].Translation.y, originalPose[i].Translation.y);
        EXPECT_FLOAT_EQ(setup.basePose[i].Translation.z, originalPose[i].Translation.z);
    }
}

//==============================================================================
// Pose Modification Tests
//==============================================================================

TEST(OneShotBlendTest, FullWeightAppliesClipPose)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(1.0f);
    oneShot.BlendInDuration = 0.0f;
    oneShot.BlendOutDuration = 0.0f;
    oneShot.Weight = 1.0f;

    TestSetup setup;

    oneShot.Trigger();
    // Advance to midpoint: playback at 0.5s, clip interpolates bone0 from (0,0,0) to (10,0,0)
    oneShot.Update(0.5f, setup.basePose, setup.parentIndices, setup.boneNames);

    // bone0 should be at approximately (5, 0, 0) with full weight
    EXPECT_NEAR(setup.basePose[0].Translation.x, 5.0f, 0.1f);
    EXPECT_NEAR(setup.basePose[0].Translation.y, 0.0f, 0.01f);
    EXPECT_NEAR(setup.basePose[0].Translation.z, 0.0f, 0.01f);

    // bone1 and bone2 should be unchanged (not animated by clip)
    EXPECT_NEAR(setup.basePose[1].Translation.x, 0.0f, 0.01f);
    EXPECT_NEAR(setup.basePose[2].Translation.x, 0.0f, 0.01f);
}

TEST(OneShotBlendTest, ReTriggerDuringPlaybackRestarts)
{
    OneShotBlend oneShot;
    oneShot.Clip = MakeTestClip(1.0f);
    oneShot.BlendInDuration = 0.1f;
    oneShot.BlendOutDuration = 0.1f;

    TestSetup setup;

    oneShot.Trigger();
    oneShot.Update(0.5f, setup.basePose, setup.parentIndices, setup.boneNames);
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::Playing);

    // Re-trigger mid-playback should restart
    oneShot.Trigger();
    EXPECT_EQ(oneShot.GetPhase(), OneShotBlend::Phase::BlendIn);
}

TEST(OneShotBlendTest, IdleUpdateDoesNotModifyPose)
{
    OneShotBlend oneShot;
    TestSetup setup;
    auto originalPose = setup.basePose;

    // Update while idle should do nothing
    oneShot.Update(0.5f, setup.basePose, setup.parentIndices, setup.boneNames);

    for (sizet i = 0; i < setup.basePose.size(); ++i)
    {
        EXPECT_FLOAT_EQ(setup.basePose[i].Translation.x, originalPose[i].Translation.x);
    }
}
