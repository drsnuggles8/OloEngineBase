#include "OloEnginePCH.h"

// =============================================================================
// PhysicsJoint3DTest — Functional Test.
//
// Cross-subsystem seam under test:
//   PhysicsJoint3DComponent (authored ECS data) × JoltScene two-body
//   constraint creation × Physics3D simulation, all driven through a real
//   Scene::OnUpdateRuntime. The joint constraints are built in the
//   second pass of Scene::OnPhysics3DStart (after every rigidbody exists)
//   and torn down in OnPhysics3DStop.
//
// Each test stands up a minimal scene for one JointType3D and asserts the
// constrained motion that joint type is supposed to produce — using
// positional tolerances, never float `==` (see CLAUDE.md / docs/testing.md).
// Where a free body would fall away / swing freely, the joint must visibly
// hold / limit it, so a broken constraint is detectable by a wide margin.
//
// A final test round-trips the component through the save-game serializer.
//
// Functional-test contract: see docs/testing.md §7, ADR 0001/0002/0003.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class PhysicsJoint3DTest : public FunctionalTest
{
  protected:
    // Each test constructs its own entities in the test body and then calls
    // EnablePhysics3D() — the joint scenarios differ too much to share one
    // BuildScene, and EnablePhysics3D() snapshots whatever exists at call time.
    void BuildScene() override {}

    Entity MakeBox(const std::string& name, const glm::vec3& pos, BodyType3D type, f32 halfExtent = 0.5f)
    {
        Entity e = GetScene().CreateEntity(name);
        e.GetComponent<TransformComponent>().Translation = pos;
        auto& rb = e.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = type;
        rb.m_Mass = 1.0f;
        rb.m_LinearDrag = 0.0f;
        rb.m_AngularDrag = 0.0f;
        auto& col = e.AddComponent<BoxCollider3DComponent>();
        col.m_HalfExtents = glm::vec3(halfExtent);
        return e;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }
};

