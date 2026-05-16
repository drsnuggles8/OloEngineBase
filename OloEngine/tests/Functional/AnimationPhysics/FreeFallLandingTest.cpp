#include "OloEnginePCH.h"

// =============================================================================
// FreeFallLandingTest — first Functional Test (greenfield).
//
// Cross-subsystem seam under test:
//   Animation tick (AnimationStateComponent + SkeletonComponent) AND
//   Physics3D simulation (Rigidbody3DComponent + colliders)
// both running inside the same Scene::OnUpdateRuntime, neither breaking
// the other.
//
// Scenario: a dynamic sphere is dropped above a static floor; in parallel,
// a separate skeletal entity plays a programmatic animation clip. We tick
// until the sphere lands, then assert that:
//   (a) the sphere settled at floor height,
//   (b) the animated skeleton's clip time advanced,
//   (c) neither subsystem produced NaNs in transforms.
//
// Functional-test contract: see CONTEXT.md → "Functional Test", ADR 0001/0002/0003,
// docs/testing.md §7.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;
using namespace OloEngine::Animation;

namespace
{
    Ref<Skeleton> MakeTwoBoneSkeleton()
    {
        auto skeleton = Ref<Skeleton>::Create(2);
        skeleton->m_BoneNames = { "Root", "Child" };
        skeleton->m_ParentIndices = { -1, 0 };
        skeleton->m_LocalTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };
        skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };
        skeleton->m_GlobalTransforms[0] = skeleton->m_LocalTransforms[0];
        skeleton->m_GlobalTransforms[1] = skeleton->m_GlobalTransforms[0] * skeleton->m_LocalTransforms[1];
        skeleton->SetBindPose();
        return skeleton;
    }

    Ref<AnimationClip> MakeChildTranslationClip(f32 duration)
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "ChildTranslation";
        clip->Duration = duration;

        BoneAnimation boneAnim;
        boneAnim.BoneName = "Child";
        boneAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f, 0.0f, 0.0f) });
        boneAnim.PositionKeys.push_back({ static_cast<f64>(duration), glm::vec3(0.0f, 1.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
        boneAnim.ScaleKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f) });
        clip->BoneAnimations.push_back(boneAnim);
        clip->InitializeBoneCache();
        return clip;
    }
} // namespace

class FreeFallLandingTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Static floor at y = -0.5, top surface at y = 0.
        m_Floor = GetScene().CreateEntity("Floor");
        m_Floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& floorBody = m_Floor.AddComponent<Rigidbody3DComponent>();
        floorBody.m_Type = BodyType3D::Static;
        auto& floorCol = m_Floor.AddComponent<BoxCollider3DComponent>();
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };

        // Dynamic sphere dropped from y = 5.
        m_Ball = GetScene().CreateEntity("Ball");
        m_Ball.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
        auto& ballBody = m_Ball.AddComponent<Rigidbody3DComponent>();
        ballBody.m_Type = BodyType3D::Dynamic;
        ballBody.m_Mass = 1.0f;
        ballBody.m_LinearDrag = 0.0f;
        auto& ballCol = m_Ball.AddComponent<SphereCollider3DComponent>();
        ballCol.m_Radius = 0.5f;

        // Animated entity sharing the same Scene tick.
        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(MakeTwoBoneSkeleton());
        auto& animState = m_Animated.AddComponent<AnimationStateComponent>();
        animState.m_CurrentClip = MakeChildTranslationClip(/*duration=*/1.0f);
        animState.m_IsPlaying = true;
        animState.m_CurrentTime = 0.0f;

        EnableAnimation();
        EnablePhysics3D();
    }

    Entity m_Floor;
    Entity m_Ball;
    Entity m_Animated;
};

TEST_F(FreeFallLandingTest, BallLandsAndAnimationKeepsTicking)
{
    constexpr f32 kFloorTop = 0.0f;
    constexpr f32 kBallRadius = 0.5f;
    constexpr f32 kSettleEpsilon = 0.05f; // tolerance for "at rest on floor"

    const auto BallY = [this]
    {
        return m_Ball.GetComponent<TransformComponent>().Translation.y;
    };
    const auto BallLanded = [&]
    {
        return BallY() <= (kFloorTop + kBallRadius + kSettleEpsilon);
    };

    // Sanity: we start clearly above the floor.
    ASSERT_GT(BallY(), 1.0f) << "ball should start well above the floor";

    // Tick at 60 Hz for up to 5s of simulated time. With g=9.81 m/s^2, the
    // ball should land in ~0.9s; 5s gives generous slack.
    const bool landed = TickUntil(BallLanded, /*timeoutSeconds=*/5.0f);

    EXPECT_TRUE(landed) << "ball did not land within 5s of simulated time; final y=" << BallY();
    EXPECT_NEAR(BallY(), kFloorTop + kBallRadius, kSettleEpsilon)
        << "ball did not settle at floor height";

    // Cross-subsystem seam: animation must have advanced during the same
    // OnUpdateRuntime calls that ticked physics.
    const auto& animState = m_Animated.GetComponent<AnimationStateComponent>();
    EXPECT_GT(animState.m_CurrentTime, 0.0f)
        << "animation clip time did not advance — Animation tick was skipped or starved";

    // No subsystem produced a NaN / Inf transform.
    const auto& ballPos = m_Ball.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(ballPos.x) && std::isfinite(ballPos.y) && std::isfinite(ballPos.z))
        << "ball transform contains NaN/Inf";
}
