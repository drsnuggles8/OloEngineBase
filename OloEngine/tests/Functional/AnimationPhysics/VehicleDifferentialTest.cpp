#include "OloEnginePCH.h"

// =============================================================================
// VehicleDifferentialTest — Functional Test.
//
// OLO_TEST_LAYER: Functional
//
// Cross-subsystem seam under test:
//   VehicleComponent's authored drive mode + differential config (issue #438) ×
//   JoltScene::CreateVehicle's WheeledVehicleController construction × the Jolt
//   simulation, driven through a real Scene::OnUpdateRuntime.
//
// Before #438 the drivetrain was a single hard-coded rear differential
// (mLeftWheel = 2, mRightWheel = 3, full engine torque). It is now DERIVED from
// VehicleComponent::m_DriveMode plus five tunables. Two very different failure
// modes follow, so this file tests on two levels:
//
//   1. CONTRACT — read the differential list back off the live controller
//      (JoltScene::GetVehicleDrivetrain) and assert exactly which wheel indices
//      receive torque and in what ratio. A behavioural test can only report
//      "the car moved"; only this can catch a drive mode that silently drove
//      the WRONG axle, or a front/rear torque split applied backwards.
//
//   2. BEHAVIOUR — each mode must actually accelerate the chassis by metres.
//      A configuration can be structurally plausible and still produce a car
//      that doesn't move (an empty differential list, a wheel index Jolt
//      rejects), which the contract assertions alone would not notice.
//
// The last acceptance criterion of #438 is "the existing jeep is unaffected",
// so the default-construction case is asserted explicitly here as well —
// VehicleTest.cpp is deliberately left untouched as the independent witness.
//
// Functional-test contract: see docs/testing.md §7, ADR 0001/0002/0003.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>
#include <cmath>
#include <limits>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Wheel indices of the standard layout JoltScene::CreateVehicle builds.
    constexpr i32 kFL = 0;
    constexpr i32 kFR = 1;
    constexpr i32 kRL = 2;
    constexpr i32 kRR = 3;
} // namespace

class VehicleDifferentialTest : public FunctionalTest
{
  protected:
    void BuildScene() override {}

    // Same ground + chassis geometry as VehicleTest, so a reader can compare
    // the drive-mode numbers here against that file's RWD baseline directly.
    void MakeGround()
    {
        Entity e = GetScene().CreateEntity("Ground");
        e.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
        e.AddComponent<Rigidbody3DComponent>().m_Type = BodyType3D::Static;
        e.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 50.0f, 0.5f, 50.0f };
    }

    Entity MakeCar(VehicleDriveMode mode)
    {
        Entity e = GetScene().CreateEntity("Car");
        e.GetComponent<TransformComponent>().Translation = { 0.0f, 1.3f, 0.0f };
        auto& rb = e.AddComponent<Rigidbody3DComponent>();
        rb.m_Type = BodyType3D::Dynamic;
        rb.m_Mass = 150.0f;
        e.AddComponent<BoxCollider3DComponent>().m_HalfExtents = { 0.9f, 0.4f, 1.8f };
        e.AddComponent<VehicleComponent>().m_DriveMode = mode;
        return e;
    }

    static glm::vec3 Pos(Entity e)
    {
        return e.GetComponent<TransformComponent>().Translation;
    }

    JoltScene::VehicleDrivetrainInfo Drivetrain(Entity car)
    {
        auto info = GetScene().GetPhysicsScene()->GetVehicleDrivetrain(car.GetUUID());
        EXPECT_TRUE(info.has_value()) << "no live vehicle drivetrain for the car entity";
        return info.value_or(JoltScene::VehicleDrivetrainInfo{});
    }

    /// Settle on the suspension, then floor the throttle and report how far the
    /// chassis travelled along world +Z (its local forward, unrotated here).
    f32 DriveForwardDistance(Entity car)
    {
        TickFor(1.0f);
        const glm::vec3 start = Pos(car);
        car.GetComponent<VehicleComponent>().m_ThrottleInput = 1.0f;
        TickFor(3.0f);
        return Pos(car).z - start.z;
    }
};

