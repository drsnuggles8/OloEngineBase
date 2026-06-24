#include "OloEnginePCH.h"

// =============================================================================
// VehicleTest — Functional Test.
//
// OLO_TEST_LAYER: Functional
//
// Cross-subsystem seam under test:
//   VehicleComponent (authored ECS data) × JoltScene's Jolt VehicleConstraint +
//   WheeledVehicleController × Physics3D simulation, all driven through a real
//   Scene::OnUpdateRuntime. The vehicle is built in the pass of
//   Scene::OnPhysics3DStart that runs after every rigidbody exists (the chassis
//   IS the entity's Rigidbody3DComponent), registered as a Jolt step listener so
//   its suspension / traction run each physics tick, and torn down in
//   OnPhysics3DStop. Driver input (throttle / steer / brake) is read off the
//   component each step.
//
// MVP slice of issue #308 item 5 (Vehicles). The chassis is a standard
// four-wheel car (two steerable front wheels + two driven rear wheels). Each
// test stands up a minimal scene and asserts the behaviour a working vehicle
// produces by a wide margin — never float `==` (see CLAUDE.md / docs/testing.md):
//   * the suspension holds the chassis ABOVE where a bare falling box would rest
//     (the discriminator: a broken / absent constraint just drops the box);
//   * full throttle drives the chassis forward along its local +Z (Jolt's
//     vehicle forward) by metres a parked car never travels.
//
// A final test round-trips the component through the save-game serializer.
//
// Functional-test contract: see docs/testing.md §7, ADR 0001/0002/0003.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Physics3D/JoltScene.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class VehicleTest : public FunctionalTest
{
  protected:
    // Each test constructs its own entities in the body and then calls
    // EnablePhysics3D() (which snapshots whatever exists at call time), so
    // BuildScene stays empty — the scenarios differ too much to share one.
    void BuildScene() override {}

    // A large static floor whose TOP face sits at y = 0, so wheel-ray contacts
    // and resting heights are measured against a known ground plane.
    Entity MakeGround()
    {
        Entity e = GetScene().CreateEntity("Ground");
        e.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        auto& rb = e.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Static;
        auto& col = e.AddComponent<BoxCollider3DComponent>();
        col.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        return e;
    }

    // A four-wheel car chassis with default VehicleComponent geometry. The box
    // half-height (0.4) matches the default wheel attachment height (-0.4), so a
    // working vehicle rests with the box bottom ~0.75 m above the ground (held up
    // by the suspension) while a bare box would sink until its bottom touches the
    // ground (centre at y = 0.4). That gap is the test's discriminator.
    Entity MakeCar(const glm::vec3& pos)
    {
        Entity e = GetScene().CreateEntity("Car");
        e.GetComponent<TransformComponent>().Translation = pos;
        auto& rb = e.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Dynamic;
        rb.m_Mass = 150.0f;
        auto& col = e.AddComponent<BoxCollider3DComponent>();
        col.m_HalfExtents = { 0.9f, 0.4f, 1.8f };
        e.AddComponent<VehicleComponent>(); // all-default car
        return e;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }
};

// -----------------------------------------------------------------------------
// Creation — after physics starts, the chassis owns a live Jolt vehicle: the
// component's runtime token is set and the scene reports exactly one vehicle.
// -----------------------------------------------------------------------------
TEST_F(VehicleTest, VehicleConstraintIsCreatedAtPhysicsStart)
{
    MakeGround();
    Entity car = MakeCar({ 0.0f, 1.3f, 0.0f });

    EnablePhysics3D();

    EXPECT_NE(car.GetComponent<VehicleComponent>().m_RuntimeVehicleToken, 0u)
        << "vehicle did not get a runtime token at physics start";
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetVehicleCount(), 1u)
        << "JoltScene did not build exactly one VehicleConstraint";
}

// -----------------------------------------------------------------------------
// Suspension bears the load — with no throttle the car settles ON its wheels,
// held well above where the bare chassis box would rest (centre y = 0.4). A
// broken / absent vehicle constraint fails this by a wide margin (the box just
// drops to ~0.4). The car must also stay finite and not drift without input.
// -----------------------------------------------------------------------------
TEST_F(VehicleTest, VehicleSettlesOnSuspensionAboveGround)
{
    MakeGround();
    Entity car = MakeCar({ 0.0f, 1.3f, 0.0f });

    EnablePhysics3D();

    TickFor(2.5f); // suspension is damped → settles well within 2.5 s

    const glm::vec3 p = Pos(car);
    EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    // Discriminator: a bare box rests at y = 0.4; the suspension must hold the
    // chassis higher than that. (Resting height is ~1.1 with the default car.)
    EXPECT_GT(p.y, 0.7f) << "chassis sank to the ground — suspension is not bearing the load; y=" << p.y;
    EXPECT_LT(p.y, 1.6f) << "chassis rose above its spawn — suspension is pushing it off; y=" << p.y;
    // No driver input → the car should sit still, not creep across the floor.
    EXPECT_LT(std::abs(p.x), 0.5f) << "parked car drifted in X; x=" << p.x;
    EXPECT_LT(std::abs(p.z), 0.5f) << "parked car drifted in Z; z=" << p.z;
}

