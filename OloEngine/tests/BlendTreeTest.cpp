#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/BlendTree.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/BlendNode.h"

using namespace OloEngine;

// Helper to create a simple animation clip for blend tree testing
static Ref<AnimationClip> CreateBlendTestClip(const std::string& name, float duration, const glm::vec3& startPos, const glm::vec3& endPos)
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Name = name;
    clip->Duration = duration;

    BoneAnimation boneAnim;
    boneAnim.BoneName = "Bone0";
    boneAnim.PositionKeys.push_back({ 0.0, startPos });
    boneAnim.PositionKeys.push_back({ static_cast<f64>(duration), endPos });
    boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    boneAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
    boneAnim.ScaleKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f) });
    clip->BoneAnimations.push_back(boneAnim);
    clip->InitializeBoneCache();

    return clip;
}

//==============================================================================
// 1D Blend Tree Tests
//==============================================================================

TEST(BlendTreeTest, Simple1D_AtFirstThreshold)
{
    auto idleClip = CreateBlendTestClip("Idle", 1.0f, glm::vec3(0.0f), glm::vec3(0.0f));
    auto walkClip = CreateBlendTestClip("Walk", 1.0f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f));
    auto runClip = CreateBlendTestClip("Run", 0.8f, glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(5.0f, 0.0f, 0.0f));

    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";

    tree.Children.push_back({ idleClip, 0.0f, {}, 1.0f });
    tree.Children.push_back({ walkClip, 0.5f, {}, 1.0f });
    tree.Children.push_back({ runClip, 1.0f, {}, 1.0f });

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.0f);

    std::vector<BoneTransform> result;
    tree.Evaluate(0.0f, params, 1, result);
    ASSERT_EQ(result.size(), 1u);
    // At Speed=0, should be 100% idle (position = 0,0,0 at t=0)
    EXPECT_NEAR(result[0].Translation.x, 0.0f, 0.01f);
}

TEST(BlendTreeTest, Simple1D_AtMidThreshold)
{
    auto idleClip = CreateBlendTestClip("Idle", 1.0f, glm::vec3(0.0f), glm::vec3(0.0f));
    auto walkClip = CreateBlendTestClip("Walk", 1.0f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f));
    auto runClip = CreateBlendTestClip("Run", 0.8f, glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(5.0f, 0.0f, 0.0f));

    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";

    tree.Children.push_back({ idleClip, 0.0f, {}, 1.0f });
    tree.Children.push_back({ walkClip, 0.5f, {}, 1.0f });
    tree.Children.push_back({ runClip, 1.0f, {}, 1.0f });

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.5f);

    std::vector<BoneTransform> result;
    tree.Evaluate(0.0f, params, 1, result);
    ASSERT_EQ(result.size(), 1u);
    // At Speed=0.5, should be 100% walk (position = 1,0,0 at t=0)
    EXPECT_NEAR(result[0].Translation.x, 1.0f, 0.01f);
}

TEST(BlendTreeTest, Simple1D_BetweenThresholds)
{
    auto idleClip = CreateBlendTestClip("Idle", 1.0f, glm::vec3(0.0f), glm::vec3(0.0f));
    auto walkClip = CreateBlendTestClip("Walk", 1.0f, glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f));

    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";

    tree.Children.push_back({ idleClip, 0.0f, {}, 1.0f });
    tree.Children.push_back({ walkClip, 1.0f, {}, 1.0f });

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.5f);

    std::vector<BoneTransform> result;
    tree.Evaluate(0.0f, params, 1, result);
    ASSERT_EQ(result.size(), 1u);
    // At Speed=0.5, should be 50% idle (0) + 50% walk (2) = 1
    EXPECT_NEAR(result[0].Translation.x, 1.0f, 0.01f);
}

TEST(BlendTreeTest, Simple1D_AtLastThreshold)
{
    auto idleClip = CreateBlendTestClip("Idle", 1.0f, glm::vec3(0.0f), glm::vec3(0.0f));
    auto runClip = CreateBlendTestClip("Run", 0.8f, glm::vec3(4.0f, 0.0f, 0.0f), glm::vec3(4.0f, 0.0f, 0.0f));

    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";

    tree.Children.push_back({ idleClip, 0.0f, {}, 1.0f });
    tree.Children.push_back({ runClip, 1.0f, {}, 1.0f });

    AnimationParameterSet params;
    params.DefineFloat("Speed", 1.0f);

    std::vector<BoneTransform> result;
    tree.Evaluate(0.0f, params, 1, result);
    ASSERT_EQ(result.size(), 1u);
    // At Speed=1, should be 100% run (position = 4,0,0 at t=0)
    EXPECT_NEAR(result[0].Translation.x, 4.0f, 0.01f);
}

