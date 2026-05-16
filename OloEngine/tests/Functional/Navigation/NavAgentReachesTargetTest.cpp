#include "OloEnginePCH.h"

// =============================================================================
// NavAgentReachesTargetTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × NavMeshGenerator (Recast bake) × NavMeshQuery (Detour
//   pathfind) × NavigationSystem (per-frame agent stepping) × NavAgentComponent.
//   The full nav stack: bake a navmesh from scene geometry, set it on the
//   scene, give an agent a target, and verify Navigation::OnUpdate
//   converges the agent's TransformComponent toward the goal across ticks.
//
// Scenario: a static floor (collider geometry the Recast bake walks),
// a NavAgent placed at one end of it with a target at the other end.
// After ticking enough frames for the agent to traverse at its
// m_MaxSpeed, the agent's transform should be within stopping
// distance of the target.
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

class NavAgentReachesTargetTest : public FunctionalTest
{
  protected:
    static constexpr glm::vec3 kStart{ -5.0f, 0.5f, 0.0f };
    static constexpr glm::vec3 kTarget{ 5.0f, 0.5f, 0.0f };

    void BuildScene() override
    {
        // Build geometry the navmesh bake can walk on. A static box
        // collider counts as scene geometry per NavMeshGenerator (header
        // comment: "MeshComponents + TerrainComponents + Collider3DComponents").
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.05f, 0.0f };
        Rigidbody3DComponent floorBody;
        floorBody.m_Type = BodyType3D::Static;
        BoxCollider3DComponent floorCol;
        floorCol.m_HalfExtents = { 20.0f, 0.05f, 5.0f };
        floor.AddComponent<BoxCollider3DComponent>(floorCol);
        floor.AddComponent<Rigidbody3DComponent>(floorBody);

        // We need Physics3D started for collider geometry to be walkable
        // by the navmesh bake (Recast walks the actual physics shapes).
        EnablePhysics3D();

        // Bake the navmesh. Bounds y-max must clear AgentHeight (2.0m) above the
        // floor surface (y=0) or rcFilterWalkableLowHeightSpans removes the whole
        // walkable area for lack of headroom.
        NavMeshSettings settings;
        const auto navMesh = NavMeshGenerator::Generate(
            &GetScene(), settings,
            /*boundsMin=*/glm::vec3(-25.0f, -2.0f, -7.0f),
            /*boundsMax=*/glm::vec3(25.0f, 5.0f, 7.0f));
        ASSERT_TRUE(navMesh && navMesh->IsValid())
            << "NavMeshGenerator::Generate produced an invalid mesh — Recast bake "
               "failed on the static-collider geometry.";

        GetScene().SetNavMesh(navMesh);

        // Create the nav agent at kStart, target at kTarget.
        m_Agent = GetScene().CreateEntity("Agent");
        m_Agent.GetComponent<TransformComponent>().Translation = kStart;
        m_Agent.AddComponent<NavAgentComponent>();
        auto& agent = m_Agent.GetComponent<NavAgentComponent>();
        agent.m_MaxSpeed = 8.0f; // fast enough to cross in a couple seconds
        agent.m_StoppingDistance = 0.3f;
        agent.m_LockYAxis = true;
        agent.m_TargetPosition = kTarget;
        agent.m_HasTarget = true;
        agent.m_HasPath = false; // force path recomputation on first tick
    }

    Entity m_Agent;
};

TEST_F(NavAgentReachesTargetTest, AgentMovesAcrossFloorAndArrivesNearTarget)
{
    const glm::vec3 startPos = m_Agent.GetComponent<TransformComponent>().Translation;
    ASSERT_NEAR(startPos.x, kStart.x, 1e-3f);

    // Distance to cover ≈ 10m, speed 8 m/s → ~1.25s. Give it 5s of slack.
    TickFor(/*seconds=*/5.0f);

    const auto& endPos = m_Agent.GetComponent<TransformComponent>().Translation;
    EXPECT_TRUE(std::isfinite(endPos.x) && std::isfinite(endPos.y) && std::isfinite(endPos.z))
        << "agent transform contains NaN/Inf";

    const glm::vec3 toTargetXZ{ kTarget.x - endPos.x, 0.0f, kTarget.z - endPos.z };
    const f32 distXZ = glm::length(toTargetXZ);

    EXPECT_LT(distXZ, 0.5f)
        << "agent did not reach the target within 5s of simulated time; "
        << "final position (" << endPos.x << "," << endPos.y << "," << endPos.z << "), "
        << "distance-to-target on XZ = " << distXZ
        << " — NavigationSystem::OnUpdate is either skipping the agent or "
           "FindPath returned no path on a baked mesh.";
}
