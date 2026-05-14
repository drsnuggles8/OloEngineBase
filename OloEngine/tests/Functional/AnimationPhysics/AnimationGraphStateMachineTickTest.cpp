#include "OloEnginePCH.h"

// =============================================================================
// AnimationGraphStateMachineTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × AnimationGraphSystem::Update × AnimationStateMachine ×
//   AnimationGraphComponent. The animation-graph path is parallel to (and
//   newer than) the simpler AnimationStateComponent path. A regression
//   that drops AnimationGraphSystem::Update from OnUpdateRuntime, or
//   fails to call StateMachine::Start when the graph hasn't been started,
//   leaves every graph-driven character frozen in their default pose.
//
// Scenario: an entity with SkeletonComponent + AnimationGraphComponent
// (programmatic graph carrying one state with a known clip). After
// ticking, assert (a) the state machine has been started, (b) the
// current state is the one we set as default. This validates the
// per-tick "ensure graph is started" branch in OnUpdateRuntime.
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

class AnimationGraphStateMachineTickTest : public FunctionalTest
{
  protected:
    static constexpr const char* kStateName = "Functional_Idle";

    void BuildScene() override
    {
        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(Fixtures::MakeSingleBoneSkeleton());

        // Programmatic graph: one layer, one state machine, one state
        // (SingleClip). The clip animates the "Root" bone.
        AnimationState state;
        state.Name = kStateName;
        state.Type = AnimationState::MotionType::SingleClip;
        state.Clip = Fixtures::MakeTranslationClip(/*duration=*/1.0f);
        state.Looping = true;

        auto stateMachine = Ref<AnimationStateMachine>::Create();
        stateMachine->AddState(state);
        stateMachine->SetDefaultState(kStateName);

        auto graph = Ref<AnimationGraph>::Create();
        AnimationLayer layer;
        layer.Name = "Base";
        layer.StateMachine = stateMachine;
        graph->Layers.push_back(std::move(layer));

        AnimationGraphComponent graphComp;
        graphComp.RuntimeGraph = graph;
        m_Animated.AddComponent<AnimationGraphComponent>(std::move(graphComp));
    }

    Entity m_Animated;
};

TEST_F(AnimationGraphStateMachineTickTest, RuntimeGraphIsStartedAndCurrentStateMatchesDefaultAfterTick)
{
    // Sanity: graph is attached but state machine hasn't been started yet —
    // OnUpdateRuntime is responsible for calling Start on first tick.
    auto& graphComp = m_Animated.GetComponent<AnimationGraphComponent>();
    ASSERT_TRUE(graphComp.RuntimeGraph);
    ASSERT_FALSE(graphComp.RuntimeGraph->Layers.empty());
    ASSERT_TRUE(graphComp.RuntimeGraph->Layers[0].StateMachine);
    EXPECT_FALSE(graphComp.RuntimeGraph->Layers[0].StateMachine->HasStarted())
        << "state machine was started before the first tick — Start() should be deferred";

    // Tick once — OnUpdateRuntime should call StateMachine::Start.
    RunFrames(1);

    EXPECT_TRUE(graphComp.RuntimeGraph->Layers[0].StateMachine->HasStarted())
        << "state machine never got started — OnUpdateRuntime's animation-graph "
           "Start branch did not fire. AnimationGraphSystem::Update was either "
           "skipped or doesn't drive the start-of-graph path.";

    EXPECT_EQ(graphComp.RuntimeGraph->Layers[0].StateMachine->GetCurrentStateName(), kStateName)
        << "state machine's current state is not the default we set — "
           "Start() did not adopt the SetDefaultState() target.";

    // Tick more frames; current state must remain stable (no transitions
    // configured, looping single clip).
    RunFrames(30);
    EXPECT_EQ(graphComp.RuntimeGraph->Layers[0].StateMachine->GetCurrentStateName(), kStateName)
        << "current state changed without any transition configured.";
    EXPECT_FALSE(graphComp.RuntimeGraph->Layers[0].StateMachine->IsInTransition())
        << "state machine entered a transition without one being configured.";
}
