#include <gtest/gtest.h>

#include "OloEngine/AI/GOAP/GoapAction.h"
#include "OloEngine/AI/GOAP/GoapAgent.h"
#include "OloEngine/AI/GOAP/GoapGoal.h"
#include "OloEngine/AI/GOAP/GoapPlanner.h"
#include "OloEngine/AI/GOAP/GoapWorldState.h"

#include <string>

using namespace OloEngine;

namespace
{
    // Small helpers so the planning scenarios below read declaratively.
    GoapWorldState State(std::initializer_list<std::pair<const char*, GoapWorldState::Value>> facts)
    {
        GoapWorldState s;
        for (const auto& [key, value] : facts)
            s.Set(std::string{ key }, value);
        return s;
    }

    GoapAction Action(const std::string& name, f32 cost, GoapWorldState pre, GoapWorldState eff)
    {
        GoapAction a;
        a.Name = name;
        a.Cost = cost;
        a.Preconditions = std::move(pre);
        a.Effects = std::move(eff);
        return a;
    }

    GoapGoal Goal(const std::string& name, GoapWorldState desired, f32 priority = 1.0f)
    {
        GoapGoal g;
        g.Name = name;
        g.Priority = priority;
        g.DesiredState = std::move(desired);
        return g;
    }
} // namespace

// ============================================================================
// GoapWorldState
// ============================================================================

TEST(GoapWorldState, SetGetHasRemove)
{
    GoapWorldState s;
    EXPECT_FALSE(s.Has("hp"));

    s.Set("hp", 100);
    s.Set("alive", true);
    EXPECT_TRUE(s.Has("hp"));
    EXPECT_EQ(s.GetOr<i32>("hp", -1), 100);
    EXPECT_TRUE(s.GetOr<bool>("alive", false));
    EXPECT_EQ(s.Size(), 2u);

    // Overwrite, not duplicate.
    s.Set("hp", 50);
    EXPECT_EQ(s.Size(), 2u);
    EXPECT_EQ(s.GetOr<i32>("hp", -1), 50);

    s.Remove("hp");
    EXPECT_FALSE(s.Has("hp"));
    EXPECT_EQ(s.Size(), 1u);
}

TEST(GoapWorldState, GetOrFallsBackOnMissingOrWrongType)
{
    GoapWorldState s;
    s.Set("count", 7);
    EXPECT_EQ(s.GetOr<i32>("missing", 99), 99);  // missing key
    EXPECT_FALSE(s.GetOr<bool>("count", false)); // present but wrong type
}

TEST(GoapWorldState, SatisfiesSemantics)
{
    GoapWorldState world = State({ { "hasAxe", true }, { "wood", 3 }, { "atTree", true } });

    EXPECT_TRUE(world.Satisfies(GoapWorldState{}));              // empty == trivially satisfied
    EXPECT_TRUE(world.Satisfies(State({ { "hasAxe", true } }))); // subset
    EXPECT_TRUE(world.Satisfies(State({ { "hasAxe", true }, { "wood", 3 } })));
    EXPECT_FALSE(world.Satisfies(State({ { "hasAxe", false } })));  // value mismatch
    EXPECT_FALSE(world.Satisfies(State({ { "wood", 4 } })));        // value mismatch
    EXPECT_FALSE(world.Satisfies(State({ { "hasSword", true } }))); // missing key
}

TEST(GoapWorldState, SatisfiesDistinguishesBoolAndIntTypes)
{
    // bool(false) and i32(0) must never satisfy one another.
    GoapWorldState world = State({ { "flag", false } });
    EXPECT_FALSE(world.Satisfies(State({ { "flag", 0 } })));
    EXPECT_TRUE(world.Satisfies(State({ { "flag", false } })));
}

TEST(GoapWorldState, ApplyEffectsOverlays)
{
    GoapWorldState s = State({ { "a", 1 }, { "b", true } });
    s.ApplyEffects(State({ { "b", false }, { "c", 9 } }));
    EXPECT_FALSE(s.GetOr<bool>("b", true)); // overwritten
    EXPECT_EQ(s.GetOr<i32>("a", -1), 1);    // untouched
    EXPECT_EQ(s.GetOr<i32>("c", -1), 9);    // added
}

