#include "OloEnginePCH.h"

// =============================================================================
// ScenePauseFreezesAllSubsystemsTest — Functional Test.
//
// Cross-subsystem seam under test:
//   `Scene::SetPaused(true)` is supposed to freeze every per-frame subsystem
//   simultaneously (physics body integration, animation time, scripts,
//   dialogue, etc.) while leaving non-tick concerns like scene streaming
//   alive. The integration concern: each subsystem honours the pause flag
//   in lockstep. A regression where physics ignores pause but animation
//   respects it (or vice versa) lets the player observe a frozen character
//   sliding along the floor — a class of bug no per-subsystem test sees.
//
// Scenario: a falling sphere + an animated entity ticking together.
//   Phase 1: tick unpaused → both should advance.
//   Phase 2: SetPaused(true), tick → neither should advance.
//   Phase 3: SetPaused(false), tick → both resume.
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
    Ref<Skeleton> MakeSingleBoneSkeleton()
    {
        auto skeleton = Ref<Skeleton>::Create(1);
        skeleton->m_BoneNames = { "Root" };
        skeleton->m_ParentIndices = { -1 };
        skeleton->m_LocalTransforms = { glm::mat4(1.0f) };
        skeleton->m_BonePreTransforms = { glm::mat4(1.0f) };
        skeleton->m_GlobalTransforms[0] = skeleton->m_LocalTransforms[0];
        skeleton->SetBindPose();
        return skeleton;
    }

    Ref<AnimationClip> MakeIdleClip()
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "PauseIdle";
        clip->Duration = 2.0f;

        BoneAnimation boneAnim;
        boneAnim.BoneName = "Root";
        boneAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
        boneAnim.PositionKeys.push_back({ 2.0, glm::vec3(0.0f, 0.5f, 0.0f) });
        boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ 2.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
        boneAnim.ScaleKeys.push_back({ 2.0, glm::vec3(1.0f) });
        clip->BoneAnimations.push_back(boneAnim);
        clip->InitializeBoneCache();
        return clip;
    }
} // namespace

class ScenePauseFreezesAllSubsystemsTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Floor — well below where the body will be after Phase 1 so the
        // body is still mid-air when we pause (i.e. has somewhere to fall).
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -50.0f, 0.0f };
        auto& fb = floor.AddComponent<Rigidbody3DComponent>();
        fb.m_Type = BodyType3D::Static;
        auto& fc = floor.AddComponent<BoxCollider3DComponent>();
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };

        // Falling body, started high so it's still in free-fall after Phase 1.
        m_Body = GetScene().CreateEntity("Body");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
        auto& body = m_Body.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_LinearDrag = 0.0f;
        auto& col = m_Body.AddComponent<SphereCollider3DComponent>();
        col.m_Radius = 0.5f;

        // Animated entity ticking in parallel.
        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(MakeSingleBoneSkeleton());
        auto& animState = m_Animated.AddComponent<AnimationStateComponent>();
        animState.m_CurrentClip = MakeIdleClip();
        animState.m_IsPlaying = true;
        animState.m_CurrentTime = 0.0f;

        EnableAnimation();
        EnablePhysics3D();
    }

    [[nodiscard]] f32 BodyY() const
    {
        return m_Body.GetComponent<TransformComponent>().Translation.y;
    }

    [[nodiscard]] f32 AnimTime() const
    {
        return m_Animated.GetComponent<AnimationStateComponent>().m_CurrentTime;
    }

    Entity m_Body;
    Entity m_Animated;
};

TEST_F(ScenePauseFreezesAllSubsystemsTest, PauseStopsAllTickingSubsystemsResumeRestartsThem)
{
    // -------- Phase 1: unpaused, both subsystems advance ---------------
    ASSERT_FALSE(GetScene().IsPaused()) << "scene started paused unexpectedly";

    const f32 startY = BodyY();
    const f32 startAnim = AnimTime();

    RunFrames(/*count=*/15); // 0.25s

    const f32 phase1Y = BodyY();
    const f32 phase1Anim = AnimTime();

    ASSERT_LT(phase1Y, startY - 0.05f)
        << "body did not fall during unpaused tick — physics not bound or pause already on";
    ASSERT_GT(phase1Anim, startAnim + 0.01f)
        << "animation did not advance during unpaused tick";

    // -------- Phase 2: paused, both subsystems freeze ------------------
    GetScene().SetPaused(true);
    ASSERT_TRUE(GetScene().IsPaused());

    RunFrames(/*count=*/30); // 0.5s of "wall time" but no simulated tick

    const f32 phase2Y = BodyY();
    const f32 phase2Anim = AnimTime();

    EXPECT_NEAR(phase2Y, phase1Y, 1e-4f)
        << "body advanced while scene was paused — physics ignored pause flag";
    EXPECT_NEAR(phase2Anim, phase1Anim, 1e-4f)
        << "animation advanced while scene was paused — animation ignored pause flag";

    // -------- Phase 3: resumed, both subsystems pick up again ----------
    GetScene().SetPaused(false);
    ASSERT_FALSE(GetScene().IsPaused());

    RunFrames(/*count=*/15); // 0.25s

    const f32 phase3Y = BodyY();
    const f32 phase3Anim = AnimTime();

    EXPECT_LT(phase3Y, phase2Y - 0.05f)
        << "body did not resume falling after unpause — physics did not restart";
    EXPECT_GT(phase3Anim, phase2Anim + 0.01f)
        << "animation did not resume after unpause — animation did not restart";

    // No NaNs leaked anywhere across the pause/resume boundary.
    EXPECT_TRUE(std::isfinite(phase3Y)) << "body y is NaN after pause/resume cycle";
    EXPECT_TRUE(std::isfinite(phase3Anim)) << "animation time is NaN after pause/resume cycle";
}
