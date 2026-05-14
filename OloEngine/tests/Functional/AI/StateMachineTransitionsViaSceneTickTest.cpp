#include "OloEnginePCH.h"

// =============================================================================
// StateMachineTransitionsViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AISystem::OnUpdate × StateMachineComponent.RuntimeFSM ×
//   FSMState.OnUpdate × FSMTransition.Condition. Scene::OnUpdateRuntime
//   forwards into AISystem::OnUpdate, which for each entity with a
//   StateMachineComponent calls RuntimeFSM->Start (once) then
//   RuntimeFSM->Update per tick. A state's OnUpdate sets a blackboard
//   value; a transition condition reads that value and flips to the
//   target state. A regression that stops invoking Start/Update, or one
//   that swaps the blackboard reference, leaves the FSM frozen in the
//   initial state forever.
//
// Scenario: Idle state runs OnUpdate, sets `seen_enemy = true` after
// ~0.3s of simulated time. A transition condition reads that flag and
// routes Idle → Chase. After 0.5s of ticks (well past the trigger),
// CurrentState must be "Chase".
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/FSM/StateMachine.h"
#include "OloEngine/AI/FSM/State.h"
#include "OloEngine/AI/FSM/Transition.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Idle state: accumulates dt into the blackboard so the transition
    // condition can observe simulated time.
    class IdleState : public FSMState
    {
      public:
        void OnUpdate(Entity entity, BTBlackboard& blackboard, f32 dt) override
        {
            (void)entity;
            f32 elapsed = blackboard.Get<f32>("idle_elapsed", 0.0f);
            elapsed += dt;
            blackboard.Set("idle_elapsed", elapsed);
            if (elapsed >= 0.3f)
            {
                blackboard.Set("seen_enemy", true);
            }
        }
    };

    class ChaseState : public FSMState
    {
        // No-op — we only care that we reached this state.
    };
} // namespace

class StateMachineTransitionsViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_AI = GetScene().CreateEntity("Guard");
        auto& smc = m_AI.AddComponent<StateMachineComponent>();

        auto fsm = Ref<StateMachine>::Create();

        auto idle = Ref<IdleState>::Create();
        idle->ID = "Idle";
        fsm->AddState(idle);

        auto chase = Ref<ChaseState>::Create();
        chase->ID = "Chase";
        fsm->AddState(chase);

        FSMTransition t;
        t.FromState = "Idle";
        t.ToState = "Chase";
        t.Condition = [](Entity, const BTBlackboard& bb) {
            return bb.Get<bool>("seen_enemy", false);
        };
        fsm->AddTransition(t);

        fsm->SetInitialState("Idle");
        smc.RuntimeFSM = fsm;
    }

    Entity m_AI;
};

TEST_F(StateMachineTransitionsViaSceneTickTest, FsmStartsInIdleAndTransitionsToChaseAfterConditionMet)
{
    auto& smc = m_AI.GetComponent<StateMachineComponent>();
    ASSERT_TRUE(smc.RuntimeFSM);
    EXPECT_FALSE(smc.RuntimeFSM->IsStarted())
        << "FSM was started before any Scene tick — AISystem isn't deferring Start to OnUpdate.";

    // Tick well past 0.3s so the Idle state's OnUpdate sees enough elapsed.
    TickFor(/*seconds=*/0.5f);

    EXPECT_TRUE(smc.RuntimeFSM->IsStarted())
        << "FSM never started — Scene::OnUpdateRuntime is not driving AISystem::OnUpdate.";
    EXPECT_EQ(smc.RuntimeFSM->GetCurrentStateID(), std::string("Chase"))
        << "FSM did not transition Idle → Chase despite 'seen_enemy' being set after 0.3s. "
           "Either FSMTransition.Condition isn't being evaluated, or the blackboard "
           "the OnUpdate wrote to differs from the one the condition reads.";

    // Sanity: blackboard reflects what Idle wrote before the transition.
    EXPECT_TRUE(smc.Blackboard.Get<bool>("seen_enemy", false))
        << "blackboard.seen_enemy is missing — Idle state never ran OnUpdate, "
           "or AISystem swapped blackboards mid-tick.";
}
