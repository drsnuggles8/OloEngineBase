#include <gtest/gtest.h>
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/AI/FSM/State.h"
#include "OloEngine/AI/FSM/Transition.h"
#include "OloEngine/AI/FSM/StateMachine.h"
#include "OloEngine/AI/FSM/StateMachineAsset.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"
#include "OloEngine/AI/AIRegistry.h"

using namespace OloEngine;

// ============================================================================
// Mock states for testing
// ============================================================================

class MockState : public FSMState
{
  public:
    int EnterCount = 0;
    int UpdateCount = 0;
    int ExitCount = 0;

    void OnEnter(Entity /*entity*/, BTBlackboard& /*bb*/) override { ++EnterCount; }
    void OnUpdate(Entity /*entity*/, BTBlackboard& /*bb*/, f32 /*dt*/) override { ++UpdateCount; }
    void OnExit(Entity /*entity*/, BTBlackboard& /*bb*/) override { ++ExitCount; }
};

// ============================================================================
// StateMachine tests
// ============================================================================

class StateMachineTest : public ::testing::Test
{
  protected:
    Ref<StateMachine> fsm;
    BTBlackboard bb;
    Entity entity;

    Ref<MockState> idleState;
    Ref<MockState> walkState;
    Ref<MockState> attackState;

    void SetUp() override
    {
        fsm = Ref<StateMachine>::Create();
        idleState = Ref<MockState>::Create();
        walkState = Ref<MockState>::Create();
        attackState = Ref<MockState>::Create();

        fsm->AddState("Idle", idleState);
        fsm->AddState("Walk", walkState);
        fsm->AddState("Attack", attackState);
    }
};

TEST_F(StateMachineTest, StartCallsOnEnter)
{
    fsm->SetInitialState("Idle");
    fsm->Start(entity, bb);
    EXPECT_TRUE(fsm->IsStarted());
    EXPECT_EQ(fsm->GetCurrentStateID(), "Idle");
    EXPECT_EQ(idleState->EnterCount, 1);
}

TEST_F(StateMachineTest, UpdateCallsOnUpdate)
{
    fsm->SetInitialState("Idle");
    fsm->Start(entity, bb);
    fsm->Update(entity, bb, 0.016f);
    EXPECT_EQ(idleState->UpdateCount, 1);
    fsm->Update(entity, bb, 0.016f);
    EXPECT_EQ(idleState->UpdateCount, 2);
}

TEST_F(StateMachineTest, TransitionChangesState)
{
    bb.Set("shouldWalk", false);
    fsm->SetInitialState("Idle");

    FSMTransition transition;
    transition.FromState = "Idle";
    transition.ToState = "Walk";
    transition.Condition = [](Entity /*entity*/, const BTBlackboard& bb) -> bool
    {
        return bb.Get<bool>("shouldWalk");
    };
    fsm->AddTransition(transition);

    fsm->Start(entity, bb);
    fsm->Update(entity, bb, 0.016f);
    // Condition not met, still Idle
    EXPECT_EQ(fsm->GetCurrentStateID(), "Idle");

    // Now trigger the transition
    bb.Set("shouldWalk", true);
    fsm->Update(entity, bb, 0.016f);
    EXPECT_EQ(fsm->GetCurrentStateID(), "Walk");
    EXPECT_EQ(idleState->ExitCount, 1);
    EXPECT_EQ(walkState->EnterCount, 1);
}

TEST_F(StateMachineTest, ForceTransition)
{
    fsm->SetInitialState("Idle");
    fsm->Start(entity, bb);

    fsm->ForceTransition("Attack", entity, bb);
    EXPECT_EQ(fsm->GetCurrentStateID(), "Attack");
    EXPECT_EQ(idleState->ExitCount, 1);
    EXPECT_EQ(attackState->EnterCount, 1);
}

TEST_F(StateMachineTest, ForceTransition_ToSameState_DoesNothing)
{
    fsm->SetInitialState("Idle");
    fsm->Start(entity, bb);
    fsm->ForceTransition("Idle", entity, bb);
    // Should not re-enter
    EXPECT_EQ(idleState->EnterCount, 1);
    EXPECT_EQ(idleState->ExitCount, 0);
}

TEST_F(StateMachineTest, ForceTransition_InvalidState_DoesNothing)
{
    fsm->SetInitialState("Idle");
    fsm->Start(entity, bb);
    fsm->ForceTransition("NonExistent", entity, bb);
    EXPECT_EQ(fsm->GetCurrentStateID(), "Idle");
}

TEST_F(StateMachineTest, UpdateBeforeStart_DoesNothing)
{
    fsm->SetInitialState("Idle");
    // No Start() called
    fsm->Update(entity, bb, 0.016f);
    EXPECT_EQ(idleState->UpdateCount, 0);
}

TEST_F(StateMachineTest, MultipleTransitions_FirstMatchWins)
{
    bb.Set("priority", i32(0));
    fsm->SetInitialState("Idle");

    FSMTransition toWalk;
    toWalk.FromState = "Idle";
    toWalk.ToState = "Walk";
    toWalk.Condition = [](Entity /*entity*/, const BTBlackboard& bb) -> bool
    {
        return bb.Get<i32>("priority") >= 1;
    };

    FSMTransition toAttack;
    toAttack.FromState = "Idle";
    toAttack.ToState = "Attack";
    toAttack.Condition = [](Entity /*entity*/, const BTBlackboard& bb) -> bool
    {
        return bb.Get<i32>("priority") >= 2;
    };

    fsm->AddTransition(toWalk);
    fsm->AddTransition(toAttack);

    fsm->Start(entity, bb);

    // Both conditions met, but Walk transition was added first
    bb.Set("priority", i32(2));
    fsm->Update(entity, bb, 0.016f);
    EXPECT_EQ(fsm->GetCurrentStateID(), "Walk");
}

// ============================================================================
// StateMachineAsset tests
// ============================================================================

TEST(StateMachineAssetTest, HasCorrectAssetType)
{
    auto asset = Ref<StateMachineAsset>::Create();
    EXPECT_EQ(asset->GetAssetType(), AssetType::StateMachine);
    EXPECT_EQ(StateMachineAsset::GetStaticType(), AssetType::StateMachine);
}

TEST(StateMachineAssetTest, StoresStatesAndTransitions)
{
    auto asset = Ref<StateMachineAsset>::Create();

    FSMStateData state;
    state.ID = "Idle";
    state.TypeName = "IdleState";
    asset->AddState(std::move(state));

    FSMTransitionData trans;
    trans.FromState = "Idle";
    trans.ToState = "Walk";
    trans.ConditionExpression = "shouldWalk == true";
    asset->AddTransition(std::move(trans));

    asset->SetInitialStateID("Idle");

    EXPECT_EQ(asset->GetStates().size(), 1u);
    EXPECT_EQ(asset->GetTransitions().size(), 1u);
    EXPECT_EQ(asset->GetInitialStateID(), "Idle");
}

// ============================================================================
// AIRegistry - FSM State Registry tests
// ============================================================================

TEST(FSMStateRegistryTest, RegisterAndCreate)
{
    FSMStateRegistry::Register("MockState", []() -> Ref<FSMState>
    {
        return Ref<MockState>::Create();
    });

    auto state = FSMStateRegistry::Create("MockState");
    EXPECT_NE(state, nullptr);
    auto* mock = dynamic_cast<MockState*>(state.get());
    EXPECT_NE(mock, nullptr);
}

TEST(FSMStateRegistryTest, UnknownTypeReturnsNull)
{
    auto state = FSMStateRegistry::Create("UnknownStateType");
    EXPECT_EQ(state, nullptr);
}