// -----------------------------------------------------------------------------
// The jeep is unaffected (#438's last acceptance criterion, at the data level):
// a default-constructed VehicleComponent is rear-wheel drive with Jolt's own
// differential defaults, so the drivetrain built from it is bit-for-bit the
// hard-coded one this slice replaced.
// -----------------------------------------------------------------------------
TEST_F(VehicleDifferentialTest, DefaultVehicleReproducesTheLegacyRearDifferential)
{
    MakeGround();
    Entity car = MakeCar(VehicleDriveMode::RearWheelDrive);
    // Explicitly assert the AUTHORED default too — a changed default would move
    // every existing scene's jeep to a different axle without touching a scene.
    ASSERT_EQ(VehicleComponent{}.m_DriveMode, VehicleDriveMode::RearWheelDrive);

    EnablePhysics3D();

    const auto drivetrain = Drivetrain(car);
    ASSERT_EQ(drivetrain.m_Differentials.size(), 1u)
        << "rear-wheel drive must build exactly one differential";
    const auto& diff = drivetrain.m_Differentials[0];
    EXPECT_EQ(diff.m_LeftWheel, kRL);
    EXPECT_EQ(diff.m_RightWheel, kRR);
    // The legacy values: full engine torque, Jolt's own defaults for the rest.
    EXPECT_FLOAT_EQ(diff.m_EngineTorqueRatio, 1.0f);
    EXPECT_FLOAT_EQ(diff.m_LeftRightSplit, 0.5f);
    EXPECT_FLOAT_EQ(diff.m_LimitedSlipRatio, 1.4f);
    EXPECT_FLOAT_EQ(diff.m_DifferentialRatio, 3.42f);
    EXPECT_FLOAT_EQ(drivetrain.m_CenterLimitedSlipRatio, 1.4f);
}

// -----------------------------------------------------------------------------
// Front-wheel drive moves the torque to the FRONT axle. This is the assertion a
// behavioural test cannot make: an FWD car that was still (wrongly) driving the
// rear wheels would accelerate forward exactly as convincingly.
// -----------------------------------------------------------------------------
TEST_F(VehicleDifferentialTest, FrontWheelDriveDrivesTheFrontAxle)
{
    MakeGround();
    Entity car = MakeCar(VehicleDriveMode::FrontWheelDrive);

    EnablePhysics3D();

    const auto drivetrain = Drivetrain(car);
    ASSERT_EQ(drivetrain.m_Differentials.size(), 1u)
        << "front-wheel drive must build exactly one differential";
    EXPECT_EQ(drivetrain.m_Differentials[0].m_LeftWheel, kFL);
    EXPECT_EQ(drivetrain.m_Differentials[0].m_RightWheel, kFR);
    EXPECT_FLOAT_EQ(drivetrain.m_Differentials[0].m_EngineTorqueRatio, 1.0f);
}

// -----------------------------------------------------------------------------
// All-wheel drive builds TWO differentials — front then rear — whose engine
// torque ratios split by m_FrontTorqueSplit and, per Jolt's own contract, sum
// to 1. A split applied to the wrong axle is the classic silent bug here, so
// the ratios are checked against the axle they belong to, not just summed.
// -----------------------------------------------------------------------------
TEST_F(VehicleDifferentialTest, AllWheelDriveSplitsTorqueBetweenBothAxles)
{
    MakeGround();
    Entity car = MakeCar(VehicleDriveMode::AllWheelDrive);
    auto& vc = car.GetComponent<VehicleComponent>();
    vc.m_FrontTorqueSplit = 0.3f; // deliberately asymmetric: 30% front, 70% rear
    vc.m_CenterLimitedSlipRatio = 2.5f;
    vc.m_LimitedSlipRatio = 1.8f;
    vc.m_LeftRightSplit = 0.4f;
    vc.m_DifferentialRatio = 4.0f;

    EnablePhysics3D();

    const auto drivetrain = Drivetrain(car);
    ASSERT_EQ(drivetrain.m_Differentials.size(), 2u)
        << "all-wheel drive must build one differential per axle";

    const auto& front = drivetrain.m_Differentials[0];
    const auto& rear = drivetrain.m_Differentials[1];
    EXPECT_EQ(front.m_LeftWheel, kFL);
    EXPECT_EQ(front.m_RightWheel, kFR);
    EXPECT_EQ(rear.m_LeftWheel, kRL);
    EXPECT_EQ(rear.m_RightWheel, kRR);
    EXPECT_NEAR(front.m_EngineTorqueRatio, 0.3f, 1e-5f) << "front axle did not get m_FrontTorqueSplit";
    EXPECT_NEAR(rear.m_EngineTorqueRatio, 0.7f, 1e-5f) << "rear axle did not get the remainder";
    EXPECT_NEAR(front.m_EngineTorqueRatio + rear.m_EngineTorqueRatio, 1.0f, 1e-5f)
        << "engine torque ratios must sum to 1 (Jolt's contract)";

    // The shared per-differential knobs reach BOTH differentials.
    for (const auto& d : { front, rear })
    {
        EXPECT_NEAR(d.m_LimitedSlipRatio, 1.8f, 1e-5f);
        EXPECT_NEAR(d.m_LeftRightSplit, 0.4f, 1e-5f);
        EXPECT_NEAR(d.m_DifferentialRatio, 4.0f, 1e-5f);
    }
    EXPECT_NEAR(drivetrain.m_CenterLimitedSlipRatio, 2.5f, 1e-5f);
}

