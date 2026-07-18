#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// WorldOriginRebaseNavTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene::RebaseOrigin (floating-origin, issue #613) × Navigation
//   (NavMeshGenerator bake × NavMeshQuery × CrowdManager × NavigationSystem).
//
//   A baked navmesh and its live crowd live in ABSOLUTE world space inside
//   Detour, which offers no in-place tile translate (its tile-grid origin is
//   private and drives every spatial query). A rebase would otherwise leave a
//   shifted agent pathing against an un-shifted mesh — a silent desync. #613's
//   RebaseNavigation regenerates the mesh at the shifted bounds, rebuilds the
//   query + crowd, and carries each agent's world-space target across, so the
//   agent keeps converging on the SAME absolute-space goal after the rebase.
//
// Per docs/agent-rules/crowd-manager-follower-parity.md, a valid navmesh means
// the agent is driven by the CROWD follower (not the manual one); this test
// exercises that path across the rebake/re-register cycle.
//
// Headless: navmesh bake + agent stepping are GPU-free.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/NavMesh.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class WorldOriginRebaseNavTest : public FunctionalTest
{
  protected:
    static constexpr glm::vec3 kStart{ -8.0f, 0.5f, 0.0f };
    static constexpr glm::vec3 kTarget{ 8.0f, 0.5f, 0.0f };

    void BuildScene() override
    {
        // Walkable static floor for the Recast bake.
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.05f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 20.0f, 0.05f, 5.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // An explicit bounds volume so the floating-origin REBAKE has a
        // NavMeshBoundsComponent to shift (RebaseNavigation moves these boxes).
        auto boundsEntity = GetScene().CreateEntity("NavBounds");
        auto& bounds = boundsEntity.AddComponent<NavMeshBoundsComponent>();
        bounds.m_Min = { -25.0f, -2.0f, -7.0f };
        bounds.m_Max = { 25.0f, 5.0f, 7.0f };

        EnablePhysics3D();

        NavMeshSettings settings;
        const auto navMesh = NavMeshGenerator::Generate(
            &GetScene(), settings, bounds.m_Min, bounds.m_Max);
        ASSERT_TRUE(navMesh && navMesh->IsValid())
            << "initial NavMeshGenerator::Generate failed on the floor geometry";
        GetScene().SetNavMesh(navMesh);

        m_Agent = GetScene().CreateEntity("Agent");
        m_Agent.GetComponent<TransformComponent>().Translation = kStart;
        m_Agent.AddComponent<NavAgentComponent>();
        auto& agent = m_Agent.GetComponent<NavAgentComponent>();
        agent.m_MaxSpeed = 8.0f;
        agent.m_StoppingDistance = 0.3f;
        agent.m_LockYAxis = true;
        agent.m_TargetPosition = kTarget;
        agent.m_HasTarget = true;
        agent.m_HasPath = false;
    }

    Entity m_Agent;
};

TEST_F(WorldOriginRebaseNavTest, AgentContinuesToTargetAcrossRebase)
{
    // Let the agent start moving toward the target (crowd follower registers).
    TickFor(0.6f);
    const glm::vec3 midAbs = GetScene().RebasedToAbsolute(m_Agent.GetComponent<TransformComponent>().Translation);
    EXPECT_GT(midAbs.x, kStart.x + 0.5f)
        << "agent did not start moving toward the target before the rebase";

    // Rebase the whole world far away. This regenerates the navmesh at the
    // shifted bounds, rebuilds query + crowd, and carries the agent's target.
    const glm::vec3 shift{ -4096.0f, 0.0f, 2048.0f };
    GetScene().RebaseOrigin(shift);

    // The navmesh was rebaked (not dropped) and is valid in the new frame.
    ASSERT_TRUE(GetScene().GetNavMesh() && GetScene().GetNavMesh()->IsValid())
        << "navmesh should have been rebaked during the rebase, not dropped";

    // The agent's world-space target followed the rebase — its ABSOLUTE target
    // is unchanged (proving the target was shifted into the new frame).
    const auto& agent = m_Agent.GetComponent<NavAgentComponent>();
    EXPECT_TRUE(agent.m_HasTarget) << "agent lost its target across the rebase";
    const glm::vec3 targetAbs = GetScene().RebasedToAbsolute(agent.m_TargetPosition);
    EXPECT_NEAR(targetAbs.x, kTarget.x, 0.5f) << "agent's absolute target drifted across the rebase";
    EXPECT_NEAR(targetAbs.z, kTarget.z, 0.5f);

    // The bounds volume also shifted so a future rebake stays consistent.
    const glm::vec3 agentAbsAfter = GetScene().RebasedToAbsolute(m_Agent.GetComponent<TransformComponent>().Translation);
    EXPECT_NEAR(agentAbsAfter.x, midAbs.x, 0.5f)
        << "agent's absolute position jumped at the rebase boundary";

    // Continue: the agent must reach the target in ABSOLUTE space, proving the
    // navmesh, crowd, and target all rebased into the SAME (shifted) frame and
    // the crowd follower re-registered against the regenerated mesh.
    const bool reached = TickUntil(
        [&]()
        {
            const glm::vec3 a = GetScene().RebasedToAbsolute(m_Agent.GetComponent<TransformComponent>().Translation);
            const glm::vec3 d{ kTarget.x - a.x, 0.0f, kTarget.z - a.z };
            return glm::length(d) < 0.6f;
        },
        /*timeoutSeconds=*/8.0f);

    EXPECT_TRUE(reached)
        << "agent failed to reach the target in absolute space after the rebase — "
           "the navmesh/crowd/target did not rebase consistently (silent nav desync).";
    EXPECT_TRUE(std::isfinite(agentAbsAfter.x));
}