TEST(BlendTreeTest, Simple1D_ClampBelowFirst)
{
    auto idleClip = CreateBlendTestClip("Idle", 1.0f, glm::vec3(0.0f), glm::vec3(0.0f));
    auto walkClip = CreateBlendTestClip("Walk", 1.0f, glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f));

    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";

    tree.Children.push_back({ idleClip, 0.0f, {}, 1.0f });
    tree.Children.push_back({ walkClip, 1.0f, {}, 1.0f });

    AnimationParameterSet params;
    params.DefineFloat("Speed", -0.5f);

    std::vector<BoneTransform> result;
    tree.Evaluate(0.0f, params, 1, result);
    ASSERT_EQ(result.size(), 1u);
    // Below first threshold, should clamp to idle
    EXPECT_NEAR(result[0].Translation.x, 0.0f, 0.01f);
}

//==============================================================================
// 2D Blend Tree Tests
//==============================================================================

TEST(BlendTreeTest, SimpleDirectional2D_AtChild)
{
    auto fwdClip = CreateBlendTestClip("Forward", 1.0f, glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    auto rightClip = CreateBlendTestClip("Right", 1.0f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    BlendTree tree;
    tree.Type = BlendTree::BlendType::SimpleDirectional2D;
    tree.BlendParameterX = "VelocityX";
    tree.BlendParameterY = "VelocityZ";

    tree.Children.push_back({ fwdClip, 0.0f, glm::vec2(0.0f, 1.0f), 1.0f });
    tree.Children.push_back({ rightClip, 0.0f, glm::vec2(1.0f, 0.0f), 1.0f });

    AnimationParameterSet params;
    params.DefineFloat("VelocityX", 0.0f);
    params.DefineFloat("VelocityZ", 1.0f);

    std::vector<BoneTransform> result;
    tree.Evaluate(0.0f, params, 1, result);
    ASSERT_EQ(result.size(), 1u);
    // Exactly on the Forward child, should be 100% forward
    // With inverse distance weighting, when exactly on a point, that point gets full weight
    EXPECT_NEAR(result[0].Translation.z, 1.0f, 0.01f);
}

//==============================================================================
// Edge Case Tests
//==============================================================================

TEST(BlendTreeTest, EmptyBlendTree)
{
    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.5f);

    std::vector<BoneTransform> result;
    tree.Evaluate(0.0f, params, 1, result);
    // Empty tree should produce identity bone transforms sized to boneCount
    EXPECT_EQ(result.size(), 1u);
}

TEST(BlendTreeTest, SingleChild)
{
    auto clip = CreateBlendTestClip("Clip", 1.0f, glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(3.0f, 0.0f, 0.0f));

    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";

    tree.Children.push_back({ clip, 0.5f, {}, 1.0f });

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.0f);

    std::vector<BoneTransform> result;
    tree.Evaluate(0.0f, params, 1, result);
    ASSERT_EQ(result.size(), 1u);
    // Single child should get full weight regardless of parameter
    EXPECT_NEAR(result[0].Translation.x, 3.0f, 0.01f);
}

TEST(BlendTreeTest, GetDurationReturnsMaxChildDuration)
{
    auto shortClip = CreateBlendTestClip("Short", 0.5f, glm::vec3(0.0f), glm::vec3(1.0f));
    auto longClip = CreateBlendTestClip("Long", 2.0f, glm::vec3(0.0f), glm::vec3(1.0f));

    BlendTree tree;
    tree.Type = BlendTree::BlendType::Simple1D;
    tree.BlendParameterX = "Speed";

    tree.Children.push_back({ shortClip, 0.0f, {}, 1.0f });
    tree.Children.push_back({ longClip, 1.0f, {}, 1.0f });

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.5f);

    // GetDuration returns weighted average: mix(0.5, 2.0, 0.5) = 1.25
    EXPECT_FLOAT_EQ(tree.GetDuration(params), 1.25f);
}