// -----------------------------------------------------------------------------
// Fixed — welds a dynamic body rigidly to a static anchor; it must not fall.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, FixedJointWeldsDynamicBodyToStaticAnchor)
{
    Entity anchor = MakeBox("Anchor", { 0.0f, 3.0f, 0.0f }, BodyType3D::Static, 0.4f);
    Entity welded = MakeBox("Welded", { 1.5f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.4f);

    auto& joint = welded.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Fixed;
    joint.m_ConnectedEntity = anchor.GetUUID();

    EnablePhysics3D();
    const glm::vec3 startPos = Pos(welded);

    TickFor(2.0f);

    const glm::vec3 endPos = Pos(welded);
    // A free dynamic body falls ~19 m in 2 s; the weld must hold it in place.
    EXPECT_NEAR(endPos.y, startPos.y, 0.1f) << "welded body fell — Fixed joint didn't hold it; y=" << endPos.y;
    EXPECT_NEAR(glm::distance(endPos, Pos(anchor)), 1.5f, 0.1f) << "weld did not preserve the relative pose";
    EXPECT_TRUE(std::isfinite(endPos.x) && std::isfinite(endPos.y) && std::isfinite(endPos.z));
}

// -----------------------------------------------------------------------------
// Point — ball-socket pendulum about a world-fixed pivot; arm length is held.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, PointJointActsAsBallSocketPendulum)
{
    const glm::vec3 pivot{ 0.0f, 3.0f, 0.0f };
    Entity bob = MakeBox("Bob", { 1.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);

    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Point;
    // m_ConnectedEntity defaults to 0 → anchored to the world.
    joint.m_LocalAnchorA = { -1.0f, 0.0f, 0.0f }; // worldA = (1,3,0)+(-1,0,0) = pivot

    EnablePhysics3D();

    f32 minY = Pos(bob).y;
    f32 maxDistErr = 0.0f;
    for (int i = 0; i < 300; ++i) // 5 s at 60 Hz
    {
        RunFrames(1);
        const glm::vec3 p = Pos(bob);
        minY = std::min(minY, p.y);
        maxDistErr = std::max(maxDistErr, std::abs(glm::distance(p, pivot) - 1.0f));
    }

    EXPECT_LT(maxDistErr, 0.1f) << "Point joint did not keep the bob at a fixed distance from the pivot";
    EXPECT_LE(minY, 2.2f) << "bob did not swing down past the pivot's height — Point constraint not driving the pendulum";
    EXPECT_TRUE(std::isfinite(Pos(bob).y));
}

// -----------------------------------------------------------------------------
// Distance — a slack rope that lets a body free-fall, then catches it at the
// maximum length.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, DistanceJointCatchesFallingBodyAtMaxLength)
{
    Entity anchor = MakeBox("Anchor", { 0.0f, 5.0f, 0.0f }, BodyType3D::Static, 0.25f);
    Entity bob = MakeBox("Bob", { 0.0f, 4.0f, 0.0f }, BodyType3D::Dynamic, 0.25f);

    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Distance;
    joint.m_ConnectedEntity = anchor.GetUUID();
    joint.m_MinDistance = 0.0f;
    joint.m_MaxDistance = 2.0f; // initial separation is 1.0, so the rope starts slack

    EnablePhysics3D();

    TickFor(4.0f);

    const glm::vec3 p = Pos(bob);
    const f32 dist = glm::distance(p, Pos(anchor));
    EXPECT_LT(p.y, 3.9f) << "bob never fell — the rope behaved as rigid";
    EXPECT_LE(dist, 2.0f + 0.1f) << "bob exceeded the rope's max length; dist=" << dist;
    EXPECT_GT(dist, 1.5f) << "rope never went taut";
    EXPECT_NEAR(p.y, 3.0f, 0.25f) << "bob did not hang at anchorY - maxDistance; y=" << p.y;
}

// -----------------------------------------------------------------------------
// Hinge — a door swinging about a horizontal axis, stopped by an angle limit.
// The ±30° limit caps the swing well short of the straight-down position a
// free ball joint would reach (y = 2.0), proving the angle limit is enforced.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, HingeJointSwingsAboutAxisAndRespectsAngleLimit)
{
    const glm::vec3 pivot{ 0.0f, 3.0f, 0.0f };
    Entity door = MakeBox("Door", { 1.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    door.GetComponent<Rigidbody3DComponent>().m_AngularDrag = 0.2f; // damp the swing toward the limit

    auto& joint = door.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Hinge;
    // m_ConnectedEntity defaults to 0 → anchored to the world.
    joint.m_LocalAnchorA = { -1.0f, 0.0f, 0.0f }; // pivot at (0,3,0)
    joint.m_Axis = { 0.0f, 0.0f, 1.0f };          // horizontal hinge axis → gravity swings it
    // Symmetric ±30° range so the swing is capped at 30° from horizontal
    // regardless of Jolt's hinge-angle sign convention.
    joint.m_HingeMinAngleDeg = -30.0f;
    joint.m_HingeMaxAngleDeg = 30.0f;

    EnablePhysics3D();

    f32 minY = Pos(door).y;
    f32 maxDistErr = 0.0f;
    f32 maxZ = 0.0f;
    for (int i = 0; i < 300; ++i) // 5 s
    {
        RunFrames(1);
        const glm::vec3 p = Pos(door);
        minY = std::min(minY, p.y);
        maxDistErr = std::max(maxDistErr, std::abs(glm::distance(p, pivot) - 1.0f));
        maxZ = std::max(maxZ, std::abs(p.z));
    }

    EXPECT_LT(maxDistErr, 0.1f) << "hinge did not keep the arm pinned to the pivot";
    EXPECT_LT(maxZ, 0.15f) << "door drifted off the hinge plane — rotation not confined to the hinge axis";
    // 30° limit: lowest point is y = 3 - sin(30°) = 2.5. A free pendulum (Point
    // joint) would reach y = 2.0; the limit must stop the door short of that.
    EXPECT_GT(minY, 2.3f) << "hinge angle limit did not stop the swing (fell past 30°); minY=" << minY;
    EXPECT_LT(minY, 2.75f) << "door did not swing down to its 30° limit; minY=" << minY;
}

// -----------------------------------------------------------------------------
// Slider — a prismatic joint: the body may only translate along one axis and
// stops at the limit. Lateral motion must stay ~0.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SliderJointConstrainsMotionToAxisAndLimit)
{
    Entity box = MakeBox("Sliding", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    box.GetComponent<Rigidbody3DComponent>().m_LinearDrag = 0.1f; // settle at the limit

    auto& joint = box.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Slider;
    // m_ConnectedEntity defaults to 0 → anchored to the world.
    joint.m_Axis = { 0.0f, 1.0f, 0.0f }; // vertical rail
    // Symmetric ±3 range so the body slides down 3 units regardless of the
    // slider's position sign convention; it cannot slide up (gravity only).
    joint.m_SliderMinLimit = -3.0f;
    joint.m_SliderMaxLimit = 3.0f;

    EnablePhysics3D();

    f32 maxLateral = 0.0f;
    for (int i = 0; i < 240; ++i) // 4 s
    {
        RunFrames(1);
        const glm::vec3 p = Pos(box);
        maxLateral = std::max(maxLateral, std::max(std::abs(p.x), std::abs(p.z)));
    }

    const glm::vec3 p = Pos(box);
    EXPECT_LT(maxLateral, 0.05f) << "slider let the body move off its axis; maxLateral=" << maxLateral;
    EXPECT_NEAR(p.y, 2.0f, 0.25f) << "body did not stop at the slider's lower limit (5 - 3); y=" << p.y;
    EXPECT_TRUE(std::isfinite(p.y));
}

// -----------------------------------------------------------------------------
// Cone — a ball-socket whose swing is confined to a cone half-angle. Pushed
// sideways, the body must stay within the cone (a free ball joint would swing
// much further out).
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, ConeJointConfinesSwingWithinHalfAngle)
{
    const glm::vec3 pivot{ 0.0f, 3.0f, 0.0f };
    Entity bob = MakeBox("ConeBob", { 0.0f, 2.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    bob.GetComponent<Rigidbody3DComponent>().m_InitialLinearVelocity = { 2.5f, 0.0f, 0.0f };

    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Cone;
    // m_ConnectedEntity defaults to 0 → anchored to the world.
    joint.m_LocalAnchorA = { 0.0f, 1.0f, 0.0f }; // worldA = (0,2,0)+(0,1,0) = pivot
    joint.m_Axis = { 0.0f, 1.0f, 0.0f };         // twist axis points up toward the pivot
    joint.m_ConeHalfAngleDeg = 20.0f;

    EnablePhysics3D();

    f32 maxHoriz = 0.0f;
    f32 maxDistErr = 0.0f;
    for (int i = 0; i < 240; ++i) // 4 s
    {
        RunFrames(1);
        const glm::vec3 p = Pos(bob);
        maxHoriz = std::max(maxHoriz, std::sqrt(p.x * p.x + p.z * p.z));
        maxDistErr = std::max(maxDistErr, std::abs(glm::distance(p, pivot) - 1.0f));
    }

    EXPECT_LT(maxDistErr, 0.15f) << "cone joint did not keep the bob pinned to the pivot";
    // 20° cone: horizontal offset is capped at sin(20°)·1 ≈ 0.34. With this
    // initial energy a free ball joint would swing past 0.7; the cone must clip
    // it. The 0.55 bound leaves generous room for hard-limit overshoot while
    // still excluding the free-swing case.
    EXPECT_LT(maxHoriz, 0.55f) << "bob escaped the cone half-angle limit; maxHoriz=" << maxHoriz;
    EXPECT_GT(maxHoriz, 0.25f) << "bob never swung out — initial velocity wasn't applied";
}

// -----------------------------------------------------------------------------
// Save-game round-trip — the authored joint data must survive
// CaptureSceneState → RestoreSceneState (exercises SaveGameComponentSerializer).
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, ComponentSurvivesSaveGameRoundTrip)
{
    constexpr f32 kEps = 1e-4f;

    Entity e = GetScene().CreateEntity("JointSaveGame");
    auto& j = e.AddComponent<PhysicsJoint3DComponent>();
    j.m_Type = JointType3D::Hinge;
    j.m_ConnectedEntity = UUID(0xABCDEF12ULL);
    j.m_LocalAnchorA = { 0.5f, 1.5f, -2.5f };
    j.m_LocalAnchorB = { -1.0f, 0.25f, 3.0f };
    j.m_Axis = { 1.0f, 0.0f, 0.0f };
    j.m_MinDistance = 0.5f;
    j.m_MaxDistance = 3.0f;
    j.m_HingeMinAngleDeg = -90.0f;
    j.m_HingeMaxAngleDeg = 45.0f;
    j.m_SliderMinLimit = -1.5f;
    j.m_SliderMaxLimit = 2.0f;
    j.m_ConeHalfAngleDeg = 75.0f;

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("JointSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<PhysicsJoint3DComponent>())
        << "PhysicsJoint3DComponent dropped by the save-game round-trip";

    const auto& rj = re.GetComponent<PhysicsJoint3DComponent>();
    EXPECT_EQ(rj.m_Type, JointType3D::Hinge);
    EXPECT_EQ(static_cast<u64>(rj.m_ConnectedEntity), 0xABCDEF12ULL);
    EXPECT_NEAR(rj.m_LocalAnchorA.x, 0.5f, kEps);
    EXPECT_NEAR(rj.m_LocalAnchorA.z, -2.5f, kEps);
    EXPECT_NEAR(rj.m_LocalAnchorB.y, 0.25f, kEps);
    EXPECT_NEAR(rj.m_Axis.x, 1.0f, kEps);
    EXPECT_NEAR(rj.m_MinDistance, 0.5f, kEps);
    EXPECT_NEAR(rj.m_MaxDistance, 3.0f, kEps);
    EXPECT_NEAR(rj.m_HingeMinAngleDeg, -90.0f, kEps);
    EXPECT_NEAR(rj.m_HingeMaxAngleDeg, 45.0f, kEps);
    EXPECT_NEAR(rj.m_SliderMinLimit, -1.5f, kEps);
    EXPECT_NEAR(rj.m_SliderMaxLimit, 2.0f, kEps);
    EXPECT_NEAR(rj.m_ConeHalfAngleDeg, 75.0f, kEps);
}
