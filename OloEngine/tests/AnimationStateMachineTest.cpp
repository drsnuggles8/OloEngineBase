#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Animation/BlendNode.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/AnimationGraph.h"
#include "OloEngine/Animation/AnimationLayer.h"
#include "Animation/AnimationTestHelpers.h"

using namespace OloEngine;

// Single-bone sampling context: bone 0 is named "Bone0" (matching CreateTestClip's
// channel), so by-name mapping resolves to the historical index-0 behaviour. The
// channel-order regression for #543 lives in Animation/AnimationGraphBoneMappingTest.cpp.
using OloEngine::AnimTest::s_Bone0Ctx;

// Helper to create a simple animation clip with one bone
static Ref<AnimationClip> CreateTestClip(const std::string& name, float duration,
                                         const glm::vec3& startPos = glm::vec3(0.0f),
                                         const glm::vec3& endPos = glm::vec3(1.0f, 0.0f, 0.0f))
{
    auto clip = Ref<AnimationClip>::Create();
    clip->Name = name;
    clip->Duration = duration;

    BoneAnimation boneAnim;
    boneAnim.BoneName = "Bone0";
    boneAnim.PositionKeys.push_back({ 0.0, startPos });
    boneAnim.PositionKeys.push_back({ static_cast<f64>(duration), endPos });
    boneAnim.RotationKeys.push_back({ 0.0, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    boneAnim.RotationKeys.push_back({ static_cast<f64>(duration), glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
    boneAnim.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
    boneAnim.ScaleKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f) });
    clip->BoneAnimations.push_back(boneAnim);
    clip->InitializeBoneCache();

    return clip;
}

//==============================================================================
// Parameter Tests
//==============================================================================

TEST(AnimationStateMachineTest, ParameterSetAndGet)
{
    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.0f);
    params.DefineBool("IsGrounded", true);
    params.DefineInt("State", 0);
    params.DefineTrigger("Jump");

    EXPECT_TRUE(params.HasParameter("Speed"));
    EXPECT_TRUE(params.HasParameter("IsGrounded"));
    EXPECT_TRUE(params.HasParameter("State"));
    EXPECT_TRUE(params.HasParameter("Jump"));
    EXPECT_FALSE(params.HasParameter("NonExistent"));

    params.SetFloat("Speed", 1.5f);
    EXPECT_FLOAT_EQ(params.GetFloat("Speed"), 1.5f);

    params.SetBool("IsGrounded", false);
    EXPECT_FALSE(params.GetBool("IsGrounded"));

    params.SetInt("State", 42);
    EXPECT_EQ(params.GetInt("State"), 42);
}

TEST(AnimationStateMachineTest, TriggerConsumption)
{
    AnimationParameterSet params;
    params.DefineTrigger("Jump");

    EXPECT_FALSE(params.IsTriggerSet("Jump"));

    params.SetTrigger("Jump");
    EXPECT_TRUE(params.IsTriggerSet("Jump"));

    params.ConsumeTrigger("Jump");
    EXPECT_FALSE(params.IsTriggerSet("Jump"));
}

//==============================================================================
// Transition Condition Tests
//==============================================================================

TEST(AnimationStateMachineTest, TransitionConditionFloat)
{
    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.5f);

    TransitionCondition cond;
    cond.ParameterName = "Speed";
    cond.Op = TransitionCondition::Comparison::Greater;
    cond.FloatThreshold = 0.1f;

    EXPECT_TRUE(cond.Evaluate(params));

    params.SetFloat("Speed", 0.05f);
    EXPECT_FALSE(cond.Evaluate(params));
}

TEST(AnimationStateMachineTest, TransitionConditionBool)
{
    AnimationParameterSet params;
    params.DefineBool("IsGrounded", true);

    TransitionCondition cond;
    cond.ParameterName = "IsGrounded";
    cond.Op = TransitionCondition::Comparison::Equal;
    cond.BoolValue = true;

    EXPECT_TRUE(cond.Evaluate(params));

    params.SetBool("IsGrounded", false);
    EXPECT_FALSE(cond.Evaluate(params));
}

TEST(AnimationStateMachineTest, TransitionConditionTrigger)
{
    AnimationParameterSet params;
    params.DefineTrigger("Jump");

    TransitionCondition cond;
    cond.ParameterName = "Jump";
    cond.Op = TransitionCondition::Comparison::TriggerSet;

    EXPECT_FALSE(cond.Evaluate(params));

    params.SetTrigger("Jump");
    EXPECT_TRUE(cond.Evaluate(params));
}

