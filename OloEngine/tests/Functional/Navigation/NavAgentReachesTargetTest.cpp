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
#include "OloEngine/Navigation/CrowdManager.h"
#include "OloEngine/Navigation/NavigationSystem.h"
#include "OloEngine/AI/BehaviorTree/BTTasks.h"

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

    Entity CreateRootTwin(Scene& scene)
    {
        scene.SetNavMesh(GetScene().GetNavMesh());
        Entity twin = scene.CreateEntity("Root twin");
        twin.GetComponent<TransformComponent>().Translation = kStart;
        auto& nav = twin.AddComponent<NavAgentComponent>(m_Agent.GetComponent<NavAgentComponent>());
        nav.m_TargetPosition = kTarget;
        nav.m_HasTarget = true;
        return twin;
    }

    Entity ParentAgentAtStart()
    {
        Entity rig = GetScene().CreateEntity("Carrier");
        auto& carrier = rig.GetComponent<TransformComponent>();
        carrier.Translation = { -3.0f, 0.0f, 1.0f };
        carrier.SetRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
        m_Agent.SetParent(rig);
        const glm::quat inverseRotation = glm::inverse(carrier.GetRotation());
        auto& local = m_Agent.GetComponent<TransformComponent>();
        local.Translation = inverseRotation * (kStart - carrier.Translation);
        local.SetRotation(inverseRotation);
        return rig;
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

TEST_F(NavAgentReachesTargetTest, ParentedEmitterSeesTheSameTicksNavigationMove)
{
    // Identity preserves the existing navigation contract, while parenting
    // takes particle placement through the hierarchy in the same tick.
    Entity rig = GetScene().CreateEntity("Emitter Rig");
    m_Agent.SetParent(rig);
    auto& emitter = m_Agent.AddComponent<ParticleSystemComponent>().System;
    emitter.UseGPU = false;
    emitter.Playing = true;

    for (i32 frame = 0; frame < 60; ++frame)
    {
        RunFrames(1);
        const glm::vec3 position = m_Agent.GetComponent<TransformComponent>().Translation;
        ASSERT_NEAR(glm::length(emitter.GetEmitterPosition() - position), 0.0f, 1.0e-4f) << frame;
    }
    EXPECT_GT(emitter.GetEmitterPosition().x, kStart.x + 1.0f);
}

TEST_F(NavAgentReachesTargetTest, RegisteredAgentTracksMovingParentLikeARepositionedRootTwin)
{
    auto twinScene = Ref<Scene>::Create();
    Entity twin = CreateRootTwin(*twinScene);
    Entity rig = ParentAgentAtStart();
    auto& carrier = rig.GetComponent<TransformComponent>();

    for (u32 frame = 0; frame < 60; ++frame)
    {
        if (frame == 12)
        {
            const glm::vec3 shift(2.0f, 0.0f, 2.0f);
            carrier.Translation += shift;
            twin.GetComponent<TransformComponent>().Translation += shift;
        }
        RunFrames(1);
        NavigationSystem::OnUpdate(twinScene.Raw(), 1.0f / 60.0f);
        const glm::vec3 world = GetScene().ComputeWorldPosition(m_Agent);
        EXPECT_NEAR(glm::length(world - twinScene->ComputeWorldPosition(twin)), 0.0f, 1.0e-3f) << frame;
        glm::vec3 crowd{};
        ASSERT_TRUE(GetScene().GetCrowdManager()->GetAgentPosition(m_Agent.GetComponent<NavAgentComponent>().m_CrowdAgentId, crowd));
        EXPECT_NEAR(world.x, crowd.x, 1.0e-3f);
        EXPECT_NEAR(world.z, crowd.z, 1.0e-3f);
        EXPECT_NEAR(world.y, kStart.y, 1.0e-4f);
        if (frame == 12)
            EXPECT_GT(world.z, 1.5f);
    }
}

TEST_F(NavAgentReachesTargetTest, ParentedRootMotionUsesTheRootTwinsWorldCornersAndHeading)
{
    auto& nav = m_Agent.GetComponent<NavAgentComponent>();
    nav.m_UseRootMotion = true;
    auto twinScene = Ref<Scene>::Create();
    Entity twin = CreateRootTwin(*twinScene);
    ParentAgentAtStart();

    for (u32 frame = 0; frame < 30; ++frame)
    {
        RunFrames(1);
        NavigationSystem::OnUpdate(twinScene.Raw(), 1.0f / 60.0f);
        const auto pose = GetScene().ComputeWorldTRS(m_Agent);
        const auto root = twinScene->ComputeWorldTRS(twin);
        EXPECT_NEAR(glm::distance(pose.Position, kStart), 0.0f, 1.0e-4f);
        EXPECT_NEAR(std::abs(glm::dot(pose.Rotation, root.Rotation)), 1.0f, 1.0e-4f) << frame;
    }
    const auto& rootNav = twin.GetComponent<NavAgentComponent>();
    ASSERT_GE(nav.m_PathCorners.Num(), 2);
    ASSERT_EQ(nav.m_PathCorners.Num(), rootNav.m_PathCorners.Num());
    for (i32 i = 0; i < nav.m_PathCorners.Num(); ++i)
        EXPECT_NEAR(glm::distance(nav.m_PathCorners[i], rootNav.m_PathCorners[i]), 0.0f, 1.0e-3f);
    const auto forward = GetScene().ComputeWorldTRS(m_Agent).Rotation * glm::vec3(0, 0, 1);
    EXPECT_GT(forward.x, 0.99f);
}

TEST_F(NavAgentReachesTargetTest, ManualFollowerReachesWorldTargetUnderMovingParent)
{
    GetScene().GetCrowdManager()->Shutdown();
    Entity rig = ParentAgentAtStart();
    RunFrames(10);
    rig.GetComponent<TransformComponent>().Translation.z += 2.0f;
    TickFor(4.0f);
    const auto position = GetScene().ComputeWorldPosition(m_Agent);
    EXPECT_LT(glm::distance(position, kTarget), 0.31f);

    BTBlackboard blackboard;
    blackboard.Set("goal", kTarget);
    BTMoveTo move;
    move.TargetBlackboardKey = "goal";
    EXPECT_EQ(move.Tick(1.0f / 60.0f, blackboard, m_Agent), BTStatus::Success);
}
