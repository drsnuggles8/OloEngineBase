#include "OloEnginePCH.h"

// =============================================================================
// AnimationGraphMultipleEntitiesAdvanceIndependentlyTest — Functional Test.
//
// Cross-subsystem seam under test:
//   AnimationGraphSystem::Update across N entities × per-entity
//   AnimationGraphComponent (RuntimeGraph + Parameters round-trip) ×
//   the parameter swap-in/swap-out in OnUpdate (graphComp.Parameters
//   → RuntimeGraph->Parameters before Update, and back out after).
//   Each entity must own its own RuntimeGraph instance so per-entity
//   Parameters don't trample each other. The earlier
//   AnimationGraphStateMachineTickTest covered a single entity; this
//   one pins the multi-entity invariant.
//
// Scenario: two entities, each with its OWN Ref<AnimationGraph>
// (different state machines so each owns independent state). After
// ticking, each entity's per-entity component must show:
//   - StateMachine.HasStarted() = true
//   - CurrentStateName matches the entity-specific default state
// If RuntimeGraphs were shared, the second entity's tick would have
// overwritten the first's, and the asserts would converge on the same
// state name.
// =============================================================================

#include "Functional/FunctionalTest.h"
#include "Functional/Helpers/AnimationFixtures.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationLayer.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Build a graph carrying one layer / one state machine / one named state.
    // Each call returns a fresh Ref so two callers get independent graphs.
    Ref<AnimationGraph> MakeGraphWithSingleState(const std::string& stateName)
    {
        AnimationState state;
        state.Name = stateName;
        state.Type = AnimationState::MotionType::SingleClip;
        state.Clip = Fixtures::MakeTranslationClip(/*duration=*/1.0f);
        state.Looping = true;

        auto stateMachine = Ref<AnimationStateMachine>::Create();
        stateMachine->AddState(state);
        stateMachine->SetDefaultState(stateName);

        auto graph = Ref<AnimationGraph>::Create();
        AnimationLayer layer;
        layer.Name = "Base";
        layer.StateMachine = stateMachine;
        graph->Layers.push_back(std::move(layer));
        return graph;
    }
} // namespace

class AnimationGraphMultipleEntitiesAdvanceIndependentlyTest : public FunctionalTest
{
  protected:
    static constexpr const char* kFirstStateName = "FirstIdle";
    static constexpr const char* kSecondStateName = "SecondIdle";

    void BuildScene() override
    {
        m_First = GetScene().CreateEntity("First");
        m_First.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());
        AnimationGraphComponent gc1;
        gc1.RuntimeGraph = MakeGraphWithSingleState(kFirstStateName);
        m_First.AddComponent<AnimationGraphComponent>(std::move(gc1));

        m_Second = GetScene().CreateEntity("Second");
        m_Second.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());
        AnimationGraphComponent gc2;
        gc2.RuntimeGraph = MakeGraphWithSingleState(kSecondStateName);
        m_Second.AddComponent<AnimationGraphComponent>(std::move(gc2));
    }

    Entity m_First;
    Entity m_Second;
};

TEST_F(AnimationGraphMultipleEntitiesAdvanceIndependentlyTest, EachEntityKeepsItsOwnStateMachineState)
{
    auto& firstGc = m_First.GetComponent<AnimationGraphComponent>();
    auto& secondGc = m_Second.GetComponent<AnimationGraphComponent>();
    ASSERT_NE(firstGc.RuntimeGraph.Raw(), secondGc.RuntimeGraph.Raw())
        << "the two entities ended up sharing the same RuntimeGraph instance — "
           "test setup is broken or AddComponent copied through a Ref-sharing path.";
    ASSERT_FALSE(firstGc.RuntimeGraph->Layers[0].StateMachine->HasStarted());
    ASSERT_FALSE(secondGc.RuntimeGraph->Layers[0].StateMachine->HasStarted());

    RunFrames(2);

    const auto* firstSM = firstGc.RuntimeGraph->Layers[0].StateMachine.Raw();
    const auto* secondSM = secondGc.RuntimeGraph->Layers[0].StateMachine.Raw();

    EXPECT_TRUE(firstSM->HasStarted());
    EXPECT_TRUE(secondSM->HasStarted());
    EXPECT_EQ(firstSM->GetCurrentStateName(), std::string(kFirstStateName))
        << "first entity's state machine doesn't report its own default state — "
           "AnimationGraphSystem may be writing one entity's parameters into "
           "another's RuntimeGraph (parameter swap-in/swap-out crossed wires).";
    EXPECT_EQ(secondSM->GetCurrentStateName(), std::string(kSecondStateName))
        << "second entity's state machine doesn't report its own default state.";
    EXPECT_NE(firstSM->GetCurrentStateName(), secondSM->GetCurrentStateName())
        << "both entities converged on the same state name — they're effectively "
           "sharing state machine state across the per-entity boundary.";
}
