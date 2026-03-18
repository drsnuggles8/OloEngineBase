#include "OloEnginePCH.h"
#include "OloEngine/Animation/AnimationStateMachine.h"
#include "OloEngine/Core/Log.h"
#include <algorithm>

namespace OloEngine
{
    void AnimationStateMachine::AddState(const AnimationState& state)
    {
        m_States[state.Name] = state;
    }

    void AnimationStateMachine::RemoveState(const std::string& stateName)
    {
        m_States.erase(stateName);

        if (m_DefaultState == stateName)
        {
            m_DefaultState.clear();
        }
        if (m_CurrentState == stateName)
        {
            m_CurrentState.clear();
            m_InTransition = false;
        }
        if (m_TransitionTargetState == stateName)
        {
            m_InTransition = false;
            m_TransitionTargetState.clear();
        }

        std::erase_if(m_Transitions, [&stateName](const AnimationTransition& t)
                      { return t.SourceState == stateName || t.DestinationState == stateName; });
    }

    void AnimationStateMachine::SetDefaultState(const std::string& stateName)
    {
        m_DefaultState = stateName;
    }

    void AnimationStateMachine::AddTransition(const AnimationTransition& transition)
    {
        m_Transitions.push_back(transition);
    }

    void AnimationStateMachine::Start(const AnimationParameterSet& /*params*/)
    {
        m_CurrentState = m_DefaultState;
        m_CurrentStateTime = 0.0f;
        m_InTransition = false;
        m_TransitionBlendFactor = 0.0f;
        m_TransitionElapsed = 0.0f;
        m_TargetStateTime = 0.0f;
        m_Started = true;
    }

    void AnimationStateMachine::Update(f32 dt, AnimationParameterSet& params,
                                       sizet boneCount,
                                       std::vector<BoneTransform>& outBoneTransforms)
    {
        OLO_PROFILE_FUNCTION();

        if (m_CurrentState.empty())
        {
            outBoneTransforms.resize(boneCount);
            return;
        }

        // Cache params for GetCurrentStateNormalizedTime
        m_LastParams = params;

        auto* currentState = GetState(m_CurrentState);
        if (!currentState)
        {
            outBoneTransforms.resize(boneCount);
            return;
        }

        f32 currentDuration = currentState->GetEffectiveDuration(params);

        // Advance time (Speed is already factored into GetEffectiveDuration)
        m_CurrentStateTime += dt;

        // Handle looping
        if (currentState->Looping && currentDuration > 0.0f)
        {
            while (m_CurrentStateTime >= currentDuration)
            {
                m_CurrentStateTime -= currentDuration;
            }
        }

        if (m_InTransition)
        {
            m_TransitionElapsed += dt;
            m_TransitionBlendFactor = (m_TransitionDuration > 0.0f)
                                          ? glm::clamp(m_TransitionElapsed / m_TransitionDuration, 0.0f, 1.0f)
                                          : 1.0f;

            auto* targetState = GetState(m_TransitionTargetState);
            if (targetState)
            {
                m_TargetStateTime += dt;
                f32 targetDuration = targetState->GetEffectiveDuration(params);
                if (targetState->Looping && targetDuration > 0.0f)
                {
                    while (m_TargetStateTime >= targetDuration)
                    {
                        m_TargetStateTime -= targetDuration;
                    }
                }
            }

            if (m_TransitionBlendFactor >= 1.0f)
            {
                // Transition complete
                m_CurrentState = m_TransitionTargetState;
                m_CurrentStateTime = m_TargetStateTime;
                m_InTransition = false;
                m_TransitionBlendFactor = 0.0f;
                m_TransitionElapsed = 0.0f;
                m_TargetStateTime = 0.0f;

                currentState = GetState(m_CurrentState);
                if (currentState)
                {
                    f32 normalizedTime = 0.0f;
                    f32 dur = currentState->GetEffectiveDuration(params);
                    if (dur > 0.0f)
                    {
                        normalizedTime = m_CurrentStateTime / dur;
                    }
                    currentState->Evaluate(normalizedTime, params, boneCount, outBoneTransforms);
                }
                else
                {
                    outBoneTransforms.resize(boneCount);
                }

                // Check for new transitions after completing
                CheckTransitions(params, boneCount);
                return;
            }

            // Cross-fade between current and target states
            std::vector<BoneTransform> currentTransforms;
            std::vector<BoneTransform> targetTransforms;

            {
                f32 normalizedTime = 0.0f;
                if (currentDuration > 0.0f)
                {
                    normalizedTime = m_CurrentStateTime / currentDuration;
                }
                currentState->Evaluate(normalizedTime, params, boneCount, currentTransforms);
            }

            if (targetState)
            {
                f32 targetDuration = targetState->GetEffectiveDuration(params);
                f32 normalizedTime = 0.0f;
                if (targetDuration > 0.0f)
                {
                    normalizedTime = m_TargetStateTime / targetDuration;
                }
                targetState->Evaluate(normalizedTime, params, boneCount, targetTransforms);
            }

            // Blend
            BlendTree::BlendBoneTransforms(currentTransforms, targetTransforms, m_TransitionBlendFactor, outBoneTransforms);
            return;
        }

        // Not in transition - evaluate current state
        f32 normalizedTime = 0.0f;
        if (currentDuration > 0.0f)
        {
            normalizedTime = m_CurrentStateTime / currentDuration;
        }
        currentState->Evaluate(normalizedTime, params, boneCount, outBoneTransforms);

        // Check for transitions
        CheckTransitions(params, boneCount);
    }

