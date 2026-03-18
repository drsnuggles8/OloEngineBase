#pragma once

#include "OloEngine/AI/FSM/State.h"
#include "OloEngine/AI/FSM/Transition.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"
#include "OloEngine/Core/Ref.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class Entity;

    class StateMachine : public RefCounted
    {
      public:
        StateMachine() = default;
        ~StateMachine() override = default;

        void AddState(Ref<FSMState> state);
        void AddState(const StateID& id, Ref<FSMState> state);
        void AddTransition(const FSMTransition& transition);
        void SetInitialState(const StateID& stateId);

        void Start(Entity entity, BTBlackboard& blackboard);
        void Update(Entity entity, BTBlackboard& blackboard, f32 dt);
        void ForceTransition(const StateID& stateId, Entity entity, BTBlackboard& blackboard);

        [[nodiscard]] const StateID& GetCurrentStateID() const
        {
            return m_CurrentState;
        }

        [[nodiscard]] bool IsStarted() const
        {
            return m_IsStarted;
        }

      private:
        std::unordered_map<StateID, Ref<FSMState>> m_States;
        std::vector<FSMTransition> m_Transitions;
        StateID m_CurrentState;
        StateID m_InitialState;
        bool m_IsStarted = false;
    };
} // namespace OloEngine