TEST(GoapWorldState, UnsatisfiedCount)
{
    GoapWorldState world = State({ { "a", true }, { "b", true } });
    EXPECT_EQ(world.UnsatisfiedCount(GoapWorldState{}), 0u);
    EXPECT_EQ(world.UnsatisfiedCount(State({ { "a", true } })), 0u);
    EXPECT_EQ(world.UnsatisfiedCount(State({ { "a", false }, { "c", true } })), 2u);
}

TEST(GoapWorldState, EqualityAndHashAreOrderIndependent)
{
    GoapWorldState a;
    a.Set("z", 1);
    a.Set("a", true);
    a.Set("m", 5);

    GoapWorldState b;
    b.Set("m", 5);
    b.Set("a", true);
    b.Set("z", 1);

    EXPECT_EQ(a, b);
    EXPECT_EQ(a.Hash(), b.Hash());
}

TEST(GoapWorldState, HashSeparatesBoolFalseFromIntZero)
{
    GoapWorldState a = State({ { "x", false } });
    GoapWorldState b = State({ { "x", 0 } });
    EXPECT_FALSE(a == b);
    EXPECT_NE(a.Hash(), b.Hash());
}

// ============================================================================
// GoapPlanner
// ============================================================================

TEST(GoapPlanner, GoalAlreadySatisfiedYieldsEmptyFoundPlan)
{
    GoapWorldState start = State({ { "safe", true } });
    GoapGoal goal = Goal("BeSafe", State({ { "safe", true } }));

    GoapPlan plan = GoapPlanner::Plan(start, goal, {});
    EXPECT_TRUE(plan.Found);
    EXPECT_TRUE(plan.Empty());
}

TEST(GoapPlanner, SingleActionPlan)
{
    GoapWorldState start;
    GoapGoal goal = Goal("HasAxe", State({ { "hasAxe", true } }));
    std::vector<GoapAction> actions = {
        Action("GetAxe", 1.0f, GoapWorldState{}, State({ { "hasAxe", true } })),
    };

    GoapPlan plan = GoapPlanner::Plan(start, goal, actions);
    ASSERT_TRUE(plan.Found);
    ASSERT_EQ(plan.Length(), 1u);
    EXPECT_EQ(plan.Steps[0].Name, "GetAxe");
    EXPECT_FLOAT_EQ(plan.TotalCost, 1.0f);
}

TEST(GoapPlanner, MultiStepChainIsOrderedCorrectly)
{
    GoapWorldState start;
    GoapGoal goal = Goal("HasFirewood", State({ { "hasFirewood", true } }));
    std::vector<GoapAction> actions = {
        // Intentionally out of order to prove the planner orders by dependency.
        Action("ChopWood", 1.0f, State({ { "hasAxe", true } }), State({ { "hasFirewood", true } })),
        Action("GetAxe", 1.0f, GoapWorldState{}, State({ { "hasAxe", true } })),
    };

    GoapPlan plan = GoapPlanner::Plan(start, goal, actions);
    ASSERT_TRUE(plan.Found);
    ASSERT_EQ(plan.Length(), 2u);
    EXPECT_EQ(plan.Steps[0].Name, "GetAxe");
    EXPECT_EQ(plan.Steps[1].Name, "ChopWood");
    EXPECT_FLOAT_EQ(plan.TotalCost, 2.0f);
}

TEST(GoapPlanner, UnreachableGoalIsNotFound)
{
    GoapWorldState start;
    GoapGoal goal = Goal("HasFirewood", State({ { "hasFirewood", true } }));
    std::vector<GoapAction> actions = {
        Action("GetAxe", 1.0f, GoapWorldState{}, State({ { "hasAxe", true } })),
    };

    GoapPlan plan = GoapPlanner::Plan(start, goal, actions);
    EXPECT_FALSE(plan.Found);
    EXPECT_TRUE(plan.Empty());
}

TEST(GoapPlanner, ChoosesCheaperOfCompetingPlans)
{
    GoapWorldState start;
    GoapGoal goal = Goal("HasGold", State({ { "hasGold", true } }));
    std::vector<GoapAction> actions = {
        Action("MineGold", 5.0f, GoapWorldState{}, State({ { "hasGold", true } })),
        Action("StealGold", 1.0f, GoapWorldState{}, State({ { "hasGold", true } })),
    };

    GoapPlan plan = GoapPlanner::Plan(start, goal, actions);
    ASSERT_TRUE(plan.Found);
    ASSERT_EQ(plan.Length(), 1u);
    EXPECT_EQ(plan.Steps[0].Name, "StealGold");
    EXPECT_FLOAT_EQ(plan.TotalCost, 1.0f);
}

