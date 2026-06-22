#include "OloEnginePCH.h"

// =============================================================================
// PerceptionDetectsTargetViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × PerceptionSystem × PerceptionComponent / PerceptibleComponent ×
//   AISystem × BehaviorTreeComponent (BTCanSeeTarget) × BTBlackboard.
//   Scene::OnUpdateRuntime runs PerceptionSystem::OnUpdate immediately before
//   AISystem::OnUpdate, so within one frame the sensor result is computed and
//   then consumed by the behavior tree. A regression in that ordering (or in
//   the Scene→PerceptionSystem wiring) silently blinds every NPC — no per-node
//   or per-math test sees that.
//
// Scenario: a "Watcher" looking down -Z with a 90° / 10 m sight cone and a
// behavior tree whose root is a BTCanSeeTarget guard over a SetBlackboardValue
// task. A perceptible "Intruder" of another team is moved around to exercise
// in-cone, out-of-range, behind, same-team, cloaked, and memory cases. Each
// asserts both the component result and that the BT reacted (or didn't).
//
// LOS raycasting is disabled here (RequireLineOfSight = false) so the case is
// physics-independent; the occlusion seam is pinned separately in
// PerceptionLineOfSightBlockedByWallViaSceneTickTest.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/Perception/PerceptionSystem.h"
#include "OloEngine/AI/BehaviorTree/BehaviorTree.h"
#include "OloEngine/AI/BehaviorTree/BTPerceptionNodes.h"
#include "OloEngine/AI/BehaviorTree/BTTasks.h"

#include <glm/glm.hpp>

using namespace OloEngine;
using namespace OloEngine::Functional;

class PerceptionDetectsTargetViaSceneTickTest : public FunctionalTest
{
  protected:
    static constexpr const char* kSawFlag = "SawSomething";

    void BuildScene() override
    {
        // Watcher at the origin, default (identity) rotation → looks down -Z.
        m_Watcher = GetScene().CreateEntity("Watcher");
        m_Watcher.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };

        auto& pc = m_Watcher.AddComponent<PerceptionComponent>();
        pc.SightRange = 10.0f;
        pc.FovDegrees = 90.0f;
        pc.EyeOffset = { 0.0f, 0.0f, 0.0f }; // eye at the entity origin for simple geometry
        pc.RequireLineOfSight = false;       // physics-independent
        pc.PerceiverTeam = 0;
        pc.DetectSameTeam = false;

        // Behavior tree: run the SetBlackboardValue task only while a target is
        // visible (BTCanSeeTarget guard) — proves the node reads fresh perception.
        BehaviorTreeComponent bt;
        bt.IsRunning = true;
        bt.RuntimeTree = Ref<BehaviorTree>::Create();
        auto guard = Ref<BTCanSeeTarget>::Create();
        auto setFlag = Ref<BTSetBlackboardValue>::Create();
        setFlag->Key = kSawFlag;
        setFlag->ValueToSet = true;
        guard->Children.push_back(setFlag);
        bt.RuntimeTree->SetRoot(guard);
        m_Watcher.AddComponent<BehaviorTreeComponent>(std::move(bt));

        // Intruder of another team, straight ahead, inside range and cone.
        m_Intruder = GetScene().CreateEntity("Intruder");
        m_Intruder.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -5.0f };
        auto& perc = m_Intruder.AddComponent<PerceptibleComponent>();
        perc.Team = 1;
        perc.IsPerceptible = true;
    }

    [[nodiscard]] const PerceptionComponent& Perception() const
    {
        return m_Watcher.GetComponent<PerceptionComponent>();
    }
    [[nodiscard]] const BTBlackboard& WatcherBlackboard() const
    {
        return m_Watcher.GetComponent<BehaviorTreeComponent>().Blackboard;
    }

    Entity m_Watcher;
    Entity m_Intruder;
};

