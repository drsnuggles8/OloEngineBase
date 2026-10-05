#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <limits>

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

// -----------------------------------------------------------------
// Clip switching and one-shot clips (issue #1533). A script names a clip in
// m_RequestedClip; the next Update blends to it from its first frame, and a
// clip whose loop flag is off holds its final pose instead of wrapping.
// -----------------------------------------------------------------

static Ref<AnimationClip> NamedClip(const char* name, f32 duration)
{
    auto clip = CreateBothBonesClip(duration);
    clip->Name = name;
    return clip;
}

TEST(AnimationSystem, AOneShotClipHoldsItsFinalPose)
{
    auto skeleton = CreateTwoBoneSkeleton(glm::mat4(1.0f));

    AnimationStateComponent animState;
    animState.m_CurrentClip = CreateBothBonesClip(1.0f);
    animState.m_IsPlaying = true;
    animState.m_Loop = false;

    // 1.2 s into a 1 s clip: held at the end, where the child sits at y = 2.
    AnimationSystem::Update(animState, *skeleton, 0.6f);
    AnimationSystem::Update(animState, *skeleton, 0.6f);
    EXPECT_FLOAT_EQ(animState.m_CurrentTime, 1.0f);
    EXPECT_NEAR(glm::vec3(skeleton->m_LocalTransforms[1][3]).y, 2.0f, 1e-4f);

    // The control: the same clip looping has wrapped to 0.2 s, y = 1.2.
    AnimationStateComponent looping;
    looping.m_CurrentClip = animState.m_CurrentClip;
    looping.m_IsPlaying = true;
    AnimationSystem::Update(looping, *skeleton, 0.6f);
    AnimationSystem::Update(looping, *skeleton, 0.6f);
    EXPECT_NEAR(looping.m_CurrentTime, 0.2f, 1e-4f);
    EXPECT_NEAR(glm::vec3(skeleton->m_LocalTransforms[1][3]).y, 1.2f, 1e-3f);
}

TEST(AnimationSystem, AClipRequestBlendsToTheNamedClipFromItsFirstFrame)
{
    auto skeleton = CreateTwoBoneSkeleton(glm::mat4(1.0f));
    auto idle = NamedClip("Idle", 2.0f);
    auto sit = NamedClip("Sit", 1.0f);

    AnimationStateComponent animState;
    animState.m_AvailableClips = { idle, sit };
    animState.m_CurrentClip = idle;
    animState.m_CurrentTime = 0.5f;
    animState.m_IsPlaying = true;
    animState.m_BlendDuration = 0.25f;
    animState.m_RequestedClip = "Sit";
    animState.m_RequestedLoop = false;

    AnimationSystem::Update(animState, *skeleton, 0.1f);

    EXPECT_TRUE(animState.m_RequestedClip.empty()) << "a request is consumed by the Update that applies it";
    EXPECT_EQ(animState.m_CurrentClipIndex, 1);
    ASSERT_TRUE(animState.m_Blending);
    EXPECT_EQ(animState.m_NextClip, sit);
    EXPECT_FALSE(animState.m_NextLoop);
    EXPECT_NEAR(animState.m_NextTime, 0.1f, 1e-5f) << "the target starts from its first frame";

    // Past the blend: Sit is current, keeps its one-shot flag, and holds its end.
    for (int i = 0; i < 20; ++i)
    {
        AnimationSystem::Update(animState, *skeleton, 0.1f);
    }
    EXPECT_FALSE(animState.m_Blending);
    EXPECT_EQ(animState.m_CurrentClip, sit);
    EXPECT_FALSE(animState.m_Loop);
    EXPECT_FLOAT_EQ(animState.m_CurrentTime, 1.0f);
}

TEST(AnimationSystem, ARequestForTheCurrentClipKeepsItsPlaceAndTakesTheLoopFlag)
{
    auto idle = NamedClip("Idle", 2.0f);

    AnimationStateComponent animState;
    animState.m_AvailableClips = { idle };
    animState.m_CurrentClip = idle;
    animState.m_CurrentTime = 0.75f;
    animState.m_RequestedClip = "Idle";
    animState.m_RequestedLoop = false;

    AnimationSystem::ApplyClipRequest(animState);

    // A script may write the request every frame; that must not restart the clip.
    EXPECT_FALSE(animState.m_Blending);
    EXPECT_FLOAT_EQ(animState.m_CurrentTime, 0.75f);
    EXPECT_FALSE(animState.m_Loop);
    EXPECT_TRUE(animState.m_RequestedClip.empty());
}

