#include "OloEnginePCH.h"

// =============================================================================
// CharacterPushesDynamicBodyTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Physics3D × character controller × dynamic-body interaction. The
//   character controller's `OnAdjustBodyVelocity` callback (which we just
//   re-checked during the Jolt CharacterContact API drift fix) is what
//   lets a player character push physics objects in the world — kicking
//   crates, knocking over enemies, shoving doors. A regression there is
//   the "ghost player" bug: the character can pass through dynamic
//   bodies without disturbing them, breaking every gameplay interaction
//   that relies on physical pushing.
//
// Scenario: a dynamic crate sits in front of the character. The
// character walks forward into it. After ticking, assert the crate has
// moved AT LEAST some distance along the push direction — proves the
// character–body interaction is wired through OnAdjustBodyVelocity.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class CharacterPushesDynamicBodyTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Floor.
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // Crate — dynamic body in front of the character.
        m_Crate = GetScene().CreateEntity("Crate");
        m_Crate.GetComponent<TransformComponent>().Translation = { 2.0f, 0.5f, 0.0f };
        SphereCollider3DComponent crateCol;
        crateCol.m_Radius = 0.4f;
        m_Crate.AddComponent<SphereCollider3DComponent>(crateCol);
        Rigidbody3DComponent crateBody;
        crateBody.m_Type = BodyType3D::Dynamic;
        crateBody.m_Mass = 0.5f; // light enough to be pushed
        crateBody.m_LinearDrag = 0.5f;
        m_Crate.AddComponent<Rigidbody3DComponent>(crateBody);

        // Character at origin, walking +x.
        m_Character = GetScene().CreateEntity("Player");
        m_Character.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
        CapsuleCollider3DComponent capsule;
        capsule.m_Radius = 0.4f;
        capsule.m_HalfHeight = 0.6f;
        m_Character.AddComponent<CapsuleCollider3DComponent>(capsule);
        m_Character.AddComponent<CharacterController3DComponent>();

        EnablePhysics3D();
    }

    Entity m_Crate;
    Entity m_Character;
};

TEST_F(CharacterPushesDynamicBodyTest, WalkingIntoDynamicBodyMovesIt)
{
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);

    auto controller = joltScene->GetCharacterController(m_Character);
    ASSERT_TRUE(controller);
    controller->SetGravityEnabled(false);
    controller->SetControlMovementInAir(true);
    controller->SetLinearVelocity({ 3.0f, 0.0f, 0.0f }); // Toward the crate.

    const f32 crateStartX = m_Crate.GetComponent<TransformComponent>().Translation.x;

    // Tick long enough for the character to reach the crate (~0.5s) and
    // for several frames of contact (push) afterwards.
    TickFor(/*seconds=*/2.0f);

    const auto& crateT = m_Crate.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(crateT.x) && std::isfinite(crateT.y) && std::isfinite(crateT.z))
        << "crate transform NaN/Inf";

    const f32 crateDx = crateT.x - crateStartX;
    EXPECT_GT(crateDx, 0.2f)
        << "the character did not push the crate forward; crate x went from "
        << crateStartX << " to " << crateT.x
        << " — JoltCharacterController::OnAdjustBodyVelocity is not interacting "
           "with the dynamic body, or the controller is configured to ignore "
           "dynamic-body collision.";

    // The crate should move primarily along +X; sanity-check it didn't get
    // launched into the sky or sideways by a degenerate impulse.
    EXPECT_NEAR(crateT.y, 0.4f, 1.0f) << "crate launched vertically; y=" << crateT.y;
    EXPECT_NEAR(crateT.z, 0.0f, 1.0f) << "crate ejected on Z; z=" << crateT.z;
}