TEST(GoapPlanner, DijkstraModeFindsMinimumCostMultiStepPlan)
{
    // Two routes to the goal: a single expensive action, or a two-step cheap
    // chain. With HeuristicWeight 0 the planner is provably minimum-cost.
    GoapWorldState start;
    GoapGoal goal = Goal("Done", State({ { "done", true } }));
    std::vector<GoapAction> actions = {
        Action("ExpensiveDirect", 10.0f, GoapWorldState{}, State({ { "done", true } })),
        Action("CheapPrep", 1.0f, GoapWorldState{}, State({ { "prepped", true } })),
        Action("CheapFinish", 1.0f, State({ { "prepped", true } }), State({ { "done", true } })),
    };

    GoapPlannerSettings settings;
    settings.HeuristicWeight = 0.0f;
    GoapPlan plan = GoapPlanner::Plan(start, goal, actions, settings);
    ASSERT_TRUE(plan.Found);
    ASSERT_EQ(plan.Length(), 2u);
    EXPECT_EQ(plan.Steps[0].Name, "CheapPrep");
    EXPECT_EQ(plan.Steps[1].Name, "CheapFinish");
    EXPECT_FLOAT_EQ(plan.TotalCost, 2.0f);
}

TEST(GoapPlanner, IsUsableGateExcludesAction)
{
    GoapWorldState start;
    GoapGoal goal = Goal("HasGold", State({ { "hasGold", true } }));

    GoapAction steal = Action("StealGold", 1.0f, GoapWorldState{}, State({ { "hasGold", true } }));
    steal.IsUsable = []()
    { return false; }; // guards are watching
    std::vector<GoapAction> actions = {
        Action("MineGold", 5.0f, GoapWorldState{}, State({ { "hasGold", true } })),
        steal,
    };

    GoapPlan plan = GoapPlanner::Plan(start, goal, actions);
    ASSERT_TRUE(plan.Found);
    ASSERT_EQ(plan.Length(), 1u);
    EXPECT_EQ(plan.Steps[0].Name, "MineGold");
}

TEST(GoapPlanner, MaxPlanLengthBoundsTheSearch)
{
    GoapWorldState start;
    GoapGoal goal = Goal("HasFirewood", State({ { "hasFirewood", true } }));
    std::vector<GoapAction> actions = {
        Action("GetAxe", 1.0f, GoapWorldState{}, State({ { "hasAxe", true } })),
        Action("ChopWood", 1.0f, State({ { "hasAxe", true } }), State({ { "hasFirewood", true } })),
    };

    GoapPlannerSettings settings;
    settings.MaxPlanLength = 1; // the 2-step chain cannot fit
    GoapPlan plan = GoapPlanner::Plan(start, goal, actions, settings);
    EXPECT_FALSE(plan.Found);
}

TEST(GoapPlanner, IterationCapTerminatesAndReports)
{
    GoapWorldState start;
    GoapGoal goal = Goal("HasFirewood", State({ { "hasFirewood", true } }));
    std::vector<GoapAction> actions = {
        Action("GetAxe", 1.0f, GoapWorldState{}, State({ { "hasAxe", true } })),
        Action("ChopWood", 1.0f, State({ { "hasAxe", true } }), State({ { "hasFirewood", true } })),
    };

    GoapPlannerSettings settings;
    settings.MaxIterations = 0; // expand nothing
    GoapPlanner::Stats stats;
    GoapPlan plan = GoapPlanner::Plan(start, goal, actions, settings, &stats);
    EXPECT_FALSE(plan.Found);
    EXPECT_TRUE(stats.HitIterationCap);
}

TEST(GoapPlanner, NoOpActionsDoNotCauseInfiniteLoop)
{
    // An action whose effects are already true (a no-op cycle) must be pruned
    // by the best-cost check rather than expanding forever.
    GoapWorldState start = State({ { "idle", true } });
    GoapGoal goal = Goal("HasGold", State({ { "hasGold", true } }));
    std::vector<GoapAction> actions = {
        Action("Loiter", 0.0f, State({ { "idle", true } }), State({ { "idle", true } })),
        Action("Work", 1.0f, State({ { "idle", true } }), State({ { "hasGold", true } })),
    };

    GoapPlanner::Stats stats;
    GoapPlan plan = GoapPlanner::Plan(start, goal, actions, {}, &stats);
    ASSERT_TRUE(plan.Found);
    EXPECT_EQ(plan.Steps.back().Name, "Work");
    EXPECT_FALSE(stats.HitIterationCap);
}

