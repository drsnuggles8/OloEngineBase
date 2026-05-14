#include "OloEnginePCH.h"

// =============================================================================
// BehaviorTreeSequenceShortCircuitsOnFailureTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AISystem × BehaviorTreeComponent × BTSequence × BT child
//   ticking order. The Sequence composite must:
//     - Tick children left-to-right.
//     - Return Failure on the first child that fails.
//     - NOT tick subsequent siblings after a failure.
//   The previous BehaviorTreeAdvancesViaSceneTickTest pinned the
//   "ticks from inside Scene update" wiring. This test pins the
//   short-circuit semantics. A regression that keeps ticking past a
//   Failed child silently makes "do A, only-if-A-succeeds do B"
//   patterns ignore preconditions.
//
// Scenario: tree =
//   Sequence(
//     Set("ran_first", true),       // Success — runs.
//     CheckBlackboardKey("missing"),// Failure — short-circuit.
//     Set("ran_third", true)        // Should NEVER run.
//   )
// After one tick:
//   - ran_first present and true (first child fired)
//   - ran_third NOT present (sequence stopped at the second child)
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/BehaviorTree/BehaviorTree.h"
#include "OloEngine/AI/BehaviorTree/BTComposites.h"
#include "OloEngine/AI/BehaviorTree/BTTasks.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class BehaviorTreeSequenceShortCircuitsOnFailureTest : public FunctionalTest
{
  protected:
    static constexpr const char* kFirstKey  = "ran_first";
    static constexpr const char* kThirdKey  = "ran_third";
    static constexpr const char* kMissingKey = "this_key_is_absent";

    void BuildScene() override
    {
        m_Agent = GetScene().CreateEntity("Agent");

        BehaviorTreeComponent bt;
        bt.IsRunning = true;
        bt.RuntimeTree = Ref<BehaviorTree>::Create();

        auto seq = Ref<BTSequence>::Create();

        auto first = Ref<BTSetBlackboardValue>::Create();
        first->Key = kFirstKey;
        first->ValueToSet = true;
        seq->Children.push_back(first);

        auto failer = Ref<BTCheckBlackboardKey>::Create();
        failer->Key = kMissingKey; // not present → Failure
        seq->Children.push_back(failer);

        auto third = Ref<BTSetBlackboardValue>::Create();
        third->Key = kThirdKey;
        third->ValueToSet = true;
        seq->Children.push_back(third);

        bt.RuntimeTree->SetRoot(seq);
        m_Agent.AddComponent<BehaviorTreeComponent>(std::move(bt));
    }

    Entity m_Agent;
};

TEST_F(BehaviorTreeSequenceShortCircuitsOnFailureTest, FirstChildRunsThirdChildSkippedAfterMiddleFails)
{
    RunFrames(1);

    const auto& bt = m_Agent.GetComponent<BehaviorTreeComponent>();
    EXPECT_TRUE(bt.Blackboard.Has(kFirstKey))
        << "first sequence child didn't run — Sequence never ticked the first "
           "child, or the BT didn't tick at all.";
    EXPECT_TRUE(bt.Blackboard.Get<bool>(kFirstKey, false));

    EXPECT_FALSE(bt.Blackboard.Has(kThirdKey))
        << "third sequence child ran despite the second returning Failure — "
           "BTSequence::Tick is not short-circuiting on the first failure; "
           "it's iterating all children unconditionally.";
}
