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

TEST_F(ContactConstraintLimitsHonouredTest, DensePileSettlesOnFloorInsteadOfTunnelling)
{
    // Defaults now match m_MaxContactConstraints to m_MaxBodyPairs (65536) rather
    // than the old 10240, which was too small for the advertised 65536-body
    // ceiling — see PhysicsSettings.h.
    Physics3DSystem::SetSettings(PhysicsSettings::GetDefaults());

    constexpr f32 kFloorTop = 0.0f;
    constexpr f32 kBoxHalfExtent = 0.5f;
    constexpr u32 kBodyCount = 40;

    CreateFloor(GetScene(), kFloorTop);

    std::vector<Entity> boxes;
    boxes.reserve(kBodyCount);
    for (u32 i = 0; i < kBodyCount; ++i)
    {
        // Stack in a tight grid a little above the floor so most bodies are
        // in simultaneous mutual contact once they land — the scenario that
        // overflowed the old undersized contact-constraint buffer.
        const f32 x = static_cast<f32>(i % 5) * (kBoxHalfExtent * 2.0f);
        const f32 z = static_cast<f32>(i / 5 % 8) * (kBoxHalfExtent * 2.0f);
        const f32 y = 2.0f + static_cast<f32>(i / 40) * (kBoxHalfExtent * 2.0f);

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
