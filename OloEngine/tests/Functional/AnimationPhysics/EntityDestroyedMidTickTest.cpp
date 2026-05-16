#include "OloEnginePCH.h"

// =============================================================================
// EntityDestroyedMidTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene × Physics × Animation lifecycle. When the player destroys an
//   entity at runtime (an enemy dies, a dropped item is picked up, a
//   projectile expires), every subsystem holding state for that entity has
//   to release it cleanly while the scene keeps ticking. Forgetting to
//   destroy the Jolt body, dangling pointers in the contact listener, or
//   the animation tick walking a stale view are common production bugs.
//
// Scenario: three dynamic bodies (A/B/C) falling toward a floor with an
// animated entity ticking in parallel. Mid-flight (~0.25s in), destroy B.
// Continue ticking. Assert that:
//   (a) the destroy itself didn't crash,
//   (b) A and C land cleanly with finite transforms,
//   (c) the animation entity keeps advancing,
//   (d) B is gone from the scene.
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
        clip->Name = "DestroyMidTickIdle";
        clip->Duration = 1.0f;
        BoneAnimation boneAnim;
        boneAnim.BoneName = "Root";
        boneAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
        boneAnim.PositionKeys.push_back({ 1.0, glm::vec3(0.0f, 0.5f, 0.0f) });
        boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.RotationKeys.push_back({ 1.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
        boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
        boneAnim.ScaleKeys.push_back({ 1.0, glm::vec3(1.0f) });
        clip->BoneAnimations.push_back(boneAnim);
        clip->InitializeBoneCache();
        return clip;
    }

    Entity MakeFallingSphere(Scene& scene, const std::string& name, glm::vec3 pos)
    {
        auto e = scene.CreateEntity(name);
        e.GetComponent<TransformComponent>().Translation = pos;
        auto& body = e.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        auto& col = e.AddComponent<SphereCollider3DComponent>();
        col.m_Radius = 0.4f;
        return e;
    }
} // namespace

class EntityDestroyedMidTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& fb = floor.AddComponent<Rigidbody3DComponent>();
        fb.m_Type = BodyType3D::Static;
        auto& fc = floor.AddComponent<BoxCollider3DComponent>();
        fc.m_HalfExtents = { 50.0f, 0.5f, 50.0f };

        // Three falling spheres laterally separated so they don't collide
        // with each other before B is destroyed.
        m_A = MakeFallingSphere(GetScene(), "A", { -2.0f, 5.0f, 0.0f });
        m_B = MakeFallingSphere(GetScene(), "B", { 0.0f, 5.0f, 0.0f });
        m_C = MakeFallingSphere(GetScene(), "C", { 2.0f, 5.0f, 0.0f });

        // Animation canary.
        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(MakeSingleBoneSkeleton());
        auto& anim = m_Animated.AddComponent<AnimationStateComponent>();
        anim.m_CurrentClip = MakeIdleClip();
        anim.m_IsPlaying = true;

        EnableAnimation();
        EnablePhysics3D();
    }

    Entity m_A;
    Entity m_B;
    Entity m_C;
    Entity m_Animated;
};

TEST_F(EntityDestroyedMidTickTest, DestroyingOneBodyDoesNotBreakOthersOrAnimation)
{
    // Phase 1: tick a bit so all three bodies are mid-air with non-zero
    // velocity. Snapshot the animation time as our advancement baseline.
    RunFrames(/*count=*/15); // 0.25s

    const f32 animBeforeDestroy = m_Animated.GetComponent<AnimationStateComponent>().m_CurrentTime;
    ASSERT_GT(animBeforeDestroy, 0.0f);

    // Snapshot A and C state pre-destroy. We don't snapshot B because we're
    // about to destroy it.
    const glm::vec3 aMid = m_A.GetComponent<TransformComponent>().Translation;
    const glm::vec3 cMid = m_C.GetComponent<TransformComponent>().Translation;
    ASSERT_LT(aMid.y, 5.0f); // both fell at least a little
    ASSERT_LT(cMid.y, 5.0f);

    // Phase 2: destroy the middle entity while the simulation is live.
    GetScene().DestroyEntity(m_B);
    m_B = Entity{}; // null out our handle so we don't accidentally touch it

    // Phase 3: continue ticking. This is the real assertion — the next
    // OnUpdateRuntime must not crash when iterating physics views, the
    // contact listener must not dereference B's freed body, etc.
    RunFrames(/*count=*/45); // 0.75s — long enough for A and C to land

    // (a)+(b) A and C have finite positions and ended up at/near the floor.
    const glm::vec3 aEnd = m_A.GetComponent<TransformComponent>().Translation;
    const glm::vec3 cEnd = m_C.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(aEnd.x) && std::isfinite(aEnd.y) && std::isfinite(aEnd.z))
        << "A transform NaN/Inf after B was destroyed mid-tick";
    EXPECT_TRUE(std::isfinite(cEnd.x) && std::isfinite(cEnd.y) && std::isfinite(cEnd.z))
        << "C transform NaN/Inf after B was destroyed mid-tick";
    EXPECT_LT(aEnd.y, 1.0f) << "A failed to land after B's destruction; y=" << aEnd.y;
    EXPECT_LT(cEnd.y, 1.0f) << "C failed to land after B's destruction; y=" << cEnd.y;

    // (c) Animation kept advancing — the destroy didn't poison the per-frame
    // tick of unrelated subsystems.
    const f32 animAfter = m_Animated.GetComponent<AnimationStateComponent>().m_CurrentTime;
    EXPECT_GT(animAfter, animBeforeDestroy + 0.05f)
        << "animation tick did not advance after entity destruction";

    // (d) B is actually gone. Sweep all entities by tag and confirm no "B".
    auto view = GetScene().GetAllEntitiesWith<TagComponent>();
    bool foundB = false;
    for (auto e : view)
    {
        Entity entity{ e, &GetScene() };
        if (entity.GetComponent<TagComponent>().Tag == "B")
        {
            foundB = true;
            break;
        }
    }
    EXPECT_FALSE(foundB) << "destroyed entity B is still present in the scene";
}
