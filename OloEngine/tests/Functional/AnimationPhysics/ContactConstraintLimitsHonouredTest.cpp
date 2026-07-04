#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// ContactConstraintLimitsHonouredTest — Functional Test.
//
// Cross-subsystem seam under test:
//   PhysicsSettings (project config) × JoltScene::Initialize (Jolt bootstrap).
//   Before issue #523, JoltScene hardcoded its own s_MaxBodies / s_MaxBodyPairs /
//   s_MaxContactConstraints constexpr constants and silently ignored
//   PhysicsSettings entirely — a project could raise MaxContactConstraints and
//   it would have zero effect, and the default (10240) was far too small for
//   the advertised 65536-body ceiling, so a dense pile could overflow Jolt's
//   contact-constraint buffer and bodies would tunnel through static geometry.
//
// Scenario:
//   (a) Set a custom PhysicsSettings before physics starts and assert
//       JoltScene actually passed those exact numbers to
//       JPH::PhysicsSystem::Init (via the GetMaxBodies/GetMaxBodyPairs/
//       GetMaxContactConstraints diagnostics accessors) instead of its own
//       hardcoded constants.
//   (b) Drop a modest pile of dynamic bodies onto a static floor under the
//       (now-raised) default settings and assert every one settles at floor
//       height instead of tunnelling through it.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Physics3D/Physics3DSystem.h"
#include "OloEngine/Physics3D/PhysicsSettings.h"
#include "OloEngine/Physics3D/JoltScene.h"

#include <cmath>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    Entity CreateFloor(Scene& scene, f32 topY)
    {
        Entity floor = scene.CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, topY - 0.5f, 0.0f };
        auto& floorBody = floor.AddComponent<Rigidbody3DComponent>();
        floorBody.m_Type = BodyType3D::Static;
        auto& floorCol = floor.AddComponent<BoxCollider3DComponent>();
        floorCol.m_HalfExtents = { 50.0f, 0.5f, 50.0f };
        return floor;
    }
} // namespace

class ContactConstraintLimitsHonouredTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // BuildScene() intentionally does nothing subsystem-specific — each
        // TEST_F configures PhysicsSettings and the scene itself, since the
        // two tests in this file need different settings before
        // EnablePhysics3D() bootstraps JoltScene::Initialize().
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        // PhysicsSettings is a process-static (Physics3DSystem::s_PhysicsSettings);
        // reset it so this test can't leak custom limits into later tests.
        Physics3DSystem::SetSettings(PhysicsSettings::GetDefaults());
    }
};

TEST_F(ContactConstraintLimitsHonouredTest, CustomSettingsAreAppliedNotHardcoded)
{
    PhysicsSettings settings = PhysicsSettings::GetDefaults();
    settings.m_MaxBodies = 4096;
    settings.m_MaxBodyPairs = 8192;
    settings.m_MaxContactConstraints = 2048;
    Physics3DSystem::SetSettings(settings);

    CreateFloor(GetScene(), /*topY=*/0.0f);
    EnablePhysics3D();

    JoltScene* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);

    EXPECT_EQ(joltScene->GetMaxBodies(), settings.m_MaxBodies)
        << "JoltScene::Init ignored PhysicsSettings::m_MaxBodies";
    EXPECT_EQ(joltScene->GetMaxBodyPairs(), settings.m_MaxBodyPairs)
        << "JoltScene::Init ignored PhysicsSettings::m_MaxBodyPairs";
    EXPECT_EQ(joltScene->GetMaxContactConstraints(), settings.m_MaxContactConstraints)
        << "JoltScene::Init ignored PhysicsSettings::m_MaxContactConstraints — the two-init-path "
           "divergence from issue #523 regressed";
}