// -----------------------------------------------------------------------------
// Behaviour: every drive mode must actually pull the car forward by metres. A
// structurally valid differential list can still yield a car that never moves
// (a wheel index Jolt rejects, an engine torque ratio of 0), which the contract
// assertions above cannot see. Parked cars travel ~0, so the margin is wide.
// One test per mode rather than a loop: each needs its own Scene, and gtest
// already gives us exactly that per TEST_F.
// -----------------------------------------------------------------------------
TEST_F(VehicleDifferentialTest, RearWheelDriveAcceleratesTheChassisForward)
{
    MakeGround();
    Entity car = MakeCar(VehicleDriveMode::RearWheelDrive);
    EnablePhysics3D();

    const f32 travelled = DriveForwardDistance(car);
    const glm::vec3 end = Pos(car);
    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));
    EXPECT_GT(travelled, 1.0f) << "RWD did not drive the car forward; dz=" << travelled;
    EXPECT_GT(end.y, 0.7f) << "chassis sank off its wheels while driving; y=" << end.y;
}

TEST_F(VehicleDifferentialTest, FrontWheelDriveAcceleratesTheChassisForward)
{
    MakeGround();
    Entity car = MakeCar(VehicleDriveMode::FrontWheelDrive);
    EnablePhysics3D();

    const f32 travelled = DriveForwardDistance(car);
    const glm::vec3 end = Pos(car);
    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));
    EXPECT_GT(travelled, 1.0f) << "FWD did not drive the car forward; dz=" << travelled;
    EXPECT_GT(end.y, 0.7f) << "chassis sank off its wheels while driving; y=" << end.y;
}

TEST_F(VehicleDifferentialTest, AllWheelDriveAcceleratesTheChassisForward)
{
    MakeGround();
    Entity car = MakeCar(VehicleDriveMode::AllWheelDrive);
    EnablePhysics3D();

    const f32 travelled = DriveForwardDistance(car);
    const glm::vec3 end = Pos(car);
    EXPECT_TRUE(std::isfinite(end.x) && std::isfinite(end.y) && std::isfinite(end.z));
    EXPECT_GT(travelled, 1.0f) << "AWD did not drive the car forward; dz=" << travelled;
    EXPECT_GT(end.y, 0.7f) << "chassis sank off its wheels while driving; y=" << end.y;
}

// -----------------------------------------------------------------------------
// A corrupt / scripted out-of-range drive mode must fall back to rear-wheel
// drive rather than indexing off the end of the wheel array. Reaching the
// assertions at all is half the test — an unguarded switch would have built a
// differential with garbage wheel indices and asserted inside Jolt.
// -----------------------------------------------------------------------------
TEST_F(VehicleDifferentialTest, OutOfRangeDriveModeFallsBackToRearWheelDrive)
{
    MakeGround();
    Entity car = MakeCar(VehicleDriveMode::RearWheelDrive);
    // Only reachable by a raw write (a C# / Lua script, or a hand-edited scene
    // whose value survived the serializer clamp because it was in range there).
    car.GetComponent<VehicleComponent>().m_DriveMode = static_cast<VehicleDriveMode>(99);

    EnablePhysics3D();

    const auto drivetrain = Drivetrain(car);
    ASSERT_EQ(drivetrain.m_Differentials.size(), 1u);
    EXPECT_EQ(drivetrain.m_Differentials[0].m_LeftWheel, kRL);
    EXPECT_EQ(drivetrain.m_Differentials[0].m_RightWheel, kRR);
}

