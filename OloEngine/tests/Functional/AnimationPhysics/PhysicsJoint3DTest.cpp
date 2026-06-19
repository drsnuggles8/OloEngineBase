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
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Physics3D/PhysicsEvents.h"
#include "OloEngine/Physics3D/JoltScene.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

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

// =============================================================================
// Breakable joints (issue #308 item 2). At runtime the per-step constraint
// impulse is read back, converted to a force/torque, and compared against the
// authored m_BreakForce / m_BreakTorque thresholds; over-threshold joints are
// removed and a JointBrokeEvent is published on the Scene's GameplayEventBus.
// A non-positive threshold disables that axis.
//
// The force cases pin a 1 kg body in place against gravity, so the Point
// constraint must carry exactly m·g ≈ 9.81 N every step — a stable load that
// is trivially above/below a chosen threshold. The torque case welds a
// spinning body, so the Fixed constraint must shed a large angular impulse.
// =============================================================================

// -----------------------------------------------------------------------------
// Below-threshold: the joint must survive — constraint kept, token intact, no
// event, body held in place.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, BreakableJointSurvivesLoadBelowBreakForce)
{
    Entity bob = MakeBox("HeldBob", { 0.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.25f);
    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Point;
    // m_ConnectedEntity defaults to 0 → world anchor at the body's own position.
    joint.m_BreakForce = 50.0f; // well above the ~9.81 N the joint must carry

    EnablePhysics3D();

    std::vector<JointBrokeEvent> breaks;
    GetScene().GetGameplayEvents().Subscribe<JointBrokeEvent>([&](const JointBrokeEvent& e)
                                                              { breaks.push_back(e); });

    TickFor(2.0f);

    EXPECT_TRUE(breaks.empty()) << "joint broke under a load below its break force";
    EXPECT_NE(bob.GetComponent<PhysicsJoint3DComponent>().m_RuntimeConstraintToken, 0u)
        << "surviving joint lost its runtime constraint token";
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetConstraintCount(), 1u)
        << "surviving joint's Jolt constraint was removed";
    EXPECT_NEAR(Pos(bob).y, 3.0f, 0.1f) << "body was not held in place; y=" << Pos(bob).y;
}

// -----------------------------------------------------------------------------
// Above-threshold (force): the joint must break — constraint removed, token
// cleared, exactly one JointBrokeEvent fired (BrokeByForce), body falls free.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, BreakableJointBreaksWhenForceExceedsBreakForce)
{
    Entity bob = MakeBox("BreakBob", { 0.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.25f);
    const UUID bobID = bob.GetUUID();
    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Point; // world anchor at the body's own position
    joint.m_BreakForce = 2.0f;         // below the ~9.81 N gravity load → snaps

    EnablePhysics3D();

    std::vector<JointBrokeEvent> breaks;
    GetScene().GetGameplayEvents().Subscribe<JointBrokeEvent>([&](const JointBrokeEvent& e)
                                                              { breaks.push_back(e); });

    TickFor(2.0f);

    ASSERT_EQ(breaks.size(), 1u) << "expected exactly one JointBrokeEvent";
    EXPECT_EQ(static_cast<u64>(breaks[0].EntityID), static_cast<u64>(bobID));
    EXPECT_TRUE(breaks[0].BrokeByForce) << "break should be attributed to the force threshold";
    EXPECT_FALSE(breaks[0].BrokeByTorque);
    EXPECT_GT(breaks[0].Force, joint.m_BreakForce) << "reported force should exceed the threshold";
    EXPECT_EQ(bob.GetComponent<PhysicsJoint3DComponent>().m_RuntimeConstraintToken, 0u)
        << "broken joint did not clear its runtime constraint token";
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetConstraintCount(), 0u)
        << "broken joint's Jolt constraint was not removed";
    // Freed body falls (~19 m in 2 s); it must drop well below its pinned height.
    EXPECT_LT(Pos(bob).y, 2.0f) << "body did not fall after the joint broke; y=" << Pos(bob).y;
}

// -----------------------------------------------------------------------------
// Above-threshold (torque): a Fixed joint welds a spinning body, so the
// rotation constraint must shed a large angular impulse. With break force
// disabled (0), only the torque threshold can trip — proving the torque path.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, BreakableJointBreaksWhenTorqueExceedsBreakTorque)
{
    Entity anchor = MakeBox("TorqueAnchor", { 0.0f, 3.0f, 0.0f }, BodyType3D::Static, 0.3f);
    Entity welded = MakeBox("TorqueWelded", { 1.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.3f);
    const UUID weldedID = welded.GetUUID();
    welded.GetComponent<Rigidbody3DComponent>().m_InitialAngularVelocity = { 0.0f, 0.0f, 15.0f };

    auto& joint = welded.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Fixed;
    joint.m_ConnectedEntity = anchor.GetUUID();
    joint.m_BreakForce = 0.0f;  // disabled — only torque may break this joint
    joint.m_BreakTorque = 5.0f; // the spin-arresting torque dwarfs this

    EnablePhysics3D();

    std::vector<JointBrokeEvent> breaks;
    GetScene().GetGameplayEvents().Subscribe<JointBrokeEvent>([&](const JointBrokeEvent& e)
                                                              { breaks.push_back(e); });

    TickFor(1.0f);

    ASSERT_EQ(breaks.size(), 1u) << "expected exactly one JointBrokeEvent";
    EXPECT_EQ(static_cast<u64>(breaks[0].EntityID), static_cast<u64>(weldedID));
    EXPECT_TRUE(breaks[0].BrokeByTorque) << "break should be attributed to the torque threshold";
    EXPECT_FALSE(breaks[0].BrokeByForce) << "break force was disabled, so it must not be the cause";
    EXPECT_GT(breaks[0].Torque, joint.m_BreakTorque);
    EXPECT_EQ(welded.GetComponent<PhysicsJoint3DComponent>().m_RuntimeConstraintToken, 0u);
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetConstraintCount(), 0u);
    // Once free, the welded body falls away from its held height.
    EXPECT_LT(Pos(welded).y, 2.5f) << "body did not fall after the weld broke; y=" << Pos(welded).y;
}

