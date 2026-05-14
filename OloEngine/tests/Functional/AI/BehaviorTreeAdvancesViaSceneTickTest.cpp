#include "OloEnginePCH.h"

// =============================================================================
// BehaviorTreeAdvancesViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AISystem × BehaviorTreeComponent × BTBlackboard. The
//   BehaviorTreeTest unit suite verifies individual nodes in isolation;
//   AISystem::OnUpdate is what actually drives the tree from a real
//   `Scene::OnUpdateRuntime` call. A regression in the Scene→AISystem wiring
//   (e.g. AISystem::OnUpdate dropped from OnUpdateRuntime, or the registry
//   view it iterates rebuilt under a wrong filter) silently stops every
//   AI character in the project. No per-node test sees that.
//
// Scenario: an entity carrying a BehaviorTreeComponent whose RuntimeTree's
// root is a `BTSetBlackboardValue` task. After a single Scene tick, the
// tree's BB should contain the value the task wrote — proving the tree
// actually ticked from inside the scene update.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/BehaviorTree/BehaviorTree.h"
#include "OloEngine/AI/BehaviorTree/BTTasks.h"

#include <string>

using namespace OloEngine;
using namespace OloEngine::Functional;

class BehaviorTreeAdvancesViaSceneTickTest : public FunctionalTest
{
  protected:
    static constexpr const char* kKey = "Functional_TickedFlag";

    void BuildScene() override
    {
        m_Agent = GetScene().CreateEntity("Agent");

        BehaviorTreeComponent bt;
        bt.IsRunning = true;
        bt.RuntimeTree = Ref<BehaviorTree>::Create();

        auto setNode = Ref<BTSetBlackboardValue>::Create();
        setNode->Key = kKey;
        setNode->ValueToSet = i32{ 42 };
        bt.RuntimeTree->SetRoot(setNode);

        // std::move is the canonical way to hand a programmatic component to
        // AddComponent. The defaulted move-ctor on BehaviorTreeComponent
        // preserves RuntimeTree across this transfer.
        m_Agent.AddComponent<BehaviorTreeComponent>(std::move(bt));
    }

    Entity m_Agent;
};

TEST_F(BehaviorTreeAdvancesViaSceneTickTest, RootTaskRunsAndWritesBlackboardAfterOneTick)
{
    // Sanity: blackboard is empty before the first tick.
    {
        const auto& bt = m_Agent.GetComponent<BehaviorTreeComponent>();
        ASSERT_FALSE(bt.Blackboard.Has(kKey))
            << "blackboard already has the key before any tick — test setup is wrong";
    }

    // A single OnUpdateRuntime should be enough — BTSetBlackboardValue
    // returns Success on its first tick.
    RunFrames(/*count=*/1);

    const auto& bt = m_Agent.GetComponent<BehaviorTreeComponent>();
    ASSERT_TRUE(bt.Blackboard.Has(kKey))
        << "BehaviorTree did not tick from inside the Scene update — Scene→AISystem wiring is broken";

    EXPECT_EQ(bt.Blackboard.Get<i32>(kKey), 42)
        << "BehaviorTree ticked but the task didn't write the expected value";
}