// Issue #540: before this fix, JoltScene::InitializeJolt hardcoded gravity to
// -9.81 and the fixed timestep to 1/60, and never pushed solver/sleep tuning
// onto its own JPH::PhysicsSystem at all — those settings only ever reached
// the never-stepped Physics3DSystem::m_PhysicsSystem, a no-op on the live sim.
// A project authoring non-Earth gravity, a custom timestep, or solver/sleep
// tuning had zero effect on how bodies actually simulated.
TEST_F(ContactConstraintLimitsHonouredTest, GravityTimestepAndSolverSettingsAreAppliedToLiveScene)
{
    PhysicsSettings settings = PhysicsSettings::GetDefaults();
    settings.m_Gravity = { 0.0f, -1.62f, 0.0f }; // lunar gravity — far from the -9.81 default
    settings.m_FixedTimestep = 1.0f / 30.0f;     // far from the 1/60 default
    settings.m_VelocitySolverIterations = 6;     // far from the default of 10
    settings.m_PositionSolverIterations = 4;     // far from the default of 2
    settings.m_AllowSleeping = false;            // far from the default of true
    Physics3DSystem::SetSettings(settings);

    CreateFloor(GetScene(), /*topY=*/0.0f);
    EnablePhysics3D();

    JoltScene* joltScene = GetScene().GetPhysicsScene();
    ASSERT_NE(joltScene, nullptr);

    const glm::vec3 gravity = joltScene->GetGravity();
    EXPECT_NEAR(gravity.x, settings.m_Gravity.x, 1e-4f) << "JoltScene ignored PhysicsSettings::m_Gravity.x";
    EXPECT_NEAR(gravity.y, settings.m_Gravity.y, 1e-4f)
        << "JoltScene::InitializeJolt ignored PhysicsSettings::m_Gravity — still hardcoded -9.81";
    EXPECT_NEAR(gravity.z, settings.m_Gravity.z, 1e-4f) << "JoltScene ignored PhysicsSettings::m_Gravity.z";

    EXPECT_NEAR(joltScene->GetFixedTimeStep(), settings.m_FixedTimestep, 1e-6f)
        << "JoltScene::InitializeJolt ignored PhysicsSettings::m_FixedTimestep — still hardcoded 1/60";

    const JPH::PhysicsSettings appliedJoltSettings = joltScene->GetAppliedPhysicsSettings();
    EXPECT_EQ(appliedJoltSettings.mNumVelocitySteps, settings.m_VelocitySolverIterations)
        << "JoltScene::InitializeJolt never applied PhysicsSettings::m_VelocitySolverIterations to "
           "the live JPH::PhysicsSystem";
    EXPECT_EQ(appliedJoltSettings.mNumPositionSteps, settings.m_PositionSolverIterations)
        << "JoltScene::InitializeJolt never applied PhysicsSettings::m_PositionSolverIterations to "
           "the live JPH::PhysicsSystem";
    EXPECT_EQ(appliedJoltSettings.mAllowSleeping, settings.m_AllowSleeping)
        << "JoltScene::InitializeJolt never applied PhysicsSettings::m_AllowSleeping to the live "
           "JPH::PhysicsSystem";
}

// Issue #540: a dropped body's fall distance depends on gravity actually
// reaching the live simulation, not just on the accessor reporting the right
// number back. This is the runtime-behavior half of the settings-plumbing
// check above.
TEST_F(ContactConstraintLimitsHonouredTest, CustomGravityChangesFallBehavior)
{
    PhysicsSettings settings = PhysicsSettings::GetDefaults();
    settings.m_Gravity = { 0.0f, -1.62f, 0.0f }; // lunar gravity — much weaker than -9.81
    Physics3DSystem::SetSettings(settings);

    CreateFloor(GetScene(), /*topY=*/-1000.0f); // far below so the body never lands during the test

    Entity ball = GetScene().CreateEntity("FallingBall");
    constexpr f32 kStartY = 10.0f;
    ball.GetComponent<TransformComponent>().Translation = { 0.0f, kStartY, 0.0f };
    auto& body = ball.AddComponent<Rigidbody3DComponent>();
    body.m_Type = BodyType3D::Dynamic;
    body.m_Mass = 1.0f;
    auto& col = ball.AddComponent<SphereCollider3DComponent>();
    col.m_Radius = 0.5f;

    EnablePhysics3D();
    TickFor(/*seconds=*/1.0f);

    const f32 fallDistance = kStartY - ball.GetComponent<TransformComponent>().Translation.y;
    ASSERT_TRUE(std::isfinite(fallDistance)) << "ball transform contains NaN/Inf";

    // Under Earth gravity (-9.81) the ball would fall ~4.9m in 1s (0.5 * 9.81 * 1^2);
    // under lunar gravity (-1.62) it falls ~0.81m. Assert it's well below the
    // Earth-gravity distance, catching a JoltScene that silently kept -9.81.
    constexpr f32 kEarthGravityOneSecondFallDistance = 4.9f;
    EXPECT_LT(fallDistance, kEarthGravityOneSecondFallDistance * 0.75f)
        << "ball fell as if under Earth gravity (-9.81) — JoltScene ignored the authored lunar "
           "gravity (fallDistance="
        << fallDistance << ")";
    EXPECT_GT(fallDistance, 0.1f) << "ball didn't fall at all — gravity may be zero or disabled";
}

