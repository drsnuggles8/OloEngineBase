#include "OloEnginePCH.h"

// =============================================================================
// GoapAgentPlansViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AISystem × GoapAgentComponent × GoapAgent (planner +
//   execution). The GoapTest unit suite verifies the planner and the agent's
//   plan/execute/replan loop in isolation; AISystem::OnUpdate is what actually
//   drives a GoapAgent from a real `Scene::OnUpdateRuntime` call. A regression
//   in the Scene→AISystem→GOAP wiring (e.g. the GoapAgent tick loop dropped
//   from AISystem::OnUpdate, or Enabled gating it off) silently freezes every
//   GOAP character in the project. No unit test sees that.
//
// Scenario: an entity carrying a GoapAgentComponent whose RuntimeAgent must
// chain GatherWood → MakeFire to reach the "HasFire" goal. After a few Scene
// ticks the agent's world state should satisfy the goal and the MakeFire
// action's side-effect counter should have fired — proving the brain actually
// planned and executed from inside the scene update.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/AI/AIComponents.h"
#include "OloEngine/AI/GOAP/GoapAction.h"
#include "OloEngine/AI/GOAP/GoapAgent.h"
#include "OloEngine/AI/GOAP/GoapGoal.h"
#include "OloEngine/Scene/Entity.h"

#include <memory>

using namespace OloEngine;
using namespace OloEngine::Functional;

class GoapAgentPlansViaSceneTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Agent = GetScene().CreateEntity("Planner");

        // Counter captured by value (shared_ptr) so the action closures own a
        // stable handle regardless of fixture/scene teardown order.
        m_FireLit = std::make_shared<i32>(0);

        auto agent = Ref<GoapAgent>::Create();

        GoapAction gatherWood;
        gatherWood.Name = "GatherWood";
        gatherWood.Cost = 1.0f;
        gatherWood.Effects.Set("hasWood", true);

        GoapAction makeFire;
        makeFire.Name = "MakeFire";
        makeFire.Cost = 1.0f;
        makeFire.Preconditions.Set("hasWood", true);
        makeFire.Effects.Set("hasFire", true);
        makeFire.Perform = [counter = m_FireLit](f32) -> GoapActionStatus
        {
            ++(*counter);
            return GoapActionStatus::Success;
        };

        agent->AddAction(std::move(gatherWood));
        agent->AddAction(std::move(makeFire));

        GoapGoal goal;
        goal.Name = "LightAFire";
        goal.DesiredState.Set("hasFire", true);
        agent->AddGoal(std::move(goal));

        GoapAgentComponent gac;
        gac.RuntimeAgent = std::move(agent);
        m_Agent.AddComponent<GoapAgentComponent>(std::move(gac));
    }

    Entity m_Agent;
    std::shared_ptr<i32> m_FireLit;
};

TEST_F(GoapAgentPlansViaSceneTickTest, AgentPlansAndExecutesToGoalAcrossSceneTicks)
{
    // Before any tick: no plan executed, no fire.
    EXPECT_EQ(*m_FireLit, 0);
    {
        const auto& gac = m_Agent.GetComponent<GoapAgentComponent>();
        ASSERT_NE(gac.RuntimeAgent, nullptr);
        ASSERT_FALSE(gac.RuntimeAgent->WorldState().Satisfies([]
                                                              {
            GoapWorldState s; s.Set("hasFire", true); return s; }()));
    }

    // GatherWood then MakeFire each complete in one tick; a couple extra frames
    // give the Scene→AISystem→GOAP path room without being timing-sensitive.
    RunFrames(/*count=*/5);

    const auto& gac = m_Agent.GetComponent<GoapAgentComponent>();
    ASSERT_NE(gac.RuntimeAgent, nullptr);

    GoapWorldState goal;
    goal.Set("hasFire", true);
    EXPECT_TRUE(gac.RuntimeAgent->WorldState().Satisfies(goal))
        << "GOAP agent did not reach its goal — Scene→AISystem→GOAP wiring is broken";
    EXPECT_GE(gac.RuntimeAgent->GoalsAchieved(), 1u);
    EXPECT_EQ(*m_FireLit, 1) << "MakeFire executed an unexpected number of times";
}
