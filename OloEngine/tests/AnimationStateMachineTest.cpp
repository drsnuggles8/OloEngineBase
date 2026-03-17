#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Animation/BlendNode.h"
#include "OloEngine/Animation/AnimationClip.h"

using namespace OloEngine;

// Helper to create a simple animation clip with one bone
static Ref<AnimationClip> CreateTestClip(const std::string& name, float duration)
{
	auto clip = Ref<AnimationClip>::Create();
	clip->Name = name;
	clip->Duration = duration;

	BoneAnimation boneAnim;
	boneAnim.BoneName = "Bone0";
	boneAnim.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
	boneAnim.PositionKeys.push_back({ static_cast<f64>(duration), glm::vec3(1.0f, 0.0f, 0.0f) });
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
	sm.Update(0.1f, params, 1, bones);
	EXPECT_EQ(sm.GetCurrentStateName(), "Idle");
	EXPECT_FALSE(sm.IsInTransition());

	// Set speed above threshold
	params.SetFloat("Speed", 0.5f);
	sm.Update(0.1f, params, 1, bones);
	EXPECT_TRUE(sm.IsInTransition());

	// Complete the transition
	sm.Update(0.3f, params, 1, bones);
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
	sm.Update(0.01f, params, 1, bones);
	EXPECT_TRUE(sm.IsInTransition());

	// Complete transition
	sm.Update(0.15f, params, 1, bones);
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
	sm.Update(0.5f, params, 1, bones);
	EXPECT_EQ(sm.GetCurrentStateName(), "Attack");
	EXPECT_FALSE(sm.IsInTransition());

	// At exit time (0.8)
	sm.Update(0.35f, params, 1, bones);
	EXPECT_TRUE(sm.IsInTransition());
}

TEST(AnimationStateMachineTest, CrossFadeBlending)
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
	sm.Update(0.01f, params, 1, bones);
	EXPECT_TRUE(sm.IsInTransition());
	EXPECT_EQ(bones.size(), 1u);

	// Mid-transition - should produce blended bone transforms
	sm.Update(0.25f, params, 1, bones);
	EXPECT_TRUE(sm.IsInTransition());
	EXPECT_EQ(bones.size(), 1u);

	// Complete transition
	sm.Update(0.5f, params, 1, bones);
	EXPECT_EQ(sm.GetCurrentStateName(), "Walk");
	EXPECT_FALSE(sm.IsInTransition());
}
