#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// CrowdAvoidancePreventsOverlapTest — Functional Test.
//
// Cross-subsystem seam under test:
//   NavMeshGenerator (Recast bake) x CrowdManager (DetourCrowd) x
//   NavigationSystem (per-frame crowd registration / target routing / sync) x
//   NavAgentComponent lifecycle (add, target, component-remove, entity-destroy).
//
// Regression target (issue #616): CrowdManager was constructed and ticked every
// frame but nothing ever called AddAgent/SetAgentTarget, so every NavAgent took
// the naive single-agent fallback with zero inter-agent separation or
// avoidance — two agents walking toward each other would pass straight through
// one another. NavigationSystem now lazily registers each NavAgentComponent
// with the crowd on its first tick and routes target changes through
// CrowdManager::SetAgentTarget instead.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Navigation/CrowdManager.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/NavMesh.h"

#include <algorithm>
#include <limits>

using namespace OloEngine;
using namespace OloEngine::Functional;

class CrowdAvoidancePreventsOverlapTest : public FunctionalTest
{
  protected:
    // Two agents start on opposite ends of a flat corridor and are each
    // targeted at the other's starting point, so their straight-line paths
    // cross head-on in the middle.
    static constexpr glm::vec3 kStartA{ -8.0f, 0.5f, 0.0f };
    static constexpr glm::vec3 kStartB{ 8.0f, 0.5f, 0.0f };

    void BuildScene() override
    {
        auto floor = GetScene().CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = { 0.0f, -0.05f, 0.0f };
        Rigidbody3DComponent body;
        body.m_Type = BodyType3D::Static;
        BoxCollider3DComponent col;
        col.m_HalfExtents = { 20.0f, 0.05f, 5.0f };
        floor.AddComponent<BoxCollider3DComponent>(col);
        floor.AddComponent<Rigidbody3DComponent>(body);

        EnablePhysics3D();

        NavMeshSettings settings;
        const auto navMesh = NavMeshGenerator::Generate(
            &GetScene(), settings,
            /*boundsMin=*/glm::vec3(-25.0f, -2.0f, -7.0f),
            /*boundsMax=*/glm::vec3(25.0f, 5.0f, 7.0f));
        ASSERT_TRUE(navMesh && navMesh->IsValid())
            << "flat-corridor bake failed — pre-condition broken.";
        GetScene().SetNavMesh(navMesh);

        m_AgentA = GetScene().CreateEntity("AgentA");
        m_AgentA.GetComponent<TransformComponent>().Translation = kStartA;
        m_AgentA.AddComponent<NavAgentComponent>();
        {
            auto& a = m_AgentA.GetComponent<NavAgentComponent>();
            a.m_MaxSpeed = 4.0f;
            a.m_StoppingDistance = 0.3f;
            a.m_LockYAxis = true;
        }

        m_AgentB = GetScene().CreateEntity("AgentB");
        m_AgentB.GetComponent<TransformComponent>().Translation = kStartB;
        m_AgentB.AddComponent<NavAgentComponent>();
        {
            auto& b = m_AgentB.GetComponent<NavAgentComponent>();
            b.m_MaxSpeed = 4.0f;
            b.m_StoppingDistance = 0.3f;
            b.m_LockYAxis = true;
        }
    }

    void SetTargets()
    {
        auto& a = m_AgentA.GetComponent<NavAgentComponent>();
        a.m_TargetPosition = kStartB;
        a.m_HasTarget = true;
        a.m_HasPath = false;
        a.m_TargetUnreachable = false;

        auto& b = m_AgentB.GetComponent<NavAgentComponent>();
        b.m_TargetPosition = kStartA;
        b.m_HasTarget = true;
        b.m_HasPath = false;
        b.m_TargetUnreachable = false;
    }

    Entity m_AgentA;
    Entity m_AgentB;
};