TEST_F(PerceptionDetectsTargetViaSceneTickTest, SeesTargetAheadAndDrivesBehaviorTree)
{
    RunFrames(1);

    const auto& pc = Perception();
    EXPECT_TRUE(pc.HasVisibleTarget)
        << "Watcher failed to see an in-cone, in-range target — Scene→PerceptionSystem wiring broken";
    EXPECT_EQ(pc.VisibleTarget, m_Intruder.GetUUID());
    EXPECT_TRUE(pc.HasLastKnownPosition);
    EXPECT_FLOAT_EQ(pc.LastKnownPosition.z, -5.0f);

    // PerceptionSystem mirrored the result into the AI blackboard...
    EXPECT_TRUE(WatcherBlackboard().Get<bool>(PerceptionKeys::CanSeeTarget));
    EXPECT_EQ(static_cast<u64>(WatcherBlackboard().Get<UUID>(PerceptionKeys::Target)),
              static_cast<u64>(m_Intruder.GetUUID()));

    // ...and the BTCanSeeTarget guard let its child run.
    EXPECT_TRUE(WatcherBlackboard().Get<bool>(kSawFlag))
        << "BTCanSeeTarget did not let its child tick despite a visible target";
}

TEST_F(PerceptionDetectsTargetViaSceneTickTest, DoesNotSeeTargetOutOfRange)
{
    m_Intruder.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -50.0f };

    RunFrames(1);

    EXPECT_FALSE(Perception().HasVisibleTarget);
    EXPECT_FALSE(WatcherBlackboard().Get<bool>(PerceptionKeys::CanSeeTarget));
    EXPECT_FALSE(WatcherBlackboard().Get<bool>(kSawFlag))
        << "BTCanSeeTarget ran its child for an out-of-range target";
}

TEST_F(PerceptionDetectsTargetViaSceneTickTest, DoesNotSeeTargetBehind)
{
    m_Intruder.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 5.0f }; // behind -Z look

    RunFrames(1);

    EXPECT_FALSE(Perception().HasVisibleTarget);
    EXPECT_FALSE(WatcherBlackboard().Get<bool>(kSawFlag));
}

TEST_F(PerceptionDetectsTargetViaSceneTickTest, DoesNotSeeSameTeamTarget)
{
    // Same team as the watcher; default DetectSameTeam == false filters it out.
    m_Intruder.GetComponent<PerceptibleComponent>().Team = 0;

    RunFrames(1);

    EXPECT_FALSE(Perception().HasVisibleTarget)
        << "Watcher saw an ally — team filtering failed";
}

TEST_F(PerceptionDetectsTargetViaSceneTickTest, SeesSameTeamWhenDetectSameTeamEnabled)
{
    m_Intruder.GetComponent<PerceptibleComponent>().Team = 0;
    m_Watcher.GetComponent<PerceptionComponent>().DetectSameTeam = true;

    RunFrames(1);

    EXPECT_TRUE(Perception().HasVisibleTarget);
}

TEST_F(PerceptionDetectsTargetViaSceneTickTest, DoesNotSeeNonPerceptibleTarget)
{
    m_Intruder.GetComponent<PerceptibleComponent>().IsPerceptible = false; // cloaked

    RunFrames(1);

    EXPECT_FALSE(Perception().HasVisibleTarget)
        << "Watcher saw a non-perceptible (cloaked) target";
}

TEST_F(PerceptionDetectsTargetViaSceneTickTest, RetainsLastKnownPositionAfterTargetLeavesView)
{
    RunFrames(1);
    ASSERT_TRUE(Perception().HasVisibleTarget);
    const glm::vec3 seenAt = Perception().LastKnownPosition;

    // Target slips behind the watcher; it can no longer be seen this tick.
    m_Intruder.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 8.0f };
    RunFrames(1);

    const auto& pc = Perception();
    EXPECT_FALSE(pc.HasVisibleTarget);
    EXPECT_TRUE(pc.HasLastKnownPosition)
        << "last-known-position memory was wiped when the target left view";
    EXPECT_FLOAT_EQ(pc.LastKnownPosition.z, seenAt.z); // still the -5 sighting
    EXPECT_GT(pc.TimeSinceLastSeen, 0.0f);
}