TEST(AnimationSystem, AnUnknownClipNameIsDroppedWithoutTouchingPlayback)
{
    auto idle = NamedClip("Idle", 2.0f);

    AnimationStateComponent animState;
    animState.m_AvailableClips = { idle };
    animState.m_CurrentClip = idle;
    animState.m_CurrentTime = 0.75f;
    animState.m_RequestedClip = "NoSuchClip";

    AnimationSystem::ApplyClipRequest(animState);

    EXPECT_TRUE(animState.m_RequestedClip.empty()) << "a bad name is not retried every frame";
    EXPECT_EQ(animState.m_CurrentClip, idle);
    EXPECT_FALSE(animState.m_Blending);
    EXPECT_FLOAT_EQ(animState.m_CurrentTime, 0.75f);
}

TEST(AnimationSystem, InterruptingABlendPastHalfwayStartsFromItsTarget)
{
    auto idle = NamedClip("Idle", 2.0f);
    auto walk = NamedClip("Walk", 1.0f);
    auto sit = NamedClip("Sit", 1.0f);

    AnimationStateComponent animState;
    animState.m_AvailableClips = { idle, walk, sit };
    animState.m_CurrentClip = idle;
    animState.m_CurrentTime = 1.5f;
    animState.m_Blending = true;
    animState.m_NextClip = walk;
    animState.m_NextTime = 0.3f;
    animState.m_NextLoop = true;
    animState.m_BlendFactor = 0.7f;
    animState.m_RequestedClip = "Sit";
    animState.m_RequestedLoop = false;

    AnimationSystem::ApplyClipRequest(animState);

    // The pose was 70 % Walk; blending on from Idle would snap it back.
    EXPECT_EQ(animState.m_CurrentClip, walk);
    EXPECT_FLOAT_EQ(animState.m_CurrentTime, 0.3f);
    EXPECT_TRUE(animState.m_Loop);
    EXPECT_EQ(animState.m_NextClip, sit);
    EXPECT_FLOAT_EQ(animState.m_BlendFactor, 0.0f);
    EXPECT_FALSE(animState.m_NextLoop);

    // Before halfway the source stays: the pose is still mostly Idle.
    AnimationStateComponent early = animState;
    early.m_CurrentClip = idle;
    early.m_CurrentTime = 1.5f;
    early.m_NextClip = walk;
    early.m_BlendFactor = 0.3f;
    early.m_RequestedClip = "Sit";
    AnimationSystem::ApplyClipRequest(early);
    EXPECT_EQ(early.m_CurrentClip, idle);
    EXPECT_EQ(early.m_NextClip, sit);
}

TEST(AnimationSystem, PlaybackSpeedScalesTheClipClockOnly)
{
    auto skeleton = CreateTwoBoneSkeleton(glm::mat4(1.0f));

    AnimationStateComponent half;
    half.m_CurrentClip = CreateBothBonesClip(2.0f);
    half.m_PlaybackSpeed = 0.5f;
    AnimationSystem::Update(half, *skeleton, 0.5f);
    EXPECT_NEAR(half.m_CurrentTime, 0.25f, 1e-5f);

    AnimationStateComponent frozen;
    frozen.m_CurrentClip = half.m_CurrentClip;
    frozen.m_CurrentTime = 0.8f;
    frozen.m_PlaybackSpeed = 0.0f;
    AnimationSystem::Update(frozen, *skeleton, 0.5f);
    EXPECT_FLOAT_EQ(frozen.m_CurrentTime, 0.8f) << "speed 0 freezes the pose";

    // A corrupt speed that bypassed every clamp plays as authored.
    AnimationStateComponent corrupt;
    corrupt.m_CurrentClip = half.m_CurrentClip;
    corrupt.m_PlaybackSpeed = std::numeric_limits<f32>::quiet_NaN();
    AnimationSystem::Update(corrupt, *skeleton, 0.5f);
    EXPECT_NEAR(corrupt.m_CurrentTime, 0.5f, 1e-5f);
}
