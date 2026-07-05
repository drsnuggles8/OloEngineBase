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
#include "OloEngine/Animation/Skeleton.h"
#include "Animation/AnimationTestHelpers.h"

#include <glm/gtc/matrix_transform.hpp>

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

// =============================================================================
// AnimationGraphBoneMappingTickTest — Functional Test (issue #543).
//
// Drives the graph pose pipeline through the REAL Scene::OnUpdateRuntime ->
// AnimationGraphSystem::Update path (Scene.cpp), which — unlike the unit tests
// that call Evaluate() directly — also exercises the bind-pose decomposition
// AnimationGraphSystem does before handing the graph its PoseEvalContext.
//
// The clip's channels are stored in the OPPOSITE order to the skeleton's bones,
// with only one bone keyed, so the two failure modes the fix targets are both
// observable in the ticked skeleton's local transforms:
//   (1) by-index mapping would swap the two channels onto the wrong bones;
//   (2) an un-keyed bone would collapse to identity instead of its bind pose.
// =============================================================================

namespace
{
    using OloEngine::AnimTest::MakeConstantChannel;

    // Attach a single-state, single-clip graph (one full-weight base layer)
    // that plays `clip` to `entity`.
    void AttachSingleClipGraph(Entity entity, const Ref<AnimationClip>& clip)
    {
        AnimationState state;
        state.Name = "Mapping";
        state.Type = AnimationState::MotionType::SingleClip;
        state.Clip = clip;
        state.Looping = true;

        auto stateMachine = Ref<AnimationStateMachine>::Create();
        stateMachine->AddState(state);
        stateMachine->SetDefaultState("Mapping");

        auto graph = Ref<AnimationGraph>::Create();
        AnimationLayer layer;
        layer.Name = "Base";
        layer.StateMachine = stateMachine;
        graph->Layers.push_back(std::move(layer));

        AnimationGraphComponent graphComp;
        graphComp.RuntimeGraph = graph;
        entity.AddComponent<AnimationGraphComponent>(std::move(graphComp));
    }

    glm::vec3 LocalTranslation(const Ref<Skeleton>& skeleton, sizet bone)
    {
        return glm::vec3(skeleton->m_LocalTransforms[bone][3]);
    }
} // namespace

// Both bones keyed, channels stored in reverse (Child channel first). After a
// real tick each channel must land on its NAMED bone, not its array slot.
class AnimationGraphBoneMappingTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Skeleton = Fixtures::MakeTwoBoneSkeleton(); // bones: Root(0), Child(1)

        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "ReverseOrder";
        clip->Duration = 1.0f;
        clip->BoneAnimations.push_back(MakeConstantChannel("Child", glm::vec3(0.0f, 20.0f, 0.0f)));
        clip->BoneAnimations.push_back(MakeConstantChannel("Root", glm::vec3(10.0f, 0.0f, 0.0f)));
        clip->InitializeBoneCache();

        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(m_Skeleton);
        AttachSingleClipGraph(m_Animated, clip);
    }

    Entity m_Animated;
    Ref<Skeleton> m_Skeleton;
};

TEST_F(AnimationGraphBoneMappingTickTest, ClipChannelsLandOnNamedBonesAfterSceneTick)
{
    RunFrames(3);

    // Root(0) must carry the Root channel (x=10); Child(1) the Child channel
    // (y=20). Under the old by-index fill these were swapped.
    EXPECT_NEAR(LocalTranslation(m_Skeleton, 0).x, 10.0f, 1e-3f) << "Root bone did not get the Root channel";
    EXPECT_NEAR(LocalTranslation(m_Skeleton, 0).y, 0.0f, 1e-3f);
    EXPECT_NEAR(LocalTranslation(m_Skeleton, 1).y, 20.0f, 1e-3f) << "Child bone did not get the Child channel";
    EXPECT_NEAR(LocalTranslation(m_Skeleton, 1).x, 0.0f, 1e-3f);
}

// Only Root is keyed; Child carries a distinctive bind-pose local translation.
// After a tick the un-keyed Child must rest at its bind pose, not identity.
class AnimationGraphUnkeyedBindPoseTickTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        // Two-bone skeleton with a NON-identity Child bind-pose local transform.
        m_Skeleton = Ref<Skeleton>::Create(2);
        m_Skeleton->m_BoneNames = { "Root", "Child" };
        m_Skeleton->m_ParentIndices = { -1, 0 };
        m_Skeleton->m_LocalTransforms = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f)) };
        m_Skeleton->m_BonePreTransforms = { glm::mat4(1.0f), glm::mat4(1.0f) };
        m_Skeleton->m_GlobalTransforms[0] = m_Skeleton->m_LocalTransforms[0];
        m_Skeleton->m_GlobalTransforms[1] = m_Skeleton->m_GlobalTransforms[0] * m_Skeleton->m_LocalTransforms[1];
        m_Skeleton->SetBindPose();

        auto clip = Ref<AnimationClip>::Create();
        clip->Name = "RootOnly";
        clip->Duration = 1.0f;
        clip->BoneAnimations.push_back(MakeConstantChannel("Root", glm::vec3(10.0f, 0.0f, 0.0f)));
        clip->InitializeBoneCache();

        m_Animated = GetScene().CreateEntity("Animated");
        m_Animated.AddComponent<SkeletonComponent>(m_Skeleton);
        AttachSingleClipGraph(m_Animated, clip);
    }

    Entity m_Animated;
    Ref<Skeleton> m_Skeleton;
};

TEST_F(AnimationGraphUnkeyedBindPoseTickTest, UnkeyedBoneRestsAtBindPoseNotIdentity)
{
    RunFrames(3);

    // Root(0) is animated (x=10); Child(1) is un-keyed and must keep its
    // bind-pose local translation (y=5), NOT collapse to identity (y=0).
    EXPECT_NEAR(LocalTranslation(m_Skeleton, 0).x, 10.0f, 1e-3f) << "Root channel not applied";
    EXPECT_NEAR(LocalTranslation(m_Skeleton, 1).y, 5.0f, 1e-3f)
        << "un-keyed Child collapsed to identity instead of falling back to bind pose";
}