TEST(AnimationStateMachineTest, TransitionANDLogic)
{
    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.5f);
    params.DefineBool("IsGrounded", true);

    AnimationTransition transition;
    transition.SourceState = "Idle";
    transition.DestinationState = "Walk";

    TransitionCondition cond1;
    cond1.ParameterName = "Speed";
    cond1.Op = TransitionCondition::Comparison::Greater;
    cond1.FloatThreshold = 0.1f;

    TransitionCondition cond2;
    cond2.ParameterName = "IsGrounded";
    cond2.Op = TransitionCondition::Comparison::Equal;
    cond2.BoolValue = true;

    transition.Conditions.push_back(cond1);
    transition.Conditions.push_back(cond2);

    // Both true
    EXPECT_TRUE(transition.Evaluate(params));

    // One false
    params.SetBool("IsGrounded", false);
    EXPECT_FALSE(transition.Evaluate(params));
}

//==============================================================================
// State Machine Tests
//==============================================================================

TEST(AnimationStateMachineTest, BasicTransition)
{
    auto idleClip = CreateTestClip("Idle", 1.0f);
    auto walkClip = CreateTestClip("Walk", 1.0f);

    AnimationStateMachine sm;

    AnimationState idleState;
    idleState.Name = "Idle";
    idleState.Clip = idleClip;
    sm.AddState(idleState);

    AnimationState walkState;
    walkState.Name = "Walk";
    walkState.Clip = walkClip;
    sm.AddState(walkState);

    sm.SetDefaultState("Idle");

    AnimationTransition transition;
    transition.SourceState = "Idle";
    transition.DestinationState = "Walk";
    transition.BlendDuration = 0.2f;
    TransitionCondition cond;
    cond.ParameterName = "Speed";
    cond.Op = TransitionCondition::Comparison::Greater;
    cond.FloatThreshold = 0.1f;
    transition.Conditions.push_back(cond);
    sm.AddTransition(transition);

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.0f);

    sm.Start(params);
    EXPECT_EQ(sm.GetCurrentStateName(), "Idle");

    // Speed below threshold - should stay in Idle
    std::vector<BoneTransform> bones;
    sm.Update(0.1f, params, 1, s_Bone0Ctx, bones);
    EXPECT_EQ(sm.GetCurrentStateName(), "Idle");
    EXPECT_FALSE(sm.IsInTransition());

    // Set speed above threshold
    params.SetFloat("Speed", 0.5f);
    sm.Update(0.1f, params, 1, s_Bone0Ctx, bones);
    EXPECT_TRUE(sm.IsInTransition());

    // Complete the transition
    sm.Update(0.3f, params, 1, s_Bone0Ctx, bones);
    EXPECT_EQ(sm.GetCurrentStateName(), "Walk");
    EXPECT_FALSE(sm.IsInTransition());
}

TEST(AnimationStateMachineTest, AnyStateTransition)
{
    auto idleClip = CreateTestClip("Idle", 1.0f);
    auto walkClip = CreateTestClip("Walk", 1.0f);
    auto jumpClip = CreateTestClip("Jump", 0.5f);

    AnimationStateMachine sm;

    AnimationState idleState;
    idleState.Name = "Idle";
    idleState.Clip = idleClip;
    sm.AddState(idleState);

    AnimationState walkState;
    walkState.Name = "Walk";
    walkState.Clip = walkClip;
    sm.AddState(walkState);

    AnimationState jumpState;
    jumpState.Name = "Jump";
    jumpState.Clip = jumpClip;
    jumpState.Looping = false;
    sm.AddState(jumpState);

    sm.SetDefaultState("Idle");

    // Any State -> Jump (trigger)
    AnimationTransition anyToJump;
    anyToJump.SourceState = ""; // Any state
    anyToJump.DestinationState = "Jump";
    anyToJump.BlendDuration = 0.1f;
    TransitionCondition triggerCond;
    triggerCond.ParameterName = "Jump";
    triggerCond.Op = TransitionCondition::Comparison::TriggerSet;
    anyToJump.Conditions.push_back(triggerCond);
    sm.AddTransition(anyToJump);

    AnimationParameterSet params;
    params.DefineTrigger("Jump");

    sm.Start(params);
    EXPECT_EQ(sm.GetCurrentStateName(), "Idle");

    // Trigger jump from any state
    params.SetTrigger("Jump");
    std::vector<BoneTransform> bones;
    sm.Update(0.01f, params, 1, s_Bone0Ctx, bones);
    EXPECT_TRUE(sm.IsInTransition());

    // Complete transition
    sm.Update(0.15f, params, 1, s_Bone0Ctx, bones);
    EXPECT_EQ(sm.GetCurrentStateName(), "Jump");

    // Trigger should have been consumed
    EXPECT_FALSE(params.IsTriggerSet("Jump"));
}

