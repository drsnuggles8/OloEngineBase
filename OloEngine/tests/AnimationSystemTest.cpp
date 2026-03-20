#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

using namespace OloEngine;
using namespace OloEngine::Animation;

// Creates a skeleton with two bones:
//   Bone 0 ("Root"): root bone with a custom local transform (no parent)
//   Bone 1 ("Child"): child of Root with identity local transform
// The root bone simulates the fox.gltf pattern where b_Root_00 carries
// a -90° X rotation that must be preserved during animation.
static Ref<Skeleton> CreateTwoBoneSkeleton(const glm::mat4& rootLocalTransform)
{
    auto skeleton = Ref<Skeleton>::Create(2);
    skeleton->m_BoneNames = { "Root", "Child" };
    skeleton->m_ParentIndices = { -1, 0 };
    skeleton->m_LocalTransforms = { rootLocalTransform, glm::mat4(1.0f) };
    skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };

    // Compute initial global transforms
    skeleton->m_GlobalTransforms[0] = skeleton->m_BonePreTransforms[0] * skeleton->m_LocalTransforms[0];
    skeleton->m_GlobalTransforms[1] = skeleton->m_GlobalTransforms[0] * skeleton->m_BonePreTransforms[1] * skeleton->m_LocalTransforms[1];

    // Capture bind pose (this is what ProcessSkeleton does after setup)
    skeleton->SetBindPose();

    return skeleton;
}

// Creates a clip that only animates "Child" but NOT "Root".
// This simulates the fox.gltf pattern where the root bone has no keyframes.
static Ref<AnimationClip> CreateChildOnlyClip(f32 duration)
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Name = "ChildOnly";
    clip->Duration = duration;

    BoneAnimation boneAnim;
    boneAnim.BoneName = "Child";
    boneAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f, 1.0f, 0.0f) });
    boneAnim.PositionKeys.push_back({ static_cast<f64>(duration), glm::vec3(0.0f, 2.0f, 0.0f) });
    boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    boneAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
    boneAnim.ScaleKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f) });
    clip->BoneAnimations.push_back(boneAnim);
    clip->InitializeBoneCache();

    return clip;
}

// Creates a clip that animates both "Root" and "Child".
static Ref<AnimationClip> CreateBothBonesClip(f32 duration)
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Name = "BothBones";
    clip->Duration = duration;

    // Root bone animation
    BoneAnimation rootAnim;
    rootAnim.BoneName = "Root";
    rootAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
    rootAnim.PositionKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f, 0.0f, 0.0f) });
    rootAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    rootAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    rootAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
    rootAnim.ScaleKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f) });
    clip->BoneAnimations.push_back(rootAnim);

    // Child bone animation
    BoneAnimation childAnim;
    childAnim.BoneName = "Child";
    childAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f, 1.0f, 0.0f) });
    childAnim.PositionKeys.push_back({ static_cast<f64>(duration), glm::vec3(0.0f, 2.0f, 0.0f) });
    childAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    childAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    childAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
    childAnim.ScaleKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f) });
    clip->BoneAnimations.push_back(childAnim);
    clip->InitializeBoneCache();

    return clip;
}

// -----------------------------------------------------------------
// Regression test for the "fox model flip" bug.
//
// In fox.gltf the root bone (b_Root_00) carries a -90° X rotation but
// has NO animation keyframes.  When animation starts, bones not animated
// in the current clip must keep their bind-pose local transform.
//
// The bug: m_BindPoseLocalTransforms was initialised to identity in the
// SkeletonData constructor rather than being populated by SetBindPose().
// AnimationSystem::Update() then reset ALL local transforms to identity,
// erasing the root bone's rotation and flipping the model.
// -----------------------------------------------------------------

TEST(AnimationSystem, NonAnimatedBonePreservesBindPoseTransform)
{
    // Root bone has a -90° X rotation (like fox.gltf's b_Root_00)
    const glm::mat4 rootRotation = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    auto skeleton = CreateTwoBoneSkeleton(rootRotation);

    // Clip only animates "Child", NOT "Root"
    auto clip = CreateChildOnlyClip(1.0f);

    AnimationStateComponent animState;
    animState.m_CurrentClip = clip;
    animState.m_CurrentTime = 0.0f;
    animState.m_IsPlaying = true;

    // Run one animation update
    AnimationSystem::Update(animState, *skeleton, 0.016f);

    // Root bone (index 0) must retain its bind-pose local transform (-90° X rotation)
    const glm::mat4& rootLocal = skeleton->m_LocalTransforms[0];
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(rootLocal[col][row], rootRotation[col][row], 1e-5f)
                << "Root bone local transform mismatch at [" << col << "][" << row << "]. "
                << "Non-animated bones must preserve their bind-pose transform.";
        }
    }
}