TEST(GoapPlanner, ClassicWoodcutterScenario)
{
    // start with nothing → end with firewood delivered.
    GoapWorldState start;
    GoapGoal goal = Goal("DeliverFirewood", State({ { "firewoodDelivered", true } }));
    std::vector<GoapAction> actions = {
        Action("GetAxe", 2.0f, GoapWorldState{}, State({ { "hasAxe", true } })),
        Action("GotoTree", 1.0f, GoapWorldState{}, State({ { "atTree", true } })),
        Action("ChopLog", 3.0f, State({ { "hasAxe", true }, { "atTree", true } }), State({ { "hasLog", true } })),
        Action("Deliver", 1.0f, State({ { "hasLog", true } }), State({ { "firewoodDelivered", true } })),
    };

    GoapPlan plan = GoapPlanner::Plan(start, goal, actions);
    ASSERT_TRUE(plan.Found);
    // Last action must be the delivery; ChopLog must precede it; both
    // prerequisites of ChopLog must precede ChopLog.
    EXPECT_EQ(plan.Steps.back().Name, "Deliver");
    EXPECT_FLOAT_EQ(plan.TotalCost, 7.0f);

    GoapWorldState sim = start;
    for (const auto& step : plan.Steps)
    {
        ASSERT_TRUE(sim.Satisfies(step.Preconditions))
            << "precondition of " << step.Name << " violated mid-plan";
        sim.ApplyEffects(step.Effects);
    }
    EXPECT_TRUE(sim.Satisfies(goal.DesiredState));
}

// ============================================================================
// GoapAgent
// ============================================================================

TEST(GoapAgent, PlansAndExecutesToGoalWithInstantActions)
{
    auto agent = Ref<GoapAgent>::Create();
    agent->AddAction(Action("GetAxe", 1.0f, GoapWorldState{}, State({ { "hasAxe", true } })));
    agent->AddAction(Action("ChopWood", 1.0f, State({ { "hasAxe", true } }), State({ { "hasFirewood", true } })));
    agent->AddGoal(Goal("MakeFirewood", State({ { "hasFirewood", true } })));

    // Each instant action completes in one tick; two steps + the completion tick.
    for (i32 i = 0; i < 5; ++i)
        agent->Update(0.016f);

    EXPECT_TRUE(agent->WorldState().Satisfies(State({ { "hasFirewood", true } })));
    EXPECT_GE(agent->GoalsAchieved(), 1u);
    EXPECT_FALSE(agent->HasPlan()); // nothing left to do
}

TEST(GoapAgent, PicksHighestPriorityRelevantGoal)
{
    auto agent = Ref<GoapAgent>::Create();
    // Flee runs across ticks so the agent is still mid-pursuit when we inspect
    // its chosen goal — an instant action would complete and clear it same-tick.
    GoapAction flee = Action("Flee", 1.0f, GoapWorldState{}, State({ { "safe", true } }));
    flee.Perform = [](f32)
    { return GoapActionStatus::Running; };
    agent->AddAction(Action("Eat", 1.0f, GoapWorldState{}, State({ { "fed", true } })));
    agent->AddAction(flee);
    agent->AddGoal(Goal("Eat", State({ { "fed", true } }), /*priority*/ 1.0f));
    agent->AddGoal(Goal("Survive", State({ { "safe", true } }), /*priority*/ 10.0f));

    agent->Update(0.016f);
    EXPECT_EQ(agent->CurrentGoalName(), "Survive");
}

TEST(GoapAgent, SkipsGoalWhoseRelevanceGateIsClosed)
{
    auto agent = Ref<GoapAgent>::Create();

    // Eat runs across ticks so the chosen goal is still active when inspected.
    GoapAction eatAction = Action("Eat", 1.0f, GoapWorldState{}, State({ { "fed", true } }));
    eatAction.Perform = [](f32)
    { return GoapActionStatus::Running; };
    agent->SetActions({ eatAction, Action("Flee", 1.0f, GoapWorldState{}, State({ { "safe", true } })) });

    GoapGoal eat = Goal("Eat", State({ { "fed", true } }), 1.0f);
    GoapGoal survive = Goal("Survive", State({ { "safe", true } }), 10.0f);
    survive.IsValid = [](const GoapWorldState& s)
    { return s.GetOr<bool>("underThreat", false); };
    agent->AddGoal(eat);
    agent->AddGoal(survive);

    // No threat → the higher-priority Survive goal is irrelevant, Eat is chosen.
    agent->Update(0.016f);
    EXPECT_EQ(agent->CurrentGoalName(), "Eat");
}

