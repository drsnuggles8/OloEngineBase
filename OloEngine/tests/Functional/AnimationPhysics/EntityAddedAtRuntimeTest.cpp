#include "OloEnginePCH.h"

// =============================================================================
// EntityAddedAtRuntimeTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene × Physics3D component-add lifecycle. Gameplay code spawns
//   entities at runtime constantly — projectiles, dropped loot, particle
//   debris, networked actors arriving from the server. Each one expects
//   "I just added a Rigidbody3DComponent, the body should start
//   simulating on the next tick." The existing OnPhysics3DStart only
//   iterates entities present at start time; if there's no per-add hook
//   wiring runtime entities into JoltScene, this entire workflow silently
//   does nothing — and no per-subsystem test catches it because they all
//   set up their bodies before starting physics.
//
// Scenario: build a baseline scene with one body so OnPhysics3DStart has
// at least one body to bind. Tick a few frames so physics is "live."
// Then create a *new* entity with Rigidbody3D + collider. Tick more
// frames. Assert the runtime-added body actually falls.
//
// Note: this test may legitimately fail today if OloEngine has no
// component-add hook into JoltScene. If it does, the failure documents a
// real engine gap — exactly what Functional tests exist to surface. Either way the
// signal is informative; do not weaken the assertion.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;
using namespace OloEngine::Animation;

class EntityAddedAtRuntimeTest : public FunctionalTest
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

        // Baseline body so JoltScene has at least one entity to bind on
        // start — keeps the test honest about the "runtime add" path being
        // what's exercised, not a degenerate empty-scene edge case.
        m_Baseline = GetScene().CreateEntity("Baseline");
        m_Baseline.GetComponent<TransformComponent>().Translation = { -3.0f, 5.0f, 0.0f };
        auto& bb = m_Baseline.AddComponent<Rigidbody3DComponent>();
        bb.m_Type = BodyType3D::Dynamic;
        bb.m_Mass = 1.0f;
        auto& bc = m_Baseline.AddComponent<SphereCollider3DComponent>();
        bc.m_Radius = 0.4f;

        // Animation canary — proves the per-frame tick is alive across the
        // whole test.
        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());
        auto& anim = m_Animated.AddComponent<AnimationStateComponent>();
        anim.m_CurrentClip = Fixtures::MakeTranslationClip(/*duration=*/1.0f);
        anim.m_IsPlaying = true;

        EnableAnimation();
        EnablePhysics3D();
    }

    Entity m_Baseline;
    Entity m_Animated;
};

TEST_F(EntityAddedAtRuntimeTest, BodyAddedAfterPhysicsStartActuallySimulates)
{
    // Phase 1: Run baseline physics for a moment so we know the world is
    // ticking before we add the runtime entity.
    RunFrames(/*count=*/12); // 0.2s
    const f32 baselineMidY = m_Baseline.GetComponent<TransformComponent>().Translation.y;
    ASSERT_LT(baselineMidY, 5.0f - 0.05f)
        << "baseline body did not fall — physics never bound; runtime-add test would be vacuous";

    // Phase 2: spawn the entity AFTER physics is live.
    // Two ordering rules enforced by the engine's component-add hook:
    //   1. Collider FIRST, Rigidbody SECOND — the OnComponentAdded hook on
    //      Rigidbody3DComponent constructs the Jolt body and reads the
    //      collider shape at that moment.
    //   2. Pre-populate the Rigidbody3DComponent BEFORE AddComponent — the
    //      hook reads m_Type/m_Mass at construction. Setting them after
    //      AddComponent leaves the body as a default Static body.
    Entity runtimeBody = GetScene().CreateEntity("RuntimeAdded");
    const glm::vec3 spawnPos{ 3.0f, 5.0f, 0.0f };
    runtimeBody.GetComponent<TransformComponent>().Translation = spawnPos;

    SphereCollider3DComponent colTemplate;
    colTemplate.m_Radius = 0.4f;
    runtimeBody.AddComponent<SphereCollider3DComponent>(colTemplate);

    Rigidbody3DComponent bodyTemplate;
    bodyTemplate.m_Type = BodyType3D::Dynamic;
    bodyTemplate.m_Mass = 1.0f;
    runtimeBody.AddComponent<Rigidbody3DComponent>(bodyTemplate);

    // Phase 3: tick. With gravity ≈ 9.81 m/s², 0.5s gives a free-fall delta
    // of ~1.2m — comfortably bigger than any spurious solver jitter.
    RunFrames(/*count=*/30); // 0.5s

    const glm::vec3 endPos = runtimeBody.GetComponent<TransformComponent>().Translation;

    EXPECT_TRUE(std::isfinite(endPos.x) && std::isfinite(endPos.y) && std::isfinite(endPos.z))
        << "runtime-added body transform contains NaN/Inf";

    // The headline assertion: did the runtime-added body ACTUALLY simulate?
    // If this fails, OloEngine has no component-add hook into JoltScene,
    // and gameplay code that spawns physics entities at runtime is silently
    // broken. Document via the failure message.
    EXPECT_LT(endPos.y, spawnPos.y - 0.5f)
        << "runtime-added body did not fall — y stayed at " << endPos.y
        << " (spawned at " << spawnPos.y << "). Physics3D has no component-add"
        << " hook wired into JoltScene; runtime-spawned bodies silently never"
        << " simulate. This is the bug, not the test.";

    // Sanity: baseline kept falling too (no global crash hidden by the
    // primary assertion).
    const f32 baselineEndY = m_Baseline.GetComponent<TransformComponent>().Translation.y;
    EXPECT_LT(baselineEndY, baselineMidY)
        << "baseline body stopped falling after runtime add — runtime add disturbed live simulation";
}
