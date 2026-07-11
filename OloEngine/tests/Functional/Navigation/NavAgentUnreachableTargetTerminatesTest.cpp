#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// NavAgentUnreachableTargetTerminatesTest — Functional Test.
//
// Cross-subsystem seam under test:
//   NavMeshGenerator (Recast bake) × NavMeshQuery::FindPath (Detour
//   DT_PARTIAL_RESULT) × NavigationSystem (per-frame agent stepping) ×
//   NavAgentComponent × BTMoveTo (behavior-tree consumer).
//
// Regression target: FindPath used to report a PARTIAL path (target off-navmesh
// or in a disconnected region) as full success. The manual path-follower would
// walk the agent to the nearest reachable point, clear its target, and the
// BTMoveTo node — checking arrival against the raw, never-reached target — would
// re-issue the same impossible target and return Running forever. The NPC was
// physically stuck while the behavior tree hung.
//
// Scenario: two floor platforms separated by a gap wide enough that, after
// agent-radius erosion, they are disconnected navmesh islands (same recipe as
// NavMeshOffMeshLinkBridgesGapTest). An agent sits on platform 1; the target is
// on platform 2 — reachable only as a partial path to the gap edge.
//
// Two assertions:
//   1. The raw NavigationSystem follower LATCHES m_TargetUnreachable (instead of
//      silently clearing the target) and does not teleport across the gap.
//   2. BTMoveTo TERMINATES with Failure within a bounded number of ticks rather
//      than spinning on Running.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Navigation/NavMeshGenerator.h"
#include "OloEngine/Navigation/NavMeshSettings.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/AI/BehaviorTree/BTTasks.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"

#include <cmath>

using namespace OloEngine;
using namespace OloEngine::Functional;

class NavAgentUnreachableTargetTerminatesTest : public FunctionalTest
{
  protected:
    // Agent starts on platform 1; the target sits on the disconnected platform 2.
    static constexpr glm::vec3 kStart{ -8.0f, 0.5f, 0.0f };
    static constexpr glm::vec3 kUnreachableTarget{ 8.0f, 0.5f, 0.0f };

    void BuildScene() override
    {
        // Two thin static floors with a gap in x ∈ [-2, 2]. After radius erosion
        // the walkable areas are disconnected — no path bridges the gap.
        auto addPlatform = [this](const char* name, f32 centerX)
        {
            auto p = GetScene().CreateEntity(name);
            p.GetComponent<TransformComponent>().Translation = { centerX, -0.05f, 0.0f };
            Rigidbody3DComponent body;
            body.m_Type = BodyType3D::Static;
            BoxCollider3DComponent col;
            col.m_HalfExtents = { 4.0f, 0.05f, 5.0f };
            p.AddComponent<BoxCollider3DComponent>(col);
            p.AddComponent<Rigidbody3DComponent>(body);
        };
        addPlatform("Platform1", -6.0f); // spans x ∈ [-10, -2]
        addPlatform("Platform2", 6.0f);  // spans x ∈ [2, 10]

        EnablePhysics3D();

        NavMeshSettings settings;
        const auto navMesh = NavMeshGenerator::Generate(
            &GetScene(), settings,
            /*boundsMin=*/glm::vec3(-15.0f, -2.0f, -7.0f),
            /*boundsMax=*/glm::vec3(15.0f, 5.0f, 7.0f));
        ASSERT_TRUE(navMesh && navMesh->IsValid())
            << "two-platform bake failed — pre-condition broken.";
        GetScene().SetNavMesh(navMesh);

        // Agent on platform 1, no target yet (each test drives its own).
        m_Agent = GetScene().CreateEntity("Agent");
        m_Agent.GetComponent<TransformComponent>().Translation = kStart;
        m_Agent.AddComponent<NavAgentComponent>();
        auto& agent = m_Agent.GetComponent<NavAgentComponent>();
        agent.m_MaxSpeed = 8.0f;
        agent.m_StoppingDistance = 0.3f;
        agent.m_LockYAxis = true;
    }

    static f32 DistXZ(const glm::vec3& a, const glm::vec3& b)
    {
        const f32 dx = a.x - b.x;
        const f32 dz = a.z - b.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    Entity m_Agent;
};

// The raw follower must latch "unreachable" and hold the target observable, not
// walk the agent across the gap and not silently drop the target.
TEST_F(NavAgentUnreachableTargetTerminatesTest, ManualFollowerLatchesUnreachableInsteadOfArriving)
{
    {
        auto& agent = m_Agent.GetComponent<NavAgentComponent>();
        agent.m_TargetPosition = kUnreachableTarget;
        agent.m_HasTarget = true;
        agent.m_HasPath = false;
        agent.m_TargetUnreachable = false;
    }

    // Plenty of time for the agent to walk to the gap edge and stop.
    TickFor(/*seconds=*/3.0f);

    const auto& agent = m_Agent.GetComponent<NavAgentComponent>();
    const glm::vec3 pos = m_Agent.GetComponent<TransformComponent>().Translation;

    EXPECT_TRUE(agent.m_TargetUnreachable)
        << "NavigationSystem did not flag the disconnected target as unreachable — "
           "FindPath is still reporting the partial path as a full success.";
    EXPECT_TRUE(agent.m_HasTarget)
        << "target was silently cleared; the terminal unreachable state must stay "
           "observable so consumers can stop re-issuing it.";

    EXPECT_TRUE(std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z))
        << "agent transform contains NaN/Inf";
    // The gap is x ∈ [-2, 2]; the agent must NOT have crossed to platform 2.
    EXPECT_LT(pos.x, 0.0f)
        << "agent crossed the impassable gap — a partial path was followed all the "
           "way to the target as if it were reachable. Final x="
        << pos.x;
    EXPECT_GT(DistXZ(pos, kUnreachableTarget), 5.0f)
        << "agent ended up near the unreachable target — the gap wasn't a gap.";
}

// BTMoveTo toward an unreachable target must return Failure within a bounded
// number of ticks instead of spinning on Running forever.
TEST_F(NavAgentUnreachableTargetTerminatesTest, BTMoveToTerminatesWithFailure)
{
    BTBlackboard bb;
    bb.Set("moveTarget", kUnreachableTarget);

    BTMoveTo node;
    node.TargetBlackboardKey = "moveTarget";

    constexpr i32 kMaxIterations = 600; // 10s of simulated time at 60 Hz — a hard ceiling
    bool sawFailure = false;
    i32 iterations = 0;

    for (; iterations < kMaxIterations; ++iterations)
    {
        const BTStatus status = node.Tick(1.0f / 60.0f, bb, m_Agent);

        ASSERT_NE(status, BTStatus::Success)
            << "BTMoveTo reported arrival at an unreachable target on iteration " << iterations;

        if (status == BTStatus::Failure)
        {
            sawFailure = true;
            break;
        }

        // Advance NavigationSystem so it can compute the (partial) path and flag
        // the target unreachable, which is what lets BTMoveTo terminate.
        RunFrames(1);
    }

    EXPECT_TRUE(sawFailure)
        << "BTMoveTo never terminated — it spun on Running for " << iterations
        << " ticks against an unreachable target (the original infinite-hang bug).";
    EXPECT_LT(iterations, 10)
        << "BTMoveTo eventually failed but took " << iterations
        << " ticks — the unreachable signal should propagate within a couple of frames.";
}