// The core issue #616 regression: each agent must actually be handed to the
// crowd (distinct, non-negative ids), not silently stuck at the default -1.
TEST_F(CrowdAvoidancePreventsOverlapTest, AgentsAutoRegisterWithDistinctCrowdIds)
{
    RunFrames(1);

    const auto& agentA = m_AgentA.GetComponent<NavAgentComponent>();
    const auto& agentB = m_AgentB.GetComponent<NavAgentComponent>();

    EXPECT_GE(agentA.m_CrowdAgentId, 0)
        << "NavAgentComponent never got registered with the crowd — CrowdManager::AddAgent "
           "is still never called (issue #616).";
    EXPECT_GE(agentB.m_CrowdAgentId, 0);
    EXPECT_NE(agentA.m_CrowdAgentId, agentB.m_CrowdAgentId);
}

// Two agents converging head-on must not collapse onto (near) the same point —
// impossible to guarantee under the old naive single-agent follower, which had
// no concept of other agents at all.
TEST_F(CrowdAvoidancePreventsOverlapTest, ConvergingAgentsMaintainSeparation)
{
    SetTargets();

    f32 minDistance = std::numeric_limits<f32>::max();
    bool bothRegistered = false;

    // 8s of simulated time at 60 Hz — comfortably more than the ~4s a 4 m/s
    // agent needs to cross the 16m corridor, so both agents pass the midpoint.
    for (i32 i = 0; i < 480; ++i)
    {
        RunFrames(1);

        const auto& agentA = m_AgentA.GetComponent<NavAgentComponent>();
        const auto& agentB = m_AgentB.GetComponent<NavAgentComponent>();
        bothRegistered = bothRegistered || (agentA.m_CrowdAgentId >= 0 && agentB.m_CrowdAgentId >= 0);

        const glm::vec3 posA = m_AgentA.GetComponent<TransformComponent>().Translation;
        const glm::vec3 posB = m_AgentB.GetComponent<TransformComponent>().Translation;
        minDistance = std::min(minDistance, glm::distance(posA, posB));
    }

    ASSERT_TRUE(bothRegistered) << "agents never registered with the crowd manager during the run.";

    // Combined radius is 1.0 (0.5 + 0.5); DetourCrowd's separation behaviour keeps
    // agents well clear of true overlap. Under the old naive fallback the two
    // agents would walk straight through each other, driving this to ~0.
    EXPECT_GT(minDistance, 0.5f)
        << "agents collapsed onto (near) the same point while converging — DetourCrowd "
           "separation isn't preventing overlap. Minimum observed distance: "
        << minDistance;
}

// Removing the component at runtime must free the agent's crowd slot rather
// than leaving a driverless "ghost" agent registered forever.
TEST_F(CrowdAvoidancePreventsOverlapTest, RemovingComponentUnregistersFromCrowd)
{
    RunFrames(1);

    auto* crowd = GetScene().GetCrowdManager();
    ASSERT_NE(crowd, nullptr);
    const i32 activeBefore = crowd->GetActiveAgentCount();
    ASSERT_GE(activeBefore, 2);

    m_AgentA.RemoveComponent<NavAgentComponent>();

    EXPECT_EQ(crowd->GetActiveAgentCount(), activeBefore - 1);
}

// Destroying the entity must also free the crowd slot — Scene::DestroyEntity
// doesn't route through EnTT's on_destroy signal, so this needs its own
// explicit teardown (mirroring the Rigidbody3D/Terrain/Vehicle pattern).
TEST_F(CrowdAvoidancePreventsOverlapTest, DestroyingEntityUnregistersFromCrowd)
{
    RunFrames(1);

    auto* crowd = GetScene().GetCrowdManager();
    ASSERT_NE(crowd, nullptr);
    const i32 activeBefore = crowd->GetActiveAgentCount();
    ASSERT_GE(activeBefore, 2);

    GetScene().DestroyEntity(m_AgentB);

    EXPECT_EQ(crowd->GetActiveAgentCount(), activeBefore - 1);
}
