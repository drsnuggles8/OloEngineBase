#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// WorldOriginRebasePhysicsTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::RebaseOrigin (floating-origin, issue #429) × JoltScene physics.
//   A rebase shifts every stored world position — ECS transforms AND live Jolt
//   bodies — by the same delta in one atomic game-thread pass. The physics
//   simulation must not notice: velocities are preserved (a body in flight
//   keeps moving), and static geometry shifts with the dynamic bodies (a body
//   resting on a floor stays resting instead of falling through a floor that
//   moved out from under it).
//
// This is the "passes math tests but behaves broken" failure mode CLAUDE.md
// warns about: the transform-only unit test (WorldOriginRebaseTest) proves the
// arithmetic; only driving real Jolt across a rebase proves the physics body
// shift is velocity/COM-correct and that the static floor moved too.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class WorldOriginRebasePhysicsTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // A projectile with a known horizontal velocity, gravity + drag off, so
        // its motion is a clean straight line we can predict across a rebase.
        m_Body = GetScene().CreateEntity("Projectile");
        m_Body.GetComponent<TransformComponent>().Translation = { 0.0f, 50.0f, 0.0f };
        SphereCollider3DComponent col;
        col.m_Radius = 0.3f;
        m_Body.AddComponent<SphereCollider3DComponent>(col);
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        body.m_DisableGravity = true;
        body.m_LinearDrag = 0.0f;
        body.m_InitialLinearVelocity = { 10.0f, 0.0f, 0.0f }; // 10 m/s along +x
        m_Body.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();
    }

    Entity m_Body;
};

TEST_F(WorldOriginRebasePhysicsTest, RebaseShiftsBodyAndPreservesVelocity)
{
    // Fly for 0.5 s: x ~= 5.
    TickFor(0.5f);
    const f32 xBeforeRebase = m_Body.GetComponent<TransformComponent>().Translation.x;
    EXPECT_NEAR(xBeforeRebase, 5.0f, 0.3f) << "projectile didn't fly as expected before rebase";

    // Rebase the whole world 1024 m along -x (as the origin trigger would once
    // the camera passed the threshold). The body should teleport with it.
    const glm::vec3 shift{ -1024.0f, 0.0f, 0.0f };
    GetScene().RebaseOrigin(shift);

    // Snapshot BY VALUE — TransformComponent.Translation is live and would keep
    // changing as the body flies on below, aliasing xFinal.
    const glm::vec3 afterShift = m_Body.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(afterShift.x, xBeforeRebase + shift.x, 0.05f)
        << "body transform did not shift with the rebase";
    EXPECT_TRUE(std::isfinite(afterShift.x) && std::isfinite(afterShift.y) && std::isfinite(afterShift.z));

    // Absolute (authored-frame) position is recovered by the offset.
    EXPECT_NEAR(GetScene().RebasedToAbsolute(afterShift).x, xBeforeRebase, 0.05f);
    EXPECT_NEAR(GetScene().GetWorldOrigin().x, 1024.0f, 1e-3f);

    // Velocity was preserved by the shift (DontActivate SetPosition): another
    // 0.5 s advances x by ~5 more from the shifted position, not from rest.
    TickFor(0.5f);
    const f32 xFinal = m_Body.GetComponent<TransformComponent>().Translation.x;
    EXPECT_NEAR(xFinal, afterShift.x + 5.0f, 0.3f)
        << "body did not keep moving after the rebase — velocity was lost in the shift";

    // And the absolute trajectory is a continuous ~10 m over the full second.
    EXPECT_NEAR(GetScene().RebasedToAbsolute(m_Body.GetComponent<TransformComponent>().Translation).x,
                10.0f, 0.5f);
}

// ---------------------------------------------------------------------------
// Second scenario: a body settled on a STATIC floor must stay settled after a
// rebase — proving the static floor body shifted too (a bug that only shifted
// dynamic bodies would drop the resting body through the moved-away floor).
// ---------------------------------------------------------------------------
class WorldOriginRebaseFloorTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Static floor at y = 0.
        m_Floor = GetScene().CreateEntity("Floor");
        m_Floor.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        m_Floor.AddComponent<BoxCollider3DComponent>(floorCol);
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        m_Floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // Dynamic box dropped just above the floor.
        m_Box = GetScene().CreateEntity("Box");
        m_Box.GetComponent<TransformComponent>().Translation = { 0.0f, 2.0f, 0.0f };
        BoxCollider3DComponent boxCol;
        boxCol.m_HalfExtents = { 0.5f, 0.5f, 0.5f };
        m_Box.AddComponent<BoxCollider3DComponent>(boxCol);
        Rigidbody3DComponent boxBody;
        boxBody.m_Type = BodyType3D::Dynamic;
        boxBody.m_Mass = 1.0f;
        m_Box.AddComponent<Rigidbody3DComponent>(boxBody);

        EnablePhysics3D();
    }

    Entity m_Floor;
    Entity m_Box;
};

TEST_F(WorldOriginRebaseFloorTest, RestingBodyStaysOnFloorAcrossRebase)
{
    // Let the box settle on the floor (box centre rests near y = 1.0).
    TickFor(2.0f);
    const f32 restY = m_Box.GetComponent<TransformComponent>().Translation.y;
    EXPECT_NEAR(restY, 1.0f, 0.2f) << "box did not settle on the floor before the rebase";

    // Rebase far horizontally. Floor and box both shift; the box must stay on
    // the (also-shifted) floor.
    GetScene().RebaseOrigin(glm::vec3{ 3072.0f, 0.0f, -2048.0f });

    // Simulate more; the box must not fall through a floor that stayed behind.
    TickFor(1.0f);
    const auto& p = m_Box.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(p.y, restY, 0.2f)
        << "box fell (or jumped) after the rebase — the static floor body did not shift with it";
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
}