TEST_F(ContactConstraintLimitsHonouredTest, DensePileSettlesOnFloorInsteadOfTunnelling)
{
    // Defaults now match m_MaxContactConstraints to m_MaxBodyPairs (65536) rather
    // than the old 10240, which was too small for the advertised 65536-body
    // ceiling — see PhysicsSettings.h.
    Physics3DSystem::SetSettings(PhysicsSettings::GetDefaults());

    constexpr f32 kFloorTop = 0.0f;
    constexpr f32 kBoxHalfExtent = 0.5f;
    constexpr u32 kGridX = 5;
    constexpr u32 kGridZ = 4;
    constexpr u32 kBodyCount = 80;

    CreateFloor(GetScene(), kFloorTop);

    std::vector<Entity> boxes;
    boxes.reserve(kBodyCount);
    for (u32 i = 0; i < kBodyCount; ++i)
    {
        // Stack in a tight grid a little above the floor so most bodies are
        // in simultaneous mutual contact once they land — the scenario that
        // overflowed the old undersized contact-constraint buffer. The grid
        // footprint (kGridX * kGridZ) is smaller than kBodyCount so the pile
        // wraps into additional vertical layers instead of staying flat.
        const u32 layerSize = kGridX * kGridZ;
        const f32 x = static_cast<f32>(i % kGridX) * (kBoxHalfExtent * 2.0f);
        const f32 z = static_cast<f32>(i / kGridX % kGridZ) * (kBoxHalfExtent * 2.0f);
        const f32 y = 2.0f + static_cast<f32>(i / layerSize) * (kBoxHalfExtent * 2.0f);

        Entity box = GetScene().CreateEntity("PileBox");
        box.GetComponent<TransformComponent>().Translation = { x, y, z };
        auto& body = box.AddComponent<Rigidbody3DComponent>();
        body.m_Type = BodyType3D::Dynamic;
        body.m_Mass = 1.0f;
        auto& col = box.AddComponent<BoxCollider3DComponent>();
        col.m_HalfExtents = { kBoxHalfExtent, kBoxHalfExtent, kBoxHalfExtent };
        boxes.push_back(box);
    }

    EnablePhysics3D();

    // Generous settle window: piles take longer than a single dropped body.
    TickFor(/*seconds=*/5.0f);

    // Resting on the floor or stacked on another box both keep y >= floor
    // top; tunnelling through the static floor is the only way to end up
    // below it.
    constexpr f32 kTunnelEpsilon = 0.1f;
    for (Entity box : boxes)
    {
        const auto& translation = box.GetComponent<TransformComponent>().Translation;
        ASSERT_TRUE(std::isfinite(translation.x) && std::isfinite(translation.y) && std::isfinite(translation.z))
            << "box transform contains NaN/Inf";
        EXPECT_GE(translation.y, kFloorTop - kTunnelEpsilon)
            << "box tunnelled through the static floor (y=" << translation.y << ")";
    }
}