TEST(AnimationStateMachineTest, ExitTimeTransition)
{
    auto clip = CreateTestClip("Attack", 1.0f);

    AnimationStateMachine sm;

    AnimationState attackState;
    attackState.Name = "Attack";
    attackState.Clip = clip;
    attackState.Looping = false;
    sm.AddState(attackState);

    AnimationState idleState;
    idleState.Name = "Idle";
    idleState.Clip = CreateTestClip("Idle", 1.0f);
    sm.AddState(idleState);

    sm.SetDefaultState("Attack");

    // Attack -> Idle at 80% through
    AnimationTransition exitTransition;
    exitTransition.SourceState = "Attack";
    exitTransition.DestinationState = "Idle";
    exitTransition.HasExitTime = true;
    exitTransition.ExitTime = 0.8f;
    exitTransition.BlendDuration = 0.2f;
    // No conditions - just exit time
    sm.AddTransition(exitTransition);

    AnimationParameterSet params;
    sm.Start(params);

    std::vector<BoneTransform> bones;

    // Before exit time
    sm.Update(0.5f, params, 1, s_Bone0Ctx, bones);
    EXPECT_EQ(sm.GetCurrentStateName(), "Attack");
    EXPECT_FALSE(sm.IsInTransition());

    // At exit time (0.8)
    sm.Update(0.35f, params, 1, s_Bone0Ctx, bones);
    EXPECT_TRUE(sm.IsInTransition());
}

TEST(AnimationStateMachineTest, CrossFadeBlending)
{
    // Idle starts at x=0, Walk starts at x=10 — makes blended result verifiable
    auto idleClip = CreateTestClip("Idle", 1.0f, glm::vec3(0.0f), glm::vec3(0.0f));
    auto walkClip = CreateTestClip("Walk", 1.0f, glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(10.0f, 0.0f, 0.0f));

    AnimationStateMachine sm;

    AnimationState idleState;
    idleState.Name = "Idle";
    idleState.Clip = idleClip;
    sm.AddState(idleState);

    AnimationState walkState;
    walkState.Name = "Walk";
    walkState.Clip = walkClip;
    sm.AddState(walkState);

    sm.SetDefaultState("Idle");

    AnimationTransition transition;
    transition.SourceState = "Idle";
    transition.DestinationState = "Walk";
    transition.BlendDuration = 0.5f;
    TransitionCondition cond;
    cond.ParameterName = "Speed";
    cond.Op = TransitionCondition::Comparison::Greater;
    cond.FloatThreshold = 0.1f;
    transition.Conditions.push_back(cond);
    sm.AddTransition(transition);

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.5f);

    sm.Start(params);

    std::vector<BoneTransform> bones;

    // Trigger transition
    sm.Update(0.01f, params, 1, s_Bone0Ctx, bones);
    EXPECT_TRUE(sm.IsInTransition());
    EXPECT_EQ(bones.size(), 1u);

    // Mid-transition - should produce blended bone transforms
    sm.Update(0.25f, params, 1, s_Bone0Ctx, bones);
    EXPECT_TRUE(sm.IsInTransition());
    EXPECT_EQ(bones.size(), 1u);
    // Blended between idle (x=0) and walk (x=10) — result should be between 0 and 10
    EXPECT_GT(bones[0].Translation.x, 0.0f);
    EXPECT_LT(bones[0].Translation.x, 10.0f);

    // Complete transition
    sm.Update(0.5f, params, 1, s_Bone0Ctx, bones);
    EXPECT_EQ(sm.GetCurrentStateName(), "Walk");
    EXPECT_FALSE(sm.IsInTransition());
}

//==============================================================================
// Scratch-reuse regression tests (issue #445).
//
// AnimationStateMachine::Update and AnimationGraph::Update used to build
// std::vector<BoneTransform> cross-fade / layer-accumulation scratch as
// function-local variables, heap-allocating on every ticked transition frame
// (state machine) or every layer of every tick (graph). Both now reuse
// persistent per-instance scratch members instead. These tests exercise the
// specific hazard that reuse introduces: stale data from a PREVIOUS
// transition/layer bleeding into the current one because the buffer was
// never cleared, only resized. Every Evaluate() path fully overwrites
// [0, boneCount) rather than only some indices, but this test would break if
// that invariant is ever violated for a reused buffer, whereas the old
// fresh-locals code masked the bug entirely (an under-filled local vector is
// still empty/default rather than stale).
//==============================================================================

