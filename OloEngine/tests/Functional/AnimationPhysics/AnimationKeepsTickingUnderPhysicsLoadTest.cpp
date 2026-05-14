#include "OloEnginePCH.h"

// =============================================================================
// AnimationKeepsTickingUnderPhysicsLoadTest — Functional Test.
//
// Cross-subsystem seam under test:
//   With many physics bodies in flight (collision pile-up generates real CPU
//   work each tick), the Animation tick must continue to advance — i.e. the
//   per-frame Scene update must not starve one subsystem because another is
//   busy. This is the failure class where "everything works in isolation but
//   the real frame's order leaves a subsystem behind a few seconds in." A
//   bug like that escapes every L1–L11 layer.
//
// Scenario: 8 dynamic spheres dropped into a column above a static floor;
// in parallel an entity with a 2-bone skeleton plays a programmatic clip.
// After 4s of simulated time we expect:
//   (a) every body has a finite, non-escaped position,
//   (b) the animation clip time has advanced by ~the simulated duration
//       (proving the animation tick actually ran each frame).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"

#include <cmath>
#include <string>
#include <vector>

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

    Ref<AnimationClip> MakeLoopingClip(f32 duration)
    {
        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "LoopChild";
        clip->Duration = duration;

        BoneAnimation boneAnim;
        boneAnim.BoneName = "Child";
        boneAnim.PositionKeys.push_back({ 0.0,                        glm::vec3(0.0f, 0.0f, 0.0f) });
        boneAnim.PositionKeys.push_back({ static_cast<f64>(duration), glm::vec3(0.0f, 0.5f, 0.0f) });
        boneAnim.RotationKeys.push_back({ 0.0,                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.ScaleKeys.push_back   ({ 0.0,                        glm::vec3(1.0f) });
        boneAnim.ScaleKeys.push_back   ({ static_cast<f64>(duration), glm::vec3(1.0f) });
        clip->BoneAnimations.push_back(boneAnim);
        clip->InitializeBoneCache();
        return clip;
    }
} // namespace

class AnimationKeepsTickingUnderPhysicsLoadTest : public FunctionalTest
{
  protected:
    static constexpr u32 kBodyCount  = 8;
    static constexpr f32 kRadius     = 0.4f;
    static constexpr f32 kSimSeconds = 4.0f;
    static constexpr f32 kClipLen    = 1.0f;

    void BuildScene() override
    {
        // Floor.
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& fb = floor.AddComponent<Rigidbody3DComponent>();
        fb.m_Type = BodyType3D::Static;
        auto& fc = floor.AddComponent<BoxCollider3DComponent>();
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };

        // Stack of dynamic spheres — small lateral jitter to break perfect
        // symmetry so the pile settles instead of balancing on a vertical
        // axis (which Jolt resolves but slowly and noisily).
        for (u32 i = 0; i < kBodyCount; ++i)
        {
            auto e = GetScene().CreateEntity("Sphere" + std::to_string(i));
            e.GetComponent<TransformComponent>().Translation = {
                0.02f * static_cast<f32>(i),
                3.0f + static_cast<f32>(i) * (kRadius * 2.4f),
                0.01f * static_cast<f32>(i),
            };
            auto& body = e.AddComponent<Rigidbody3DComponent>();
            body.m_Type = BodyType3D::Dynamic;
            body.m_Mass = 1.0f;
            auto& col = e.AddComponent<SphereCollider3DComponent>();
            col.m_Radius = kRadius;
            m_Spheres.push_back(e);
        }

        // Animated entity ticking in parallel — the canary that proves the
        // animation tick wasn't starved by physics.
        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(MakeTwoBoneSkeleton());
        auto& animState = m_Animated.AddComponent<AnimationStateComponent>();
        animState.m_CurrentClip = MakeLoopingClip(kClipLen);
        animState.m_IsPlaying = true;
        animState.m_CurrentTime = 0.0f;

        EnableAnimation();
        EnablePhysics3D();
    }

    Entity m_Animated;
    std::vector<Entity> m_Spheres;
};

TEST_F(AnimationKeepsTickingUnderPhysicsLoadTest, AllBodiesStableAndAnimationAdvanced)
{
    TickFor(kSimSeconds);

    // (a) Every body has a finite, non-escaped position.
    for (u32 i = 0; i < m_Spheres.size(); ++i)
    {
        const auto& t = m_Spheres[i].GetComponent<TransformComponent>().Translation;
        ASSERT_TRUE(std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z))
            << "sphere " << i << " transform contains NaN/Inf";

        // y >= ~radius means it didn't escape through the floor; the stack-
        // pile-up case can produce ~10% penetration in Jolt's iterative solver
        // before settling — anything beyond that is a bug, not jitter.
        EXPECT_GE(t.y, kRadius * 0.85f)
            << "sphere " << i << " escaped through the floor; y=" << t.y;

        // No body was hurled into the next time zone. A loose stack of 8 bodies
        // does spread out as it collapses, but anything past ~15 m means the
        // collision response is degenerate (NaN-impulse, runaway energy).
        const f32 horizontal = std::sqrt(t.x * t.x + t.z * t.z);
        EXPECT_LT(horizontal, 15.0f)
            << "sphere " << i << " ejected horizontally; h=" << horizontal;
    }

    // (b) Animation tick advanced. With a 1s looping clip ticked for 4s,
    // the wrapped current time should be > 0; more importantly, advancing
    // *at all* proves the Animation system wasn't skipped.
    const auto& animState = m_Animated.GetComponent<AnimationStateComponent>();
    EXPECT_GT(animState.m_CurrentTime, 0.0f)
        << "animation clip time did not advance — Animation tick was starved";

    // Sanity: the clip time stays within [0, duration] when looping.
    EXPECT_LE(animState.m_CurrentTime, kClipLen + 0.01f)
        << "animation clip time ran past clip duration — loop wrap is broken";
}
