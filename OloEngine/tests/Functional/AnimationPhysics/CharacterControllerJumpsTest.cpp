#include "OloEnginePCH.h"

// =============================================================================
// CharacterControllerJumpsTest — Functional Test.
//
// Cross-subsystem seam under test:
//   JoltCharacterController × Jump impulse × gravity. Jump() schedules a
//   one-shot upward impulse that's consumed in the next PreSimulate via
//   m_JumpRequested. Then gravity kicks in and the character falls. The
//   integration concern: a regression that drops the m_JumpRequested
//   flag too early (consumed but not actually applied), or applies it
//   every frame instead of once (rocket-jump bug), or fails to clear
//   m_JumpPower after consumption (each jump grows by JumpPower).
//
// Scenario: stand a character on a floor, call Jump(), tick. Assert the
// character rose at least JumpPower * dt during the upward arc, then
// fell back down under gravity to land near the original height.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/JoltCharacterController.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class CharacterControllerJumpsTest : public FunctionalTest
{
  protected:
    static constexpr f32 kStartY = 1.0f;

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

        m_Character = GetScene().CreateEntity("Player");
        m_Character.GetComponent<TransformComponent>().Translation = { 0.0f, kStartY, 0.0f };
        CapsuleCollider3DComponent capsule;
        capsule.m_Radius = 0.4f;
        capsule.m_HalfHeight = 0.6f;
        m_Character.AddComponent<CapsuleCollider3DComponent>(capsule);
        m_Character.AddComponent<CharacterController3DComponent>();

        EnablePhysics3D();
    }

    [[nodiscard]] f32 CharacterY() const
    {
        return m_Character.GetComponent<TransformComponent>().Translation.y;
    }

    Entity m_Character;
};

TEST_F(CharacterControllerJumpsTest, JumpRisesThenFallsBackUnderGravity)
{
    auto* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);

    auto controller = joltScene->GetCharacterController(m_Character);
    ASSERT_TRUE(controller);

    // Let the character settle on the floor (Jolt's overlap correction
    // pushes the capsule slightly up from its initial intersection).
    TickFor(/*seconds=*/0.5f);
    const f32 yGrounded = CharacterY();
    ASSERT_TRUE(std::isfinite(yGrounded));

    // Jump.
    controller->Jump(/*jumpPower=*/8.0f);

    // Tick for a short moment — character should be rising.
    TickFor(/*seconds=*/0.2f);
    const f32 yMidJump = CharacterY();
    EXPECT_GT(yMidJump, yGrounded + 0.3f)
        << "character did not rise after Jump(); y went from " << yGrounded << " to "
        << yMidJump << " — Jolt's CharacterVirtual didn't consume m_JumpRequested or "
                       "ApplyGravityAndJump's grounded-jump branch is broken.";

    // Tick longer — character should fall back down under gravity.
    TickFor(/*seconds=*/2.0f);
    const f32 yFinal = CharacterY();
    EXPECT_TRUE(std::isfinite(yFinal));

    // Final y should be near the grounded baseline (same standing height,
    // because they're back on the same floor). Tolerate ~0.3m of solver
    // jitter — nothing huge.
    EXPECT_NEAR(yFinal, yGrounded, 0.3f)
        << "after the full jump arc, character ended at y=" << yFinal
        << " (baseline " << yGrounded << ") — either gravity didn't bring them down "
                                         "or the floor isn't catching them on landing.";
}