// -----------------------------------------------------------------------------
// Non-finite / out-of-range differential tunables must be sanitized before they
// reach Jolt: a limited-slip ratio below 1 asserts in Debug Jolt, and a NaN
// anywhere in the drivetrain NaNs the whole solver. Scripts write these fields
// raw, so JoltScene is the last line of defence.
// -----------------------------------------------------------------------------
TEST_F(VehicleDifferentialTest, GarbageDifferentialTunablesAreSanitized)
{
    MakeGround();
    Entity car = MakeCar(VehicleDriveMode::AllWheelDrive);
    auto& vc = car.GetComponent<VehicleComponent>();
    vc.m_LimitedSlipRatio = 0.2f;                                  // below Jolt's minimum of 1
    vc.m_CenterLimitedSlipRatio = -5.0f;                           // negative
    vc.m_FrontTorqueSplit = 4.0f;                                  // outside [0, 1]
    vc.m_LeftRightSplit = std::numeric_limits<f32>::quiet_NaN();   // NaN
    vc.m_DifferentialRatio = std::numeric_limits<f32>::infinity(); // non-finite

    EnablePhysics3D();

    const auto drivetrain = Drivetrain(car);
    ASSERT_EQ(drivetrain.m_Differentials.size(), 2u);
    EXPECT_NEAR(drivetrain.m_Differentials[0].m_EngineTorqueRatio, 1.0f, 1e-5f) << "front split not clamped to 1";
    EXPECT_NEAR(drivetrain.m_Differentials[1].m_EngineTorqueRatio, 0.0f, 1e-5f) << "rear split not the clamped remainder";
    for (const auto& d : drivetrain.m_Differentials)
    {
        // STRICTLY greater than 1: Jolt's own JPH_ASSERT(mLimitedSlipRatio > 1.0f)
        // fires on exactly 1.0, so clamping to 1.0 would crash Debug builds.
        EXPECT_GT(d.m_LimitedSlipRatio, 1.0f) << "limited-slip ratio at/below Jolt's strict minimum reached the controller";
        EXPECT_TRUE(std::isfinite(d.m_LeftRightSplit));
        EXPECT_GE(d.m_LeftRightSplit, 0.0f);
        EXPECT_LE(d.m_LeftRightSplit, 1.0f);
        EXPECT_TRUE(std::isfinite(d.m_DifferentialRatio));
        EXPECT_GT(d.m_DifferentialRatio, 0.0f);
    }
    EXPECT_GT(drivetrain.m_CenterLimitedSlipRatio, 1.0f);

    // And it still simulates without NaNing out.
    TickFor(1.0f);
    EXPECT_TRUE(std::isfinite(Pos(car).y));
}

// =============================================================================
// Save-game round-trip for the new differential fields. kSaveGameFormatVersion
// was bumped 11 → 12 for these, so they are read behind a version gate; this
// pins that a save written by THIS build reads its own differential config back
// (the old-save path — v11 omitting the fields entirely — is covered by
// SaveGameVersionMigrationTest's gate mechanism).
// =============================================================================
TEST_F(VehicleDifferentialTest, DifferentialConfigSurvivesSaveGameRoundTrip)
{
    constexpr f32 kEps = 1e-4f;

    Entity e = GetScene().CreateEntity("DiffSaveGame");
    auto& v = e.AddComponent<VehicleComponent>();
    v.m_DriveMode = VehicleDriveMode::AllWheelDrive;
    v.m_FrontTorqueSplit = 0.35f;
    v.m_LeftRightSplit = 0.45f;
    v.m_LimitedSlipRatio = 2.2f;
    v.m_CenterLimitedSlipRatio = 3.1f;
    v.m_DifferentialRatio = 4.11f;

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u);

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity re = restored->FindEntityByName("DiffSaveGame");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.HasComponent<VehicleComponent>());

    const auto& rv = re.GetComponent<VehicleComponent>();
    EXPECT_EQ(rv.m_DriveMode, VehicleDriveMode::AllWheelDrive);
    EXPECT_NEAR(rv.m_FrontTorqueSplit, 0.35f, kEps);
    EXPECT_NEAR(rv.m_LeftRightSplit, 0.45f, kEps);
    EXPECT_NEAR(rv.m_LimitedSlipRatio, 2.2f, kEps);
    EXPECT_NEAR(rv.m_CenterLimitedSlipRatio, 3.1f, kEps);
    EXPECT_NEAR(rv.m_DifferentialRatio, 4.11f, kEps);
}