TEST(AnimationStateMachineTest, SequentialCrossFadesDoNotLeakScratchBetweenTransitions)
{
    // Idle(x=0) -> Walk(x=10) -> Run(x=20): each clip is distinguishable so a
    // stale carry-over from the FIRST transition's scratch would show up as a
    // wrong blended value in the SECOND transition.
    auto idleClip = CreateTestClip("Idle", 1.0f, glm::vec3(0.0f), glm::vec3(0.0f));
    auto walkClip = CreateTestClip("Walk", 1.0f, glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    auto runClip = CreateTestClip("Run", 1.0f, glm::vec3(20.0f, 0.0f, 0.0f), glm::vec3(20.0f, 0.0f, 0.0f));

    AnimationStateMachine sm;

    AnimationState idleState;
    idleState.Name = "Idle";
    idleState.Clip = idleClip;
    sm.AddState(idleState);

    AnimationState walkState;
    walkState.Name = "Walk";
    walkState.Clip = walkClip;
    sm.AddState(walkState);

    AnimationState runState;
    runState.Name = "Run";
    runState.Clip = runClip;
    sm.AddState(runState);

    sm.SetDefaultState("Idle");

    AnimationTransition idleToWalk;
    idleToWalk.SourceState = "Idle";
    idleToWalk.DestinationState = "Walk";
    idleToWalk.BlendDuration = 0.2f;
    TransitionCondition walkCond;
    walkCond.ParameterName = "Speed";
    walkCond.Op = TransitionCondition::Comparison::Equal;
    walkCond.FloatThreshold = 1.0f;
    idleToWalk.Conditions.push_back(walkCond);
    sm.AddTransition(idleToWalk);

    AnimationTransition walkToRun;
    walkToRun.SourceState = "Walk";
    walkToRun.DestinationState = "Run";
    walkToRun.BlendDuration = 0.2f;
    TransitionCondition runCond;
    runCond.ParameterName = "Speed";
    runCond.Op = TransitionCondition::Comparison::Equal;
    runCond.FloatThreshold = 2.0f;
    walkToRun.Conditions.push_back(runCond);
    sm.AddTransition(walkToRun);

    AnimationParameterSet params;
    params.DefineFloat("Speed", 0.0f);
    sm.Start(params);

    std::vector<BoneTransform> bones;

    // First transition: Idle -> Walk. Complete it fully.
    params.SetFloat("Speed", 1.0f);
    sm.Update(0.01f, params, 1, s_Bone0Ctx, bones);
    ASSERT_TRUE(sm.IsInTransition());
    sm.Update(0.3f, params, 1, s_Bone0Ctx, bones);
    ASSERT_EQ(sm.GetCurrentStateName(), "Walk");
    ASSERT_FALSE(sm.IsInTransition());
    EXPECT_NEAR(bones[0].Translation.x, 10.0f, 1e-3f) << "settled Walk pose is wrong before the second transition";

    // Second transition: Walk -> Run, reusing the same scratch buffers the
    // first transition used.
    params.SetFloat("Speed", 2.0f);
    sm.Update(0.01f, params, 1, s_Bone0Ctx, bones);
    ASSERT_TRUE(sm.IsInTransition());

    // Mid-blend: result must be strictly between Walk (10) and Run (20) — a
    // stale Idle (0) leaking in from the first transition's scratch would
    // pull this below 10 or otherwise off the Walk/Run blend line.
    sm.Update(0.1f, params, 1, s_Bone0Ctx, bones);
    EXPECT_GT(bones[0].Translation.x, 10.0f) << "second transition's blend was polluted by stale scratch data";
    EXPECT_LT(bones[0].Translation.x, 20.0f) << "second transition's blend was polluted by stale scratch data";

    sm.Update(0.3f, params, 1, s_Bone0Ctx, bones);
    EXPECT_EQ(sm.GetCurrentStateName(), "Run");
    EXPECT_FALSE(sm.IsInTransition());
    EXPECT_NEAR(bones[0].Translation.x, 20.0f, 1e-3f) << "settled Run pose is wrong after the second transition";
}

// AnimationGraph::Update reuses one persistent accumulation buffer and one
// persistent per-layer buffer across ALL layers of a tick, and across every
// subsequent tick. A two-layer graph with a masked override layer is the
// scenario where a stale layer-scratch value (rather than a freshly-Start()ed
// layer stream) would show up as wrong output on the un-masked bone.
TEST(AnimationStateMachineTest, MultiLayerGraphUpdateReusesScratchAcrossLayersAndTicksWithoutBleed)
{
    // Two-bone names: Bone0 (base-only) and Bone1 (overridden by layer 2).
    // AnimationGraph::Update builds its own PoseEvalContext internally.
    const std::vector<std::string> boneNames = { "Bone0", "Bone1" };

    auto baseClip = Ref<AnimationClip>::Create();
    baseClip->Name = "Base";
    baseClip->Duration = 1.0f;
    {
        BoneAnimation b0;
        b0.BoneName = "Bone0";
        b0.PositionKeys.push_back({ 0.0, glm::vec3(1.0f, 0.0f, 0.0f) });
        b0.PositionKeys.push_back({ 1.0, glm::vec3(1.0f, 0.0f, 0.0f) });
        baseClip->BoneAnimations.push_back(b0);

        BoneAnimation b1;
        b1.BoneName = "Bone1";
        b1.PositionKeys.push_back({ 0.0, glm::vec3(2.0f, 0.0f, 0.0f) });
        b1.PositionKeys.push_back({ 1.0, glm::vec3(2.0f, 0.0f, 0.0f) });
        baseClip->BoneAnimations.push_back(b1);
    }
    baseClip->InitializeBoneCache();

    auto overrideClip = Ref<AnimationClip>::Create();
    overrideClip->Name = "Override";
    overrideClip->Duration = 1.0f;
    {
        BoneAnimation b1;
        b1.BoneName = "Bone1";
        b1.PositionKeys.push_back({ 0.0, glm::vec3(50.0f, 0.0f, 0.0f) });
        b1.PositionKeys.push_back({ 1.0, glm::vec3(50.0f, 0.0f, 0.0f) });
        overrideClip->BoneAnimations.push_back(b1);
    }
    overrideClip->InitializeBoneCache();

    auto graph = Ref<AnimationGraph>::Create();

    AnimationLayer baseLayer;
    baseLayer.Name = "Base";
    baseLayer.Weight = 1.0f;
    baseLayer.StateMachine = Ref<AnimationStateMachine>::Create();
    AnimationState baseState;
    baseState.Name = "Base";
    baseState.Clip = baseClip;
    baseLayer.StateMachine->AddState(baseState);
    baseLayer.StateMachine->SetDefaultState("Base");
    graph->Layers.push_back(std::move(baseLayer));

    AnimationLayer overrideLayer;
    overrideLayer.Name = "Override";
    overrideLayer.Weight = 1.0f;
    overrideLayer.Mode = AnimationLayer::BlendMode::Override;
    overrideLayer.AffectedBones = { "Bone1" }; // only overrides Bone1
    overrideLayer.StateMachine = Ref<AnimationStateMachine>::Create();
    AnimationState overrideState;
    overrideState.Name = "Override";
    overrideState.Clip = overrideClip;
    overrideLayer.StateMachine->AddState(overrideState);
    overrideLayer.StateMachine->SetDefaultState("Override");
    graph->Layers.push_back(std::move(overrideLayer));

    graph->Start();

    std::vector<glm::mat4> finalBones;
    std::vector<i32> parentIndices = { -1, -1 };
    std::vector<BoneTransform> bindPose; // empty: identity fallback

    auto ExtractX = [](const glm::mat4& m)
    { return m[3][0]; };

    // Tick several times; the base layer's Bone0 (untouched by the override
    // layer) and the override layer's Bone1 must both stay correct on EVERY
    // tick, proving neither the accumulation nor the per-layer scratch
    // buffer carries stale data forward.
    for (int i = 0; i < 5; ++i)
    {
        graph->Update(0.016f, 2, finalBones, boneNames, parentIndices, bindPose);
        ASSERT_EQ(finalBones.size(), 2u);
        EXPECT_NEAR(ExtractX(finalBones[0]), 1.0f, 1e-3f) << "tick " << i << ": base-only Bone0 corrupted";
        EXPECT_NEAR(ExtractX(finalBones[1]), 50.0f, 1e-3f) << "tick " << i << ": override-layer Bone1 corrupted";
    }
}