    f32 AnimationStateMachine::GetCurrentStateNormalizedTime() const
    {
        auto* state = GetState(m_CurrentState);
        if (!state)
        {
            return 0.0f;
        }

        f32 duration = state->GetEffectiveDuration(m_LastParams);
        if (duration > 0.0f)
        {
            return m_CurrentStateTime / duration;
        }
        return 0.0f;
    }

    const AnimationState* AnimationStateMachine::GetState(const std::string& name) const
    {
        if (auto it = m_States.find(name); it != m_States.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    AnimationState* AnimationStateMachine::GetMutableState(const std::string& name)
    {
        if (auto it = m_States.find(name); it != m_States.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    void AnimationStateMachine::CheckTransitions(AnimationParameterSet& params, sizet /*boneCount*/)
    {
        if (m_InTransition)
        {
            return;
        }

        auto* currentState = GetState(m_CurrentState);
        if (!currentState)
        {
            return;
        }

        f32 currentDuration = currentState->GetEffectiveDuration(params);
        f32 normalizedTime = (currentDuration > 0.0f) ? m_CurrentStateTime / currentDuration : 0.0f;

        for (auto const& transition : m_Transitions)
        {
            // Check source state match
            if (!transition.SourceState.empty() && transition.SourceState != "*" && transition.SourceState != m_CurrentState)
            {
                continue;
            }

            // Check can transition to self
            if (!transition.CanTransitionToSelf && transition.DestinationState == m_CurrentState)
            {
                continue;
            }

            // Check destination state exists
            if (!GetState(transition.DestinationState))
            {
                continue;
            }

            // Check exit time
            if (transition.HasExitTime && normalizedTime < transition.ExitTime)
            {
                continue;
            }

            // Check conditions
            if (transition.Evaluate(params))
            {
                StartTransition(transition, params);
                return;
            }
        }
    }

    void AnimationStateMachine::StartTransition(const AnimationTransition& transition, AnimationParameterSet& params)
    {
        m_InTransition = true;
        m_TransitionTargetState = transition.DestinationState;
        m_TransitionDuration = transition.BlendDuration;
        m_TransitionElapsed = 0.0f;
        m_TransitionBlendFactor = 0.0f;
        m_TargetStateTime = 0.0f;

        // Consume any triggers in the conditions
        for (auto const& condition : transition.Conditions)
        {
            if (condition.Op == TransitionCondition::Comparison::TriggerSet)
            {
                params.ConsumeTrigger(condition.ParameterName);
            }
        }
    }
} // namespace OloEngine