// Verify that SetBindPose() must be called before the bind-pose reset takes effect.
// If m_BindPoseLocalTransforms is empty (SetBindPose never called), the reset is skipped
// and existing local transforms are left untouched.
TEST(AnimationSystem, BindPoseResetSkippedWhenNotInitialized)
{
    const glm::mat4 rootRotation = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    auto skeleton = Ref<Skeleton>::Create(2);
    skeleton->m_BoneNames = { "Root", "Child" };
    skeleton->m_ParentIndices = { -1, 0 };
    skeleton->m_LocalTransforms = { rootRotation, glm::mat4(1.0f) };
    skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };
    // Intentionally do NOT call SetBindPose() — m_BindPoseLocalTransforms should be empty

    ASSERT_TRUE(skeleton->m_BindPoseLocalTransforms.empty());

    auto clip = CreateChildOnlyClip(1.0f);

    AnimationStateComponent animState;
    animState.m_CurrentClip = clip;
    animState.m_CurrentTime = 0.0f;

    AnimationSystem::Update(animState, *skeleton, 0.016f);

    // Root local transform should still be the rotation (not overwritten with identity)
    const glm::mat4& rootLocal = skeleton->m_LocalTransforms[0];
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(rootLocal[col][row], rootRotation[col][row], 1e-5f)
                << "Root transform was incorrectly overwritten despite m_BindPoseLocalTransforms being empty.";
        }
    }
}

// Verify blending between two clips preserves non-animated bone transforms.
TEST(AnimationSystem, BlendingPreservesNonAnimatedBoneTransform)
{
    const glm::mat4 rootRotation = glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    auto skeleton = CreateTwoBoneSkeleton(rootRotation);

    // Both clips animate only "Child" — "Root" is NOT animated in either
    auto clipA = CreateChildOnlyClip(1.0f);
    auto clipB = CreateChildOnlyClip(2.0f);

    AnimationStateComponent animState;
    animState.m_CurrentClip = clipA;
    animState.m_CurrentTime = 0.5f;
    animState.m_Blending = true;
    animState.m_NextClip = clipB;
    animState.m_NextTime = 0.0f;
    animState.m_BlendFactor = 0.5f;
    animState.m_BlendDuration = 0.3f;
    animState.m_BlendTime = 0.15f;

    AnimationSystem::Update(animState, *skeleton, 0.016f);

    // Root bone must still have its bind-pose rotation
    const glm::mat4& rootLocal = skeleton->m_LocalTransforms[0];
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(rootLocal[col][row], rootRotation[col][row], 1e-5f)
                << "Root bone transform corrupted during blend at [" << col << "][" << row << "].";
        }
    }
}

// Verify that bones WITH animation keyframes are properly updated (not stuck at bind pose).
TEST(AnimationSystem, AnimatedBoneIsUpdatedFromKeyframes)
{
    auto skeleton = CreateTwoBoneSkeleton(glm::mat4(1.0f));

    auto clip = CreateBothBonesClip(1.0f);

    AnimationStateComponent animState;
    animState.m_CurrentClip = clip;
    animState.m_CurrentTime = 0.0f;
    animState.m_IsPlaying = true;

    // Advance past t=0 so we get interpolated values
    AnimationSystem::Update(animState, *skeleton, 0.5f);

    // Child bone (index 1) should have been updated by the animation
    // At t=0.5, position interpolates from (0,1,0) to (0,2,0) → (0,1.5,0)
    // The local transform should reflect this translation
    const glm::mat4& childLocal = skeleton->m_LocalTransforms[1];
    const glm::vec3 childTranslation = glm::vec3(childLocal[3]);
    EXPECT_NEAR(childTranslation.y, 1.5f, 0.1f)
        << "Animated child bone should have interpolated position.";
}
