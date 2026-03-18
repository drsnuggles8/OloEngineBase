#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Animation/BlendNode.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace OloEngine
{
    class AnimationStateMachine : public RefCounted
    {
      public:
        void AddState(const AnimationState& state);
        void RemoveState(const std::string& stateName);
        void SetDefaultState(const std::string& stateName);
        void AddTransition(const AnimationTransition& transition);

        void Start(const AnimationParameterSet& params);
        void Update(f32 dt, AnimationParameterSet& params,
                    sizet boneCount,
                    std::vector<BoneTransform>& outBoneTransforms);

        [[nodiscard]] const std::string& GetCurrentStateName() const
        {
            return m_CurrentState;
        }
        [[nodiscard]] bool IsInTransition() const
        {
            return m_InTransition;
        }
        [[nodiscard]] bool HasStarted() const
        {
            return m_Started;
        }
        [[nodiscard]] f32 GetCurrentStateNormalizedTime() const;
        [[nodiscard]] const AnimationState* GetState(const std::string& name) const;
        [[nodiscard]] const std::unordered_map<std::string, AnimationState>& GetStates() const
        {
            return m_States;
        }
        [[nodiscard]] const std::vector<AnimationTransition>& GetTransitions() const
        {
            return m_Transitions;
        }
        [[nodiscard]] const std::string& GetDefaultState() const
        {
            return m_DefaultState;
        }

      private:
        void CheckTransitions(AnimationParameterSet& params, sizet boneCount);
        void StartTransition(const AnimationTransition& transition, AnimationParameterSet& params);

        std::unordered_map<std::string, AnimationState> m_States;
        std::vector<AnimationTransition> m_Transitions;
        std::string m_CurrentState;
        std::string m_DefaultState;

        // Transition blending state
        bool m_Started = false;
        bool m_InTransition = false;
        std::string m_TransitionTargetState;
        f32 m_TransitionBlendFactor = 0.0f;
        f32 m_TransitionDuration = 0.0f;
        f32 m_TransitionElapsed = 0.0f;
        f32 m_CurrentStateTime = 0.0f;
        f32 m_TargetStateTime = 0.0f;

        // Cached for GetCurrentStateNormalizedTime
        AnimationParameterSet m_LastParams;
    };
} // namespace OloEngine
