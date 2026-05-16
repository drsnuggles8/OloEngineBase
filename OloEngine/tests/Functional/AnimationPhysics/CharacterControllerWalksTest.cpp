#include "OloEnginePCH.h"

// =============================================================================
// CharacterControllerWalksTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene × Physics3D × JoltCharacterController. The character-controller
//   path is distinct from the rigid-body path: it uses Jolt's
//   CharacterVirtual, has its own contact listener (the one whose API we
//   recently fixed for the new aggregated CharacterContact struct), and
//   ticks via JoltScene::Simulate's m_CharacterControllersToUpdate loop. A
//   regression in any of those — the OnComponentAdded hook, the
//   OnPhysics3DStart iteration, the contact-listener signature, or the
//   per-frame Update — silently breaks every player character in the
//   project.
//
// Scenario: an entity with a capsule collider + CharacterController3DComponent
// stands on a static floor, gets a horizontal SetLinearVelocity, and is
// expected to move. Asserts that after a fixed simulated duration, the
// entity's transform.x has advanced by approximately velocity × time.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class CharacterControllerWalksTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // Character — capsule collider + CharacterController3DComponent.
        // Same convention as the rigid-body path: collider FIRST so the
        // controller's shape resolves at construction.
        m_Character = GetScene().CreateEntity("Player");
        m_Character.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        CapsuleCollider3DComponent capsule;
        capsule.m_Radius = 0.4f;
        capsule.m_HalfHeight = 0.6f;
        m_Character.AddComponent<CapsuleCollider3DComponent>(capsule);
        m_Character.AddComponent<CharacterController3DComponent>();

        EnablePhysics3D();
    }

    Entity m_Character;
};

TEST_F(CharacterControllerWalksTest, SetLinearVelocityMovesEntityAlongGround)
{
    // Sanity: OnPhysics3DStart must have created a JoltCharacterController
    // for our entity. If this fails, the engine's character-controller
    // bootstrap is broken regardless of what the velocity does.
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr) << "Scene has no JoltScene after EnablePhysics3D";

    auto controller = joltScene->GetCharacterController(m_Character);
    ASSERT_TRUE(controller)
        << "JoltScene didn't create a controller for the entity — OnPhysics3DStart's "
           "CharacterController3DComponent iteration is missing or the OnComponentAdded "
           "hook didn't fire.";

    // Disable gravity and enable air-control so the test isolates the velocity-
    // application path from any groundedness/floor-collision concerns. The
    // CharacterController's velocity-integration loop only honours
    // SetLinearVelocity when grounded OR when ControlMovementInAir is true.
    controller->SetGravityEnabled(false);
    controller->SetControlMovementInAir(true);

    // Drive a horizontal velocity. CharacterVirtual integrates this each tick.
    constexpr glm::vec3 kVelocity{ 2.0f, 0.0f, 0.0f }; // 2 m/s along +X
    controller->SetLinearVelocity(kVelocity);

    const f32 startX = m_Character.GetComponent<TransformComponent>().Translation.x;

    // Tick for 1s. With v=2 m/s the entity should travel ~2m.
    TickFor(/*seconds=*/1.0f);

    const auto& t = m_Character.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z))
        << "character transform NaN/Inf";

    const f32 dx = t.x - startX;
    EXPECT_GT(dx, 1.5f)
        << "character did not walk; dx=" << dx
        << " (expected ~2.0 with v=2 m/s for 1s — "
           "if dx=0 the JoltCharacterController never integrated; if it's tiny "
           "the contact listener may be discarding contacts due to the recent "
           "CharacterContact API change).";
    EXPECT_LT(dx, 2.5f)
        << "character moved farther than expected; dx=" << dx
        << " (could be a runaway integration step or an unbounded velocity)";

    // With gravity off, vertical position should be approximately preserved.
    // Jolt's overlap-correction does push the controller up by a few tenths of
    // a meter when the capsule starts slightly intersecting the floor (which
    // is the right behaviour — that's the "skin" being unsquished). Tolerate
    // half a meter of vertical drift; anything beyond that means runaway.
    EXPECT_NEAR(t.y, 1.0f, 0.5f) << "character drifted vertically with gravity disabled; y=" << t.y;
}