// -----------------------------------------------------------------------------
// Backward compatibility: with both thresholds at the 0 default, a joint is
// unbreakable no matter how it is loaded — existing scenes keep their joints.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, UnbreakableByDefaultIgnoresLoad)
{
    Entity bob = MakeBox("UnbreakableBob", { 0.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.25f);
    bob.AddComponent<PhysicsJoint3DComponent>().m_Type = JointType3D::Point;
    // m_BreakForce / m_BreakTorque left at their 0 defaults → unbreakable.

    EnablePhysics3D();

    std::vector<JointBrokeEvent> breaks;
    GetScene().GetGameplayEvents().Subscribe<JointBrokeEvent>([&](const JointBrokeEvent& e)
                                                              { breaks.push_back(e); });

    TickFor(2.0f);

    EXPECT_TRUE(breaks.empty()) << "a default (0/0) joint must never break";
    EXPECT_NE(bob.GetComponent<PhysicsJoint3DComponent>().m_RuntimeConstraintToken, 0u);
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetConstraintCount(), 1u);
    EXPECT_NEAR(Pos(bob).y, 3.0f, 0.1f);
}

// =============================================================================
// Powered joints — motors + friction (issue #308 item 3). The Hinge / Slider
// arms of JoltScene::CreateConstraint read the authored motor fields and put the
// Jolt constraint into the requested motor state at creation time. Each test
// stands the joint up so that the motor / friction it configures produces motion
// (or resists it) that a free joint would not — a wide-margin, sign-robust
// signal that the constraint was actually powered.
// =============================================================================