TEST(GoapAgent, RunningActionSpansMultipleTicks)
{
    auto agent = Ref<GoapAgent>::Create();

    i32 ticks = 0;
    GoapAction longWalk = Action("Walk", 1.0f, GoapWorldState{}, State({ { "arrived", true } }));
    longWalk.Perform = [&ticks](f32) -> GoapActionStatus
    {
        ++ticks;
        return ticks >= 3 ? GoapActionStatus::Success : GoapActionStatus::Running;
    };
    agent->AddAction(longWalk);
    agent->AddGoal(Goal("Arrive", State({ { "arrived", true } })));

    agent->Update(0.016f); // tick 1 - running
    EXPECT_FALSE(agent->WorldState().Satisfies(State({ { "arrived", true } })));
    agent->Update(0.016f); // tick 2 - running
    agent->Update(0.016f); // tick 3 - success, effects applied
    EXPECT_EQ(ticks, 3);
    EXPECT_TRUE(agent->WorldState().Satisfies(State({ { "arrived", true } })));
}

TEST(GoapAgent, ReplansWhenAnActionFails)
{
    auto agent = Ref<GoapAgent>::Create();

    // Preferred (cheap) action always fails; fallback (expensive) succeeds.
    GoapAction primary = Action("PickLock", 1.0f, GoapWorldState{}, State({ { "doorOpen", true } }));
    primary.Perform = [](f32)
    { return GoapActionStatus::Failure; };
    GoapAction fallback = Action("BreakDoor", 5.0f, GoapWorldState{}, State({ { "doorOpen", true } }));
    // fallback has no Perform → instant success.

    // After PickLock fails the planner is re-run; to make the fallback the only
    // viable choice on replan, disable the lock-pick once it has failed.
    bool lockPickBroken = false;
    primary.IsUsable = [&lockPickBroken]()
    { return !lockPickBroken; };
    primary.OnEnter = [&lockPickBroken]()
    { lockPickBroken = true; };

    agent->AddAction(primary);
    agent->AddAction(fallback);
    agent->AddGoal(Goal("OpenDoor", State({ { "doorOpen", true } })));

    for (i32 i = 0; i < 5; ++i)
        agent->Update(0.016f);

    EXPECT_TRUE(agent->WorldState().Satisfies(State({ { "doorOpen", true } })));
}

TEST(GoapAgent, SensorRefreshesWorldStateBeforePlanning)
{
    auto agent = Ref<GoapAgent>::Create();
    // Drink is only usable while the sensor reports thirst, so the agent can only
    // become hydrated if the sensor's fact is folded in *before* planning.
    agent->AddAction(Action("Drink", 1.0f, State({ { "thirsty", true } }), State({ { "hydrated", true } })));
    agent->AddGoal(Goal("StayHydrated", State({ { "hydrated", true } })));

    bool thirsty = false;
    agent->SetSensor([&thirsty](GoapWorldState& ws)
                     { ws.Set("thirsty", thirsty); });

    // Not thirsty → Drink's precondition is unmet → no plan → not hydrated.
    agent->Update(0.016f);
    EXPECT_FALSE(agent->WorldState().GetOr<bool>("thirsty", true));
    EXPECT_FALSE(agent->WorldState().GetOr<bool>("hydrated", false));

    // Sensor now reports thirst → Drink becomes usable → agent drinks → hydrated.
    thirsty = true;
    agent->Update(0.016f);
    EXPECT_TRUE(agent->WorldState().GetOr<bool>("hydrated", false));
}

TEST(GoapAgent, NoSatisfiableGoalLeavesAgentIdle)
{
    auto agent = Ref<GoapAgent>::Create();
    // Goal requires firewood but no action can produce it.
    agent->AddAction(Action("GetAxe", 1.0f, GoapWorldState{}, State({ { "hasAxe", true } })));
    agent->AddGoal(Goal("MakeFirewood", State({ { "hasFirewood", true } })));

    agent->Update(0.016f);
    EXPECT_FALSE(agent->HasPlan());
    EXPECT_TRUE(agent->CurrentGoalName().empty());
    EXPECT_EQ(agent->GoalsAchieved(), 0u);
}
