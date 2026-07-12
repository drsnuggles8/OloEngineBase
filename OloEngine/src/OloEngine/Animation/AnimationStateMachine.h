#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Animation/AnimationState.h"
#include "OloEngine/Animation/AnimationTransition.h"
#include "OloEngine/Animation/AnimationParameter.h"
#include "OloEngine/Animation/BlendNode.h"
#include "OloEngine/Animation/RootMotion.h"
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
                    sizet boneCount, const PoseEvalContext& ctx,
                    std::vector<BoneTransform>& outBoneTransforms);

        [[nodiscard("state name needed for UI or logic")]] const std::string& GetCurrentStateName() const
        {
            return m_CurrentState;
        }
        [[nodiscard("transition check needed for blend decisions")]] bool IsInTransition() const
        {
            return m_InTransition;
        }
        [[nodiscard("started check determines if machine has been initialized")]] bool HasStarted() const
        {
            return m_Started;
        }
        [[nodiscard("normalized time needed for exit-time transitions")]] f32 GetCurrentStateNormalizedTime() const;

        // Root-motion delta the last Update() extracted (entity/model space,
        // already masked per contributing clip). During a cross-fade the source
        // and target contributions blend with the transition factor — exactly
        // like the pose. Zero motion when no active clip extracts (issue #631).
        [[nodiscard("extracted delta must be used")]] const Animation::RootMotionDelta& GetLastRootMotion() const
        {
            return m_LastRootMotion;
        }
        [[nodiscard("state pointer needed for inspection")]] const AnimationState* GetState(const std::string& name) const;
        [[nodiscard("mutable state pointer needed for editing")]] AnimationState* GetMutableState(const std::string& name);
        [[nodiscard("states map needed for enumeration")]] const std::unordered_map<std::string, AnimationState>& GetStates() const
        {
            return m_States;
        }
        [[nodiscard("mutable states map needed for editing")]] std::unordered_map<std::string, AnimationState>& GetMutableStates()
        {
            return m_States;
        }
        [[nodiscard("transitions list needed for enumeration")]] const std::vector<AnimationTransition>& GetTransitions() const
        {
            return m_Transitions;
        }
        [[nodiscard("default state name needed for initialization")]] const std::string& GetDefaultState() const
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

        // Root-motion delta extracted by the last Update() (see GetLastRootMotion)
        Animation::RootMotionDelta m_LastRootMotion;

        // Cross-fade scratch, reused across Update() calls while m_InTransition
        // is true (issue #445) — Evaluate() always resize()s + fully overwrites
        // these, so reusing them avoids a heap allocation on every ticked frame
        // of every transition instead of just the steady (non-transitioning) case.
        std::vector<BoneTransform> m_ScratchCurrentTransforms;
        std::vector<BoneTransform> m_ScratchTargetTransforms;
    };
} // namespace OloEngine