// -----------------------------------------------------------------------------
// Hinge velocity motor — drives the hinge to spin continuously, lifting the door
// up and over the top of its arc against gravity. A free / gravity-only hinge
// never rises above its horizontal start, so reaching the top is conclusive.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, HingeVelocityMotorDrivesRotationAgainstGravity)
{
    const glm::vec3 pivot{ 0.0f, 3.0f, 0.0f };
    Entity door = MakeBox("MotorDoor", { 1.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);

    auto& joint = door.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Hinge;
    // m_ConnectedEntity defaults to 0 → anchored to the world.
    joint.m_LocalAnchorA = { -1.0f, 0.0f, 0.0f }; // pivot at (0,3,0)
    joint.m_Axis = { 0.0f, 0.0f, 1.0f };          // horizontal hinge → gravity opposes the lift
    joint.m_HingeMinAngleDeg = -180.0f;           // full range → no limit, free to spin
    joint.m_HingeMaxAngleDeg = 180.0f;
    joint.m_HingeMotorMode = JointMotorMode::Velocity;
    joint.m_HingeMotorTargetVelocityDeg = 180.0f; // half a turn per second
    joint.m_HingeMaxMotorTorque = 100.0f;         // dwarfs the ~9.81 N·m gravity peak

    EnablePhysics3D();

    f32 maxY = Pos(door).y;
    f32 maxDistErr = 0.0f;
    for (int i = 0; i < 180; ++i) // 3 s at 60 Hz
    {
        RunFrames(1);
        const glm::vec3 p = Pos(door);
        maxY = std::max(maxY, p.y);
        maxDistErr = std::max(maxDistErr, std::abs(glm::distance(p, pivot) - 1.0f));
    }

    EXPECT_LT(maxDistErr, 0.1f) << "motorised hinge did not keep the arm pinned to the pivot";
    // Top of the arc is y = pivotY + armLength = 4.0; a gravity-only hinge never
    // exceeds its 3.0 start. Reaching 3.7 proves the motor drove it over the top.
    EXPECT_GT(maxY, 3.7f) << "velocity motor did not drive the hinge up over its arc; maxY=" << maxY;
    EXPECT_TRUE(std::isfinite(Pos(door).y));
}

// -----------------------------------------------------------------------------
// Hinge position motor — settles the hinge at a target angle. The hinge axis is
// vertical so gravity exerts no torque about it; the body rotates in the
// horizontal plane and the position motor parks it ~90° from its +X start.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, HingePositionMotorSettlesAtTargetAngle)
{
    const glm::vec3 pivot{ 0.0f, 3.0f, 0.0f };
    Entity door = MakeBox("PosMotorDoor", { 1.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    door.GetComponent<Rigidbody3DComponent>().m_AngularDrag = 0.5f; // damp residual spin

    auto& joint = door.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Hinge;
    joint.m_LocalAnchorA = { -1.0f, 0.0f, 0.0f }; // pivot at (0,3,0)
    joint.m_Axis = { 0.0f, 1.0f, 0.0f };          // vertical hinge → gravity makes no torque about it
    joint.m_HingeMinAngleDeg = -180.0f;           // full range → SetTargetAngle is not clamped
    joint.m_HingeMaxAngleDeg = 180.0f;
    joint.m_HingeMotorMode = JointMotorMode::Position;
    joint.m_HingeMotorTargetAngleDeg = 90.0f; // quarter turn in the horizontal plane
    joint.m_HingeMaxMotorTorque = 100.0f;

    EnablePhysics3D();

    TickFor(4.0f); // critically-damped spring → settles well within 4 s

    const glm::vec3 p = Pos(door);
    // A 90° rotation about the vertical axis moves the arm from +X (1,3,0) into
    // the XZ plane: |x-pivot| → ~0 and |z| → ~1. Bounds allow ±20° of settle
    // error and are agnostic to Jolt's rotation sign.
    EXPECT_NEAR(glm::distance(p, pivot), 1.0f, 0.1f) << "position-motor hinge lost its pivot distance";
    EXPECT_NEAR(p.y, 3.0f, 0.2f) << "vertical-axis hinge should hold the arm at pivot height; y=" << p.y;
    EXPECT_LT(std::abs(p.x), 0.4f) << "hinge did not rotate away from its +X start; x=" << p.x;
    EXPECT_GT(std::abs(p.z), 0.7f) << "hinge did not reach ~90° toward the target angle; z=" << p.z;
}

// -----------------------------------------------------------------------------
// Slider velocity motor — drives the body up its vertical rail against gravity.
// A gravity-only slider sinks to its lower limit, so rising past the start is a
// clear sign the motor is powering it.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SliderVelocityMotorDrivesBodyAlongAxis)
{
    Entity box = MakeBox("MotorSlide", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);

    auto& joint = box.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Slider;
    // m_ConnectedEntity defaults to 0 → anchored to the world.
    joint.m_Axis = { 0.0f, 1.0f, 0.0f }; // vertical rail
    joint.m_SliderMinLimit = -3.0f;
    joint.m_SliderMaxLimit = 3.0f;
    joint.m_SliderMotorMode = JointMotorMode::Velocity;
    joint.m_SliderMotorTargetVelocity = 2.0f; // +2 m/s along +axis (up)
    joint.m_SliderMaxMotorForce = 100.0f;     // dwarfs the ~9.81 N gravity load

    EnablePhysics3D();
    const f32 startY = Pos(box).y;

    f32 maxLateral = 0.0f;
    for (int i = 0; i < 180; ++i) // 3 s
    {
        RunFrames(1);
        const glm::vec3 p = Pos(box);
        maxLateral = std::max(maxLateral, std::max(std::abs(p.x), std::abs(p.z)));
    }

    const glm::vec3 p = Pos(box);
    EXPECT_LT(maxLateral, 0.05f) << "slider motor let the body move off its axis; maxLateral=" << maxLateral;
    // Drives up to the +3 limit (y ≈ 8); gravity alone would sink it to y ≈ 2.
    EXPECT_GT(p.y, startY + 1.5f) << "velocity motor did not drive the body up its rail; y=" << p.y;
    EXPECT_NEAR(p.y, startY + 3.0f, 0.3f) << "body did not reach the upper slider limit; y=" << p.y;
}

// -----------------------------------------------------------------------------
// Slider position motor — parks the body at a target offset. A horizontal rail
// removes gravity from the slide axis (the constraint carries the weight), so
// the motor settles cleanly at the target distance from the start.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SliderPositionMotorSettlesAtTargetPosition)
{
    Entity box = MakeBox("PosMotorSlide", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    box.GetComponent<Rigidbody3DComponent>().m_LinearDrag = 0.5f; // damp residual slide

    auto& joint = box.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Slider;
    joint.m_Axis = { 1.0f, 0.0f, 0.0f }; // horizontal rail → gravity is perpendicular
    joint.m_SliderMinLimit = -3.0f;
    joint.m_SliderMaxLimit = 3.0f;
    joint.m_SliderMotorMode = JointMotorMode::Position;
    joint.m_SliderMotorTargetPosition = 2.0f; // 2 m from the start along the axis
    joint.m_SliderMaxMotorForce = 100.0f;

    EnablePhysics3D();

    TickFor(4.0f);

    const glm::vec3 p = Pos(box);
    // Target is 2 m along the slide axis; |offset| ≈ 2 regardless of axis sign.
    EXPECT_NEAR(std::abs(p.x), 2.0f, 0.3f) << "position motor did not park the body at the 2 m target; x=" << p.x;
    EXPECT_NEAR(p.y, 5.0f, 0.1f) << "slider let the body fall off its horizontal rail; y=" << p.y;
    EXPECT_LT(std::abs(p.z), 0.05f) << "body drifted off the slide axis; z=" << p.z;
}

// -----------------------------------------------------------------------------
// Hinge friction (no motor) — a large friction torque holds the door against the
// gravity that would otherwise swing it down. With the motor Off, friction is
// the only thing that can keep it horizontal.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, HingeFrictionResistsSwingWithoutMotor)
{
    Entity door = MakeBox("FrictionDoor", { 1.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);

    auto& joint = door.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Hinge;
    joint.m_LocalAnchorA = { -1.0f, 0.0f, 0.0f }; // pivot at (0,3,0)
    joint.m_Axis = { 0.0f, 0.0f, 1.0f };          // horizontal hinge → gravity swings it
    joint.m_HingeMinAngleDeg = -180.0f;           // full range → no angle limit doing the holding
    joint.m_HingeMaxAngleDeg = 180.0f;
    joint.m_HingeMotorMode = JointMotorMode::Off; // friction only
    joint.m_HingeMaxFrictionTorque = 100.0f;      // >> the ~9.81 N·m gravity peak → static hold

    EnablePhysics3D();
    const f32 startY = Pos(door).y;

    TickFor(2.0f);

    // A frictionless hinge swings to y ≈ 2.0; friction must keep it near the top.
    EXPECT_NEAR(Pos(door).y, startY, 0.2f) << "friction torque did not resist the swing; y=" << Pos(door).y;
}

// -----------------------------------------------------------------------------
// Slider friction (no motor) — a large friction force holds the body on its
// vertical rail instead of letting gravity slide it down to the limit.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SliderFrictionResistsSlideWithoutMotor)
{
    Entity box = MakeBox("FrictionSlide", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);

    auto& joint = box.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Slider;
    joint.m_Axis = { 0.0f, 1.0f, 0.0f }; // vertical rail → gravity slides it down
    joint.m_SliderMinLimit = -3.0f;
    joint.m_SliderMaxLimit = 3.0f;
    joint.m_SliderMotorMode = JointMotorMode::Off; // friction only
    joint.m_SliderMaxFrictionForce = 100.0f;       // >> the ~9.81 N gravity load → static hold

    EnablePhysics3D();
    const f32 startY = Pos(box).y;

    TickFor(2.0f);

    // A frictionless slider sinks to y ≈ 2.0; friction must keep it near the start.
    EXPECT_NEAR(Pos(box).y, startY, 0.2f) << "friction force did not resist the slide; y=" << Pos(box).y;
}

// =============================================================================
// Springy (soft) limits — issue #308 item 3, SpringSettings. A limit-spring
// frequency > 0 turns the hinge/slider limits into springs: the body may
// overshoot the limit and a restoring force at that frequency pulls it back.
// Jolt's frequency mode is mass-normalized, so the static sag under gravity is
// g / ω² (ω = 2π·frequency) — at 0.5 Hz that is ~1 m, a wide, predictable
// signal that cleanly separates "soft" (sags past the stop) from "hard"
// (parks at the stop) and from "no limit at all" (falls/swings to the arc
// bottom).
// =============================================================================

// -----------------------------------------------------------------------------
// Slider soft limit — gravity drives the body down its vertical rail and ~1 m
// past the lower limit, where the 0.5 Hz critically-damped spring holds it. A
// hard-limit slider parks at the limit (y = 2.0); a slider with no working
// limit free-falls. Settling near y = 1.0 proves the spring is both soft and
// load-bearing.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SliderSoftLimitSpringSagsPastHardStop)
{
    Entity box = MakeBox("SoftSlide", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);

    auto& joint = box.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Slider;
    // m_ConnectedEntity defaults to 0 → anchored to the world.
    joint.m_Axis = { 0.0f, 1.0f, 0.0f }; // vertical rail → gravity loads the lower limit
    joint.m_SliderMinLimit = -3.0f;      // lower stop at y = 2.0
    joint.m_SliderMaxLimit = 3.0f;
    joint.m_SliderLimitSpringFrequency = 0.5f; // sag = g/ω² ≈ 1.0 m
    joint.m_SliderLimitSpringDamping = 1.0f;   // critical — settles without bouncing

    EnablePhysics3D();

    f32 maxLateral = 0.0f;
    for (int i = 0; i < 240; ++i) // 4 s at 60 Hz
    {
        RunFrames(1);
        const glm::vec3 p = Pos(box);
        maxLateral = std::max(maxLateral, std::max(std::abs(p.x), std::abs(p.z)));
    }

    const f32 y = Pos(box).y;
    EXPECT_LT(maxLateral, 0.05f) << "soft-limit slider let the body leave its rail; maxLateral=" << maxLateral;
    EXPECT_LT(y, 1.7f) << "body parked at the hard stop — the limit spring did not soften it; y=" << y;
    EXPECT_GT(y, 0.4f) << "body fell through the soft limit — the spring is not holding the load; y=" << y;
}

// -----------------------------------------------------------------------------
// Hinge soft limit — gravity swings the door down to its -30° stop and the
// 0.5 Hz spring lets it sag ~20° further (equilibrium ≈ -49°, y ≈ 1.5 for a
// 2 m arm). A hard-limit hinge parks at -30° (y = 2.0); with no working limit
// it swings to the arc bottom (y = 1.0). Settling between those proves the
// spring both yields and holds.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, HingeSoftLimitSpringSagsPastHardStop)
{
    const glm::vec3 pivot{ 0.0f, 3.0f, 0.0f };
    Entity door = MakeBox("SoftDoor", { 2.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    // The spring only damps while the limit is exceeded; inside the limits the
    // hinge is free, so without drag the door oscillates indefinitely instead
    // of settling onto the spring (same reason the position-motor test damps).
    door.GetComponent<Rigidbody3DComponent>().m_AngularDrag = 0.5f;

    auto& joint = door.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Hinge;
    joint.m_LocalAnchorA = { -2.0f, 0.0f, 0.0f }; // pivot at (0,3,0), 2 m arm
    joint.m_Axis = { 0.0f, 0.0f, 1.0f };          // horizontal hinge → gravity swings it down
    joint.m_HingeMinAngleDeg = -30.0f;            // stop at -30° → y = 3 - 2·sin(30°) = 2.0
    joint.m_HingeMaxAngleDeg = 30.0f;
    joint.m_HingeLimitSpringFrequency = 0.5f;
    joint.m_HingeLimitSpringDamping = 1.0f;

    EnablePhysics3D();

    TickFor(6.0f); // settles where the limit spring balances gravity (~-50°)

    const glm::vec3 p = Pos(door);
    EXPECT_NEAR(glm::distance(p, pivot), 2.0f, 0.1f) << "soft-limit hinge lost its pivot distance";
    EXPECT_LT(p.y, 1.85f) << "door parked at the hard stop — the limit spring did not soften it; y=" << p.y;
    EXPECT_GT(p.y, 1.05f) << "door swung to the arc bottom — the limit spring is not restoring; y=" << p.y;
}

// =============================================================================
// SwingTwist (issue #308 item 4). A ragdoll-friendly ball joint: the swing is
// confined to a cone (two half-angles) and the twist about the axis is limited
// to an authored range. Both are exercised below with position / rotation
// assertions that a broken constraint fails by a wide margin.
// =============================================================================

// -----------------------------------------------------------------------------
// Swing — like the Cone joint, the bob hangs from a pivot and is pushed
// sideways; the swing cone must clip the swing (a free ball joint swings past).
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SwingTwistConfinesSwingWithinCone)
{
    const glm::vec3 pivot{ 0.0f, 3.0f, 0.0f };
    Entity bob = MakeBox("SwingBob", { 0.0f, 2.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    bob.GetComponent<Rigidbody3DComponent>().m_InitialLinearVelocity = { 2.5f, 0.0f, 0.0f };

    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::SwingTwist;
    // m_ConnectedEntity defaults to 0 → anchored to the world.
    joint.m_LocalAnchorA = { 0.0f, 1.0f, 0.0f }; // worldA = (0,2,0)+(0,1,0) = pivot
    joint.m_Axis = { 0.0f, 1.0f, 0.0f };         // twist axis points up toward the pivot
    joint.m_SwingNormalHalfAngleDeg = 20.0f;     // ~symmetric 20° swing cone
    joint.m_SwingPlaneHalfAngleDeg = 20.0f;
    joint.m_TwistMinAngleDeg = -5.0f;
    joint.m_TwistMaxAngleDeg = 5.0f;

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

    EXPECT_LT(maxDistErr, 0.15f) << "swing-twist joint did not keep the bob pinned to the pivot";
    // 20° swing cone caps the horizontal offset at sin(20°)·1 ≈ 0.34. A free ball
    // joint would swing past 0.7; the cone must clip it well under 0.55.
    EXPECT_LT(maxHoriz, 0.55f) << "bob escaped the swing cone; maxHoriz=" << maxHoriz;
    EXPECT_GT(maxHoriz, 0.25f) << "bob never swung out — initial velocity wasn't applied";
}

// -----------------------------------------------------------------------------
// Twist — with swing locked (zero swing half-angles), the body may only twist
// about the axis. Spun up about that axis with gravity disabled, the twist
// limit must stop it; a free spin would pass 180°.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SwingTwistLimitsTwistAboutAxis)
{
    Entity bob = MakeBox("TwistBob", { 0.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    auto& rb = bob.GetComponent<Rigidbody3DComponent>();
    rb.m_DisableGravity = true;                         // isolate twist from swing
    rb.m_InitialAngularVelocity = { 0.0f, 2.0f, 0.0f }; // spin about the twist axis

    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::SwingTwist;
    // m_ConnectedEntity defaults to 0 → anchored to the world at the COM.
    joint.m_Axis = { 0.0f, 1.0f, 0.0f };    // twist axis = world up
    joint.m_SwingNormalHalfAngleDeg = 0.0f; // lock swing — only twist is free
    joint.m_SwingPlaneHalfAngleDeg = 0.0f;
    joint.m_TwistMinAngleDeg = -25.0f;
    joint.m_TwistMaxAngleDeg = 25.0f;

    EnablePhysics3D();

    f32 maxTwistDeg = 0.0f;
    for (int i = 0; i < 240; ++i) // 4 s
    {
        RunFrames(1);
        // Swing is locked, so the body's orientation is ~a pure rotation about
        // the (vertical) twist axis: q ≈ (cos(θ/2), 0, sin(θ/2), 0).
        const glm::quat q = bob.GetComponent<TransformComponent>().GetRotation();
        const f32 twistDeg = glm::degrees(2.0f * std::atan2(std::abs(q.y), std::abs(q.w)));
        maxTwistDeg = std::max(maxTwistDeg, twistDeg);
    }

    EXPECT_GT(maxTwistDeg, 10.0f) << "body never twisted — initial angular velocity wasn't applied";
    // ±25° limit; allow generous hard-limit overshoot but exclude a free spin.
    EXPECT_LT(maxTwistDeg, 45.0f) << "twist blew past the ±25° limit (free spin); maxTwistDeg=" << maxTwistDeg;
}

// -----------------------------------------------------------------------------
// Inverted twist range — an authored Min > Max (reachable from the editor's two
// independent angle fields) must not reach Jolt as-is: SwingTwistConstraintPart
// asserts on min > max. JoltScene normalises it by swapping, so the constraint
// builds and behaves as the equivalent [-Max, -Min]-style valid span instead of
// crashing. We assert only that it builds, simulates finitely, and still limits
// the twist (i.e. it didn't silently become a free spin).
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SwingTwistNormalisesInvertedTwistRange)
{
    Entity bob = MakeBox("InvTwistBob", { 0.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    auto& rb = bob.GetComponent<Rigidbody3DComponent>();
    rb.m_DisableGravity = true;
    rb.m_InitialAngularVelocity = { 0.0f, 2.0f, 0.0f };

    auto& joint = bob.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::SwingTwist;
    joint.m_Axis = { 0.0f, 1.0f, 0.0f };
    joint.m_SwingNormalHalfAngleDeg = 0.0f; // lock swing — only twist is free
    joint.m_SwingPlaneHalfAngleDeg = 0.0f;
    joint.m_TwistMinAngleDeg = 25.0f; // inverted: Min > Max
    joint.m_TwistMaxAngleDeg = -25.0f;

    EnablePhysics3D();

    f32 maxTwistDeg = 0.0f;
    for (int i = 0; i < 240; ++i) // 4 s
    {
        RunFrames(1);
        const glm::quat q = bob.GetComponent<TransformComponent>().GetRotation();
        const f32 twistDeg = glm::degrees(2.0f * std::atan2(std::abs(q.y), std::abs(q.w)));
        maxTwistDeg = std::max(maxTwistDeg, twistDeg);
        EXPECT_TRUE(std::isfinite(twistDeg)) << "inverted twist range produced a non-finite rotation";
    }

    // Normalised to a ±25° span — the twist must still be limited, not free.
    EXPECT_LT(maxTwistDeg, 45.0f) << "inverted range was not normalised; twist ran free; maxTwistDeg=" << maxTwistDeg;
}

// =============================================================================
// SixDOF (issue #308 item 4). Each of the 3 translation + 3 rotation DOF is
// independently Locked / Limited / Free. The frame's X axis is m_Axis; the
// other two axes are derived perpendicular. Default: every axis Locked.
// =============================================================================

// -----------------------------------------------------------------------------
// All axes Locked (the default) → a rigid weld; a falling body must be held.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SixDOFAllLockedHoldsBodyLikeWeld)
{
    Entity box = MakeBox("SixDOFWeld", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.3f);
    auto& joint = box.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::SixDOF;
    // Every translation + rotation mode defaults to Locked → rigid weld.

    EnablePhysics3D();

    const glm::vec3 start = Pos(box);
    f32 maxDrift = 0.0f;
    for (int i = 0; i < 240; ++i) // 4 s
    {
        RunFrames(1);
        maxDrift = std::max(maxDrift, glm::distance(Pos(box), start));
    }

    EXPECT_LT(maxDrift, 0.1f) << "all-Locked SixDOF let the body move; maxDrift=" << maxDrift;
    EXPECT_TRUE(std::isfinite(Pos(box).y));
}

// -----------------------------------------------------------------------------
// One translation axis Free (the rest Locked) → the body slides freely along
// that axis but is held on the other two. The free axis is the frame X axis
// (= m_Axis), here world-X (horizontal); a locked perpendicular axis is vertical
// and must hold the body up against gravity. Driving the free axis horizontally
// (gentle initial velocity) instead of free-falling keeps the solver load low
// and steady — like SixDOFAllLockedHoldsBodyLikeWeld — so the result is robust
// across CPUs (a high-velocity vertical free-fall is cross-CPU marginal in Jolt).
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SixDOFFreeTranslationAxisAllowsMotion)
{
    Entity box = MakeBox("SixDOFSlide", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    box.GetComponent<Rigidbody3DComponent>().m_InitialLinearVelocity = { 2.0f, 0.0f, 0.0f };

    auto& joint = box.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::SixDOF;
    joint.m_Axis = { 1.0f, 0.0f, 0.0f };            // frame X axis = world X (horizontal)
    joint.m_SixDOFTransXMode = JointAxisMode::Free; // free to slide along world X
    // Y/Z translation + all rotation stay Locked (defaults) → hold against gravity.

    EnablePhysics3D();

    f32 maxOffAxis = 0.0f;
    f32 minY = Pos(box).y;
    for (int i = 0; i < 60; ++i) // 1 s
    {
        RunFrames(1);
        const glm::vec3 p = Pos(box);
        maxOffAxis = std::max(maxOffAxis, std::abs(p.z));
        minY = std::min(minY, p.y);
    }

    const glm::vec3 p = Pos(box);
    // Slid ~2 m along the free axis (an all-Locked weld would hold x≈0).
    EXPECT_GT(p.x, 1.0f) << "free axis did not let the body slide along it; x=" << p.x;
    // Held up by the locked vertical axis (a free body falls ~5 m in 1 s to y≈0).
    EXPECT_GT(minY, 4.0f) << "a locked axis let the body fall under gravity; minY=" << minY;
    EXPECT_LT(maxOffAxis, 0.3f) << "body drifted off the constraint plane; maxOffAxis=" << maxOffAxis;
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
}

// -----------------------------------------------------------------------------
// One translation axis Limited (the rest Locked) → the body slides along that
// axis and stops at the bound, like a slider. Driven horizontally (world X) so
// the locked vertical axis holds it against gravity — a steady, low load that is
// robust across CPUs.
// -----------------------------------------------------------------------------
TEST_F(PhysicsJoint3DTest, SixDOFLimitedTranslationStopsAtBound)
{
    Entity box = MakeBox("SixDOFLimited", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    auto& rb = box.GetComponent<Rigidbody3DComponent>();
    rb.m_InitialLinearVelocity = { 1.5f, 0.0f, 0.0f };
    rb.m_LinearDrag = 0.2f; // settle at the bound

    auto& joint = box.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::SixDOF;
    joint.m_Axis = { 1.0f, 0.0f, 0.0f }; // frame X axis = world X (horizontal)
    joint.m_SixDOFTransXMode = JointAxisMode::Limited;
    // ±1 m along the free axis; the perpendicular axes get a tight range but stay
    // Locked anyway.
    joint.m_SixDOFTranslationMin = { -1.0f, -0.1f, -0.1f };
    joint.m_SixDOFTranslationMax = { 1.0f, 0.1f, 0.1f };
    // Y/Z translation + all rotation stay Locked (defaults) → hold against gravity.

    EnablePhysics3D();

    f32 maxX = 0.0f;
    f32 maxOffAxis = 0.0f;
    f32 minY = Pos(box).y;
    for (int i = 0; i < 180; ++i) // 3 s
    {
        RunFrames(1);
        const glm::vec3 p = Pos(box);
        maxX = std::max(maxX, p.x);
        maxOffAxis = std::max(maxOffAxis, std::abs(p.z));
        minY = std::min(minY, p.y);
    }

    const glm::vec3 p = Pos(box);
    // Slid toward the +1 m bound and stopped there (a free body coasts past ~2 m).
    EXPECT_GT(p.x, 0.5f) << "body never slid toward the +1 m bound; x=" << p.x;
    EXPECT_LT(maxX, 1.5f) << "limited axis did not stop the body at the +1 m bound; maxX=" << maxX;
    EXPECT_GT(minY, 4.0f) << "a locked axis let the body fall under gravity; minY=" << minY;
    EXPECT_LT(maxOffAxis, 0.3f) << "body drifted off the constraint plane; maxOffAxis=" << maxOffAxis;
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
}

// =============================================================================
// Pulley (issue #308 item 4). Two bodies hang from two fixed world-space points;
// the rope keeps |A-FixedA| + Ratio*|B-FixedB| within [Min, Max]. A heavier body
// descends and, through the taut rope, LIFTS the lighter body — which a free body
// would never do (it would fall). The segment-length sum is conserved.
// =============================================================================
TEST_F(PhysicsJoint3DTest, PulleyLiftsLighterBodyAsHeavierBodyDescends)
{
    const glm::vec3 fixedA{ 0.0f, 8.0f, 0.0f }; // pulley point above the heavy body
    const glm::vec3 fixedB{ 3.0f, 8.0f, 0.0f }; // pulley point above the light body
    Entity heavy = MakeBox("PulleyHeavy", { 0.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    Entity light = MakeBox("PulleyLight", { 3.0f, 5.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    heavy.GetComponent<Rigidbody3DComponent>().m_Mass = 2.0f; // imbalance → heavy hauls light up

    auto& joint = heavy.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Pulley;
    joint.m_ConnectedEntity = light.GetUUID();
    // Anchors default to each body centre; fixed points are authored in world space.
    joint.m_PulleyFixedPointA = fixedA; // this body (heavy)
    joint.m_PulleyFixedPointB = fixedB; // connected body (light)
    joint.m_PulleyRatio = 1.0f;
    joint.m_PulleyMinLength = 0.0f;
    joint.m_PulleyMaxLength = -1.0f; // auto = current total (3 + 3 = 6) → contract-only rope

    EnablePhysics3D();
    const glm::vec3 startHeavy = Pos(heavy);
    const glm::vec3 startLight = Pos(light);
    const f32 initialSum = glm::distance(startHeavy, fixedA) + glm::distance(startLight, fixedB);

    f32 maxSum = initialSum;
    f32 maxLateral = 0.0f;
    for (int i = 0; i < 60; ++i) // 1 s — well before the light body reaches its pulley point
    {
        RunFrames(1);
        const f32 sum = glm::distance(Pos(heavy), fixedA) + glm::distance(Pos(light), fixedB);
        maxSum = std::max(maxSum, sum);
        maxLateral = std::max(maxLateral, std::max(std::abs(Pos(heavy).x - startHeavy.x), std::abs(Pos(light).x - startLight.x)));
    }

    const glm::vec3 endHeavy = Pos(heavy);
    const glm::vec3 endLight = Pos(light);
    // Heavy side descended; light side ROSE (a free body would fall instead).
    EXPECT_LT(endHeavy.y, startHeavy.y - 0.3f) << "heavy body did not descend; y=" << endHeavy.y;
    EXPECT_GT(endLight.y, startLight.y + 0.3f) << "pulley did not lift the light body; y=" << endLight.y;
    // Rope max-length held: the segment sum never grew past its starting value.
    EXPECT_LT(maxSum, initialSum + 0.2f) << "rope stretched past its max length; maxSum=" << maxSum;
    // Conservation (ratio 1): how far the heavy fell ≈ how far the light rose.
    EXPECT_NEAR(startHeavy.y - endHeavy.y, endLight.y - startLight.y, 0.35f)
        << "rope did not conserve length (rise != fall)";
    EXPECT_LT(maxLateral, 0.3f) << "bodies swung off their vertical drop; maxLateral=" << maxLateral;
    EXPECT_TRUE(std::isfinite(endHeavy.y) && std::isfinite(endLight.y));
}

// =============================================================================
// Gear (issue #308 item 4), body-to-body v1 form. Couples two bodies' rotation
// about their axes by a ratio: connectedRotation = -ratio * thisRotation. With
// gravity off and only an initial spin on gear A, the gear must drive gear B in
// the OPPOSITE direction at the authored ratio — a free gear B never moves.
// =============================================================================
TEST_F(PhysicsJoint3DTest, GearCouplesRotationByRatio)
{
    Entity gearA = MakeBox("GearA", { 0.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    Entity gearB = MakeBox("GearB", { 2.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    auto& rbA = gearA.GetComponent<Rigidbody3DComponent>();
    rbA.m_DisableGravity = true;                         // isolate the gear coupling
    rbA.m_InitialAngularVelocity = { 0.0f, 0.0f, 3.0f }; // spin gear A about Z
    gearB.GetComponent<Rigidbody3DComponent>().m_DisableGravity = true;

    auto& joint = gearA.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::Gear;
    joint.m_ConnectedEntity = gearB.GetUUID();
    joint.m_Axis = { 0.0f, 0.0f, 1.0f };          // gear A spins about Z
    joint.m_ConnectedAxis = { 0.0f, 0.0f, 1.0f }; // gear B spins about Z
    joint.m_GearRatio = 2.0f;                     // B spins twice as fast, opposite sense

    EnablePhysics3D();

    // Signed rotation about Z from a (near-pure-Z) orientation quaternion.
    const auto zAngle = [](Entity e)
    {
        const glm::quat q = e.GetComponent<TransformComponent>().GetRotation();
        return 2.0f * std::atan2(q.z, q.w);
    };

    f32 angleA = 0.0f;
    f32 angleB = 0.0f;
    for (int i = 0; i < 30; ++i) // 0.5 s — both stay well within ±pi (no wrap)
    {
        RunFrames(1);
        angleA = zAngle(gearA);
        angleB = zAngle(gearB);
    }

    // A free gear B never rotates; the gear must spin it the opposite way.
    EXPECT_GT(angleA, 0.1f) << "gear A did not keep spinning; angleA=" << angleA;
    EXPECT_LT(angleB, -0.1f) << "gear did not counter-rotate gear B; angleB=" << angleB;
    // Ratio 2: the velocity constraint holds every step, so |angleB| ≈ 2·|angleA|.
    EXPECT_NEAR(std::abs(angleB) / std::abs(angleA), 2.0f, 0.4f)
        << "gear ratio not honoured; angleA=" << angleA << " angleB=" << angleB;
}

// =============================================================================
// RackAndPinion (issue #308 item 4), body-to-body v1 form. The CONNECTED body is
// the pinion (rotates about m_ConnectedAxis); THIS body is the rack (slides along
// m_Axis): pinionRotation = ratio * rackTranslation. Pushing the rack along its
// axis must spin the pinion — a free pinion never moves — by ratio·displacement.
// =============================================================================
TEST_F(PhysicsJoint3DTest, RackAndPinionCouplesRackSlideToPinionSpin)
{
    Entity pinion = MakeBox("Pinion", { 0.0f, 3.0f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    Entity rack = MakeBox("Rack", { 0.0f, 1.5f, 0.0f }, BodyType3D::Dynamic, 0.2f);
    pinion.GetComponent<Rigidbody3DComponent>().m_DisableGravity = true;
    auto& rbRack = rack.GetComponent<Rigidbody3DComponent>();
    rbRack.m_DisableGravity = true;
    rbRack.m_InitialLinearVelocity = { 1.0f, 0.0f, 0.0f }; // push the rack along +X

    auto& joint = rack.AddComponent<PhysicsJoint3DComponent>();
    joint.m_Type = JointType3D::RackAndPinion;
    joint.m_ConnectedEntity = pinion.GetUUID();   // the connected body is the pinion
    joint.m_Axis = { 1.0f, 0.0f, 0.0f };          // rack slides along X
    joint.m_ConnectedAxis = { 0.0f, 0.0f, 1.0f }; // pinion spins about Z
    joint.m_GearRatio = 2.0f;                     // 2 rad of pinion per metre of rack

    EnablePhysics3D();
    const f32 startRackX = Pos(rack).x;

    const auto zAngle = [](Entity e)
    {
        const glm::quat q = e.GetComponent<TransformComponent>().GetRotation();
        return 2.0f * std::atan2(q.z, q.w);
    };

    f32 rackDx = 0.0f;
    f32 pinionAngle = 0.0f;
    for (int i = 0; i < 40; ++i) // ~0.67 s — pinion stays under ±pi
    {
        RunFrames(1);
        rackDx = Pos(rack).x - startRackX;
        pinionAngle = zAngle(pinion);
    }

    // A free pinion never rotates; the rack's slide must spin it.
    EXPECT_GT(rackDx, 0.2f) << "rack did not slide along +X; dx=" << rackDx;
    EXPECT_GT(std::abs(pinionAngle), 0.2f) << "rack&pinion did not rotate the pinion; angle=" << pinionAngle;
    // Position constraint: |pinionAngle| ≈ ratio · rackDisplacement.
    EXPECT_NEAR(std::abs(pinionAngle), 2.0f * rackDx, 0.3f)
        << "rack&pinion ratio not honoured; dx=" << rackDx << " angle=" << pinionAngle;
    EXPECT_TRUE(std::isfinite(rackDx) && std::isfinite(pinionAngle));
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
    j.m_BreakForce = 320.0f;
    j.m_BreakTorque = 64.0f;
    j.m_HingeMotorMode = JointMotorMode::Velocity;
    j.m_HingeMotorTargetVelocityDeg = 120.0f;
    j.m_HingeMotorTargetAngleDeg = -30.0f;
    j.m_HingeMaxMotorTorque = 45.0f;
    j.m_HingeMaxFrictionTorque = 12.0f;
    j.m_SliderMotorMode = JointMotorMode::Position;
    j.m_SliderMotorTargetVelocity = -1.5f;
    j.m_SliderMotorTargetPosition = 2.25f;
    j.m_SliderMaxMotorForce = 88.0f;
    j.m_SliderMaxFrictionForce = 7.5f;
    j.m_HingeLimitSpringFrequency = 2.5f;
    j.m_HingeLimitSpringDamping = 0.8f;
    j.m_SliderLimitSpringFrequency = 4.0f;
    j.m_SliderLimitSpringDamping = 1.2f;
    j.m_SwingNormalHalfAngleDeg = 33.0f;
    j.m_SwingPlaneHalfAngleDeg = 18.0f;
    j.m_TwistMinAngleDeg = -22.0f;
    j.m_TwistMaxAngleDeg = 14.0f;
    j.m_SixDOFTransXMode = JointAxisMode::Free;
    j.m_SixDOFTransYMode = JointAxisMode::Limited;
    j.m_SixDOFTransZMode = JointAxisMode::Locked;
    j.m_SixDOFRotXMode = JointAxisMode::Limited;
    j.m_SixDOFRotYMode = JointAxisMode::Free;
    j.m_SixDOFRotZMode = JointAxisMode::Locked;
    j.m_SixDOFTranslationMin = { -1.0f, -0.25f, -2.0f };
    j.m_SixDOFTranslationMax = { 1.5f, 0.75f, 2.5f };
    j.m_SixDOFRotationMinDeg = { -60.0f, -30.0f, -15.0f };
    j.m_SixDOFRotationMaxDeg = { 60.0f, 90.0f, 45.0f };
    // Pulley + Gear/RackAndPinion fields (issue #308 item 4).
    j.m_PulleyFixedPointA = { 1.25f, 6.5f, -0.75f };
    j.m_PulleyFixedPointB = { -2.0f, 7.0f, 1.5f };
    j.m_PulleyRatio = 3.0f;
    j.m_PulleyMinLength = 0.5f;
    j.m_PulleyMaxLength = 9.0f;
    j.m_ConnectedAxis = { 0.0f, 0.0f, 1.0f };
    j.m_GearRatio = -2.5f; // signed: a valid reversed coupling

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
    EXPECT_NEAR(rj.m_BreakForce, 320.0f, kEps);
    EXPECT_NEAR(rj.m_BreakTorque, 64.0f, kEps);
    EXPECT_EQ(rj.m_HingeMotorMode, JointMotorMode::Velocity);
    EXPECT_NEAR(rj.m_HingeMotorTargetVelocityDeg, 120.0f, kEps);
    EXPECT_NEAR(rj.m_HingeMotorTargetAngleDeg, -30.0f, kEps);
    EXPECT_NEAR(rj.m_HingeMaxMotorTorque, 45.0f, kEps);
    EXPECT_NEAR(rj.m_HingeMaxFrictionTorque, 12.0f, kEps);
    EXPECT_EQ(rj.m_SliderMotorMode, JointMotorMode::Position);
    EXPECT_NEAR(rj.m_SliderMotorTargetVelocity, -1.5f, kEps);
    EXPECT_NEAR(rj.m_SliderMotorTargetPosition, 2.25f, kEps);
    EXPECT_NEAR(rj.m_SliderMaxMotorForce, 88.0f, kEps);
    EXPECT_NEAR(rj.m_SliderMaxFrictionForce, 7.5f, kEps);
    EXPECT_NEAR(rj.m_HingeLimitSpringFrequency, 2.5f, kEps);
    EXPECT_NEAR(rj.m_HingeLimitSpringDamping, 0.8f, kEps);
    EXPECT_NEAR(rj.m_SliderLimitSpringFrequency, 4.0f, kEps);
    EXPECT_NEAR(rj.m_SliderLimitSpringDamping, 1.2f, kEps);
    // SwingTwist + SixDOF fields (issue #308 item 4).
    EXPECT_NEAR(rj.m_SwingNormalHalfAngleDeg, 33.0f, kEps);
    EXPECT_NEAR(rj.m_SwingPlaneHalfAngleDeg, 18.0f, kEps);
    EXPECT_NEAR(rj.m_TwistMinAngleDeg, -22.0f, kEps);
    EXPECT_NEAR(rj.m_TwistMaxAngleDeg, 14.0f, kEps);
    EXPECT_EQ(rj.m_SixDOFTransXMode, JointAxisMode::Free);
    EXPECT_EQ(rj.m_SixDOFTransYMode, JointAxisMode::Limited);
    EXPECT_EQ(rj.m_SixDOFTransZMode, JointAxisMode::Locked);
    EXPECT_EQ(rj.m_SixDOFRotXMode, JointAxisMode::Limited);
    EXPECT_EQ(rj.m_SixDOFRotYMode, JointAxisMode::Free);
    EXPECT_EQ(rj.m_SixDOFRotZMode, JointAxisMode::Locked);
    EXPECT_NEAR(rj.m_SixDOFTranslationMin.x, -1.0f, kEps);
    EXPECT_NEAR(rj.m_SixDOFTranslationMin.y, -0.25f, kEps);
    EXPECT_NEAR(rj.m_SixDOFTranslationMin.z, -2.0f, kEps);
    EXPECT_NEAR(rj.m_SixDOFTranslationMax.x, 1.5f, kEps);
    EXPECT_NEAR(rj.m_SixDOFTranslationMax.y, 0.75f, kEps);
    EXPECT_NEAR(rj.m_SixDOFTranslationMax.z, 2.5f, kEps);
    EXPECT_NEAR(rj.m_SixDOFRotationMinDeg.x, -60.0f, kEps);
    EXPECT_NEAR(rj.m_SixDOFRotationMinDeg.y, -30.0f, kEps);
    EXPECT_NEAR(rj.m_SixDOFRotationMinDeg.z, -15.0f, kEps);
    EXPECT_NEAR(rj.m_SixDOFRotationMaxDeg.x, 60.0f, kEps);
    EXPECT_NEAR(rj.m_SixDOFRotationMaxDeg.y, 90.0f, kEps);
    EXPECT_NEAR(rj.m_SixDOFRotationMaxDeg.z, 45.0f, kEps);
    // Pulley + Gear/RackAndPinion fields (issue #308 item 4).
    EXPECT_NEAR(rj.m_PulleyFixedPointA.x, 1.25f, kEps);
    EXPECT_NEAR(rj.m_PulleyFixedPointA.y, 6.5f, kEps);
    EXPECT_NEAR(rj.m_PulleyFixedPointA.z, -0.75f, kEps);
    EXPECT_NEAR(rj.m_PulleyFixedPointB.x, -2.0f, kEps);
    EXPECT_NEAR(rj.m_PulleyFixedPointB.y, 7.0f, kEps);
    EXPECT_NEAR(rj.m_PulleyFixedPointB.z, 1.5f, kEps);
    EXPECT_NEAR(rj.m_PulleyRatio, 3.0f, kEps);
    EXPECT_NEAR(rj.m_PulleyMinLength, 0.5f, kEps);
    EXPECT_NEAR(rj.m_PulleyMaxLength, 9.0f, kEps);
    EXPECT_NEAR(rj.m_ConnectedAxis.z, 1.0f, kEps);
    EXPECT_NEAR(rj.m_GearRatio, -2.5f, kEps);
}