// -----------------------------------------------------------------------------
// Throttle drives it forward — full throttle accelerates the chassis along its
// local +Z (Jolt's vehicle forward) by metres. A parked / broken car never
// travels that far, and a sign error would send it backward, so a clear forward
// displacement that dominates any lateral drift is conclusive.
// -----------------------------------------------------------------------------
TEST_F(VehicleTest, VehicleDrivesForwardUnderThrottle)
{
    MakeGround();
    Entity car = MakeCar({ 0.0f, 1.3f, 0.0f });

    EnablePhysics3D();

    // Let it settle on its wheels first, then floor the throttle.
    TickFor(1.0f);
    const glm::vec3 startPos = Pos(car);

    car.GetComponent<VehicleComponent>().m_ThrottleInput = 1.0f;
    TickFor(3.0f);

    const glm::vec3 endPos = Pos(car);
    const f32 forward = endPos.z - startPos.z;
    EXPECT_TRUE(std::isfinite(endPos.x) && std::isfinite(endPos.y) && std::isfinite(endPos.z));
    EXPECT_GT(forward, 1.0f) << "throttle did not drive the car forward (+Z); dz=" << forward;
    EXPECT_GT(forward, std::abs(endPos.x - startPos.x)) << "car drifted sideways more than it drove forward";
    EXPECT_GT(endPos.y, 0.7f) << "car left its wheels / sank while driving; y=" << endPos.y;
}

// -----------------------------------------------------------------------------
// Removing the component at runtime tears the Jolt vehicle down (and unregisters
// its step listener) — the scene's vehicle count drops back to zero and the
// freed chassis box falls to the ground a bare box rests on.
// -----------------------------------------------------------------------------
TEST_F(VehicleTest, RemovingVehicleComponentDestroysTheConstraint)
{
    MakeGround();
    Entity car = MakeCar({ 0.0f, 1.3f, 0.0f });

    EnablePhysics3D();
    TickFor(1.0f);
    ASSERT_NE(GetScene().GetPhysicsScene(), nullptr);
    ASSERT_EQ(GetScene().GetPhysicsScene()->GetVehicleCount(), 1u);

    car.RemoveComponent<VehicleComponent>();
    EXPECT_EQ(GetScene().GetPhysicsScene()->GetVehicleCount(), 0u)
        << "vehicle constraint was not released when the component was removed";

    // With the suspension gone the chassis box drops onto the ground (centre
    // settles toward the box half-height, 0.4) instead of riding on its wheels.
    TickFor(2.0f);
    EXPECT_LT(Pos(car).y, 0.7f) << "chassis kept riding on phantom wheels after the vehicle was removed; y=" << Pos(car).y;
    EXPECT_TRUE(std::isfinite(Pos(car).y));
}

// =============================================================================
// Save-game round-trip — every authored VehicleComponent field must survive
// capture + restore (mirrors the PhysicsJoint3DComponent round-trip test). No
// physics here: this exercises the serializer's symmetry, not runtime behaviour.
// =============================================================================
TEST_F(VehicleTest, VehicleComponentSurvivesSaveGameRoundTrip)
{
    constexpr f32 kEps = 1e-4f;

    Entity e = GetScene().CreateEntity("VehicleSaveGame");
    auto& v = e.AddComponent<VehicleComponent>();
    // Recognisable non-default values on every authored field.
    v.m_HalfTrackWidth = 1.1f;
    v.m_FrontAxleOffset = 1.4f;
    v.m_RearAxleOffset = 1.6f;
    v.m_WheelAttachmentHeight = -0.55f;
    v.m_WheelRadius = 0.42f;
    v.m_WheelWidth = 0.3f;
    v.m_SuspensionMinLength = 0.25f;
    v.m_SuspensionMaxLength = 0.6f;
    v.m_SuspensionFrequency = 2.0f;
    v.m_SuspensionDamping = 0.7f;
    v.m_MaxEngineTorque = 650.0f;
    v.m_MaxSteerAngleDeg = 35.0f;
    v.m_MaxBrakeTorque = 1800.0f;
    v.m_ThrottleInput = 0.5f;
    v.m_SteerInput = -0.25f;
    v.m_BrakeInput = 0.1f;

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("VehicleSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<VehicleComponent>())
        << "VehicleComponent dropped by the save-game round-trip";

    const auto& rv = re.GetComponent<VehicleComponent>();
    EXPECT_NEAR(rv.m_HalfTrackWidth, 1.1f, kEps);
    EXPECT_NEAR(rv.m_FrontAxleOffset, 1.4f, kEps);
    EXPECT_NEAR(rv.m_RearAxleOffset, 1.6f, kEps);
    EXPECT_NEAR(rv.m_WheelAttachmentHeight, -0.55f, kEps);
    EXPECT_NEAR(rv.m_WheelRadius, 0.42f, kEps);
    EXPECT_NEAR(rv.m_WheelWidth, 0.3f, kEps);
    EXPECT_NEAR(rv.m_SuspensionMinLength, 0.25f, kEps);
    EXPECT_NEAR(rv.m_SuspensionMaxLength, 0.6f, kEps);
    EXPECT_NEAR(rv.m_SuspensionFrequency, 2.0f, kEps);
    EXPECT_NEAR(rv.m_SuspensionDamping, 0.7f, kEps);
    EXPECT_NEAR(rv.m_MaxEngineTorque, 650.0f, kEps);
    EXPECT_NEAR(rv.m_MaxSteerAngleDeg, 35.0f, kEps);
    EXPECT_NEAR(rv.m_MaxBrakeTorque, 1800.0f, kEps);
    EXPECT_NEAR(rv.m_ThrottleInput, 0.5f, kEps);
    EXPECT_NEAR(rv.m_SteerInput, -0.25f, kEps);
    EXPECT_NEAR(rv.m_BrakeInput, 0.1f, kEps);
    // The runtime token is never serialized — it must come back cleared.
    EXPECT_EQ(rv.m_RuntimeVehicleToken, 0u);
}
