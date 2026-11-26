#include "OloEnginePCH.h"

// =============================================================================
// PerceptionLineOfSightBlockedByWallViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × PerceptionSystem × Physics3D (JoltScene::CastRay) ×
//   PerceptionComponent.RequireLineOfSight. When LOS is required, the sensor
//   casts a ray from the eye to a candidate target and rejects it if a third
//   body blocks the line. This pins the perception↔physics seam: a regression
//   in GetPhysicsScene() lifetime, the excluded-entity set (eye/target must not
//   self-block), or CastRay traversal would let NPCs "see" through walls.
//
// Scenario: a Watcher at the origin looking down -Z, a Target 10 m ahead, and a
// static wall box straddling the line at z = -5. Range and FOV both pass, so
// the ONLY thing that can hide the target is the LOS raycast. Toggling
// RequireLineOfSight flips visibility on the identical geometry — isolating the
// raycast as the cause.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "TestTempDir.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/AI/AIComponents.h"

#include <glm/glm.hpp>
#include <fstream>

using namespace OloEngine;
using namespace OloEngine::Functional;

class PerceptionLineOfSightBlockedByWallViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Watcher at origin, identity rotation → looks down -Z. Eye lifted to 1 m.
        m_Watcher = GetScene().CreateEntityWithUUID(UUID{ 4242 }, "Watcher");
        m_Watcher.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
        auto& pc = m_Watcher.AddComponent<PerceptionComponent>();
        pc.SightRange = 20.0f;
        pc.FovDegrees = 120.0f;
        pc.EyeOffset = { 0.0f, 1.0f, 0.0f };
        pc.RequireLineOfSight = true;
        pc.PerceiverTeam = 0;

        // Target straight ahead, well inside range and cone, eye-height.
        m_Target = GetScene().CreateEntity("Target");
        m_Target.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, -10.0f };
        auto& perc = m_Target.AddComponent<PerceptibleComponent>();
        perc.Team = 1;
        perc.IsPerceptible = true;

        // Static wall straddling the line of sight at z = -5.
        m_Wall = GetScene().CreateEntity("Wall");
        m_Wall.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, -5.0f };
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Static;
        m_Wall.AddComponent<Rigidbody3DComponent>(body);
        BoxCollider3DComponent col;
        col.m_HalfExtents = { 3.0f, 3.0f, 0.5f };
        m_Wall.AddComponent<BoxCollider3DComponent>(col);

        // Bodies are created from existing components here.
        EnablePhysics3D();
    }

    [[nodiscard("test asserts on the returned sensor result")]] const PerceptionComponent& Perception() const
    {
        return m_Watcher.GetComponent<PerceptionComponent>();
    }

    Entity m_Watcher;
    Entity m_Target;
    Entity m_Wall;
};

TEST_F(PerceptionLineOfSightBlockedByWallViaSceneTickTest, WallBetweenWatcherAndTargetBlocksSight)
{
    RunFrames(1);

    EXPECT_FALSE(Perception().HasVisibleTarget)
        << "Watcher saw a target straight through a solid wall — LOS raycast (or "
           "the eye/target exclusion set) is broken";
}

TEST_F(PerceptionLineOfSightBlockedByWallViaSceneTickTest, DisablingLineOfSightSeesThroughWall)
{
    // Identical geometry; only the LOS requirement changes. The target now
    // passes range+FOV and there is no occlusion gate — so it becomes visible.
    // This is the positive control proving the wall was the sole blocker above.
    m_Watcher.GetComponent<PerceptionComponent>().RequireLineOfSight = false;

    RunFrames(1);

    EXPECT_TRUE(Perception().HasVisibleTarget)
        << "With LOS disabled the in-range, in-cone target should be visible — "
           "range/FOV gate is mis-rejecting it";
    EXPECT_EQ(Perception().VisibleTarget, m_Target.GetUUID());
}

TEST_F(PerceptionLineOfSightBlockedByWallViaSceneTickTest, MovingCarrierUsesRootTwinsSightRayAndWorldIndex)
{
    RunFrames(1);
    ASSERT_FALSE(Perception().HasVisibleTarget);

    const glm::vec3 target{ 0.0f, 1.0f, -10.0f };
    Entity rig = GetScene().CreateEntity("Perception rig");
    auto& carrier = rig.GetComponent<TransformComponent>();
    carrier.Translation = { 30.0f, 2.0f, 0.0f };
    carrier.SetRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
    const glm::quat inverseRotation = glm::inverse(carrier.GetRotation());
    m_Watcher.SetParent(rig);
    auto& sensor = m_Watcher.GetComponent<TransformComponent>();
    sensor.Translation = inverseRotation * -carrier.Translation;
    sensor.SetRotation(inverseRotation);
    m_Target.SetParent(rig);
    m_Target.GetComponent<TransformComponent>().Translation = inverseRotation * (target - carrier.Translation);
    RunFrames(1);
    EXPECT_TRUE(GetScene().QueryEntitiesInRadius(target, 0.1f).Contains(m_Target.GetUUID()));
    EXPECT_FALSE(Perception().HasVisibleTarget);

    // Both endpoints move around the end of the stationary wall this tick.
    const glm::vec3 shift{ 6.0f, 0.0f, 0.0f };
    carrier.Translation += shift;
    RunFrames(1);
    ASSERT_TRUE(Perception().HasVisibleTarget);
    EXPECT_EQ(Perception().VisibleTarget, m_Target.GetUUID());
    EXPECT_NEAR(glm::distance(Perception().LastKnownPosition, target + shift), 0.0f, 1.0e-4f);
    EXPECT_TRUE(GetScene().QueryEntitiesInRadius(target + shift, 0.1f).Contains(m_Target.GetUUID()));
    EXPECT_FALSE(GetScene().QueryEntitiesInRadius(target, 0.1f).Contains(m_Target.GetUUID()));
}

TEST_F(PerceptionLineOfSightBlockedByWallViaSceneTickTest, LuaPassesTheParentedSightPositionToNavigation)
{
    Entity rig = GetScene().CreateEntity("Target carrier");
    rig.GetComponent<TransformComponent>().Translation = { 3.0f, 0.0f, 0.0f };
    m_Target.SetParent(rig);
    m_Watcher.GetComponent<PerceptionComponent>().RequireLineOfSight = false;
    m_Watcher.AddComponent<NavAgentComponent>();
    EnableLua();
    const auto script = OloEngine::Tests::TempFile("perceived_nav_target.lua");
    std::ofstream(script) << R"(
return { OnUpdate = function(entityID, dt)
    local id = math.tointeger(entityID)
    local sight = entity_utils.get_component(id, "PerceptionComponent")
    if sight.hasVisibleTarget then
        entity_utils.get_component(id, "NavAgentComponent").targetPosition = sight.lastKnownPosition
    end
end }
)";
    RegisterLuaScript(m_Watcher, script);
    RunFrames(3);
    const auto& nav = m_Watcher.GetComponent<NavAgentComponent>();
    ASSERT_TRUE(nav.m_HasTarget);
    EXPECT_NEAR(glm::distance(nav.m_TargetPosition, glm::vec3(3, 1, -10)), 0.0f, 1.0e-4f);
}
