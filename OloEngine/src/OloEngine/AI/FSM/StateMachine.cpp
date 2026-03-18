#include "OloEnginePCH.h"
#include "StateMachine.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
	void StateMachine::AddState(Ref<FSMState> state)
	{
		m_States[state->ID] = std::move(state);
	}

	void StateMachine::AddState(const StateID& id, Ref<FSMState> state)
	{
		state->ID = id;
		m_States[id] = std::move(state);
	}

	void StateMachine::AddTransition(const FSMTransition& transition)
	{
		m_Transitions.push_back(transition);
	}

	void StateMachine::SetInitialState(const StateID& stateId)
	{
		m_InitialState = stateId;
	}

	void StateMachine::Start(Entity entity, BTBlackboard& blackboard)
	{
		if (m_InitialState.empty())
		{
			OLO_CORE_WARN("[FSM] No initial state set");
			return;
		}

		auto it = m_States.find(m_InitialState);
		if (it == m_States.end())
		{
			OLO_CORE_WARN("[FSM] Initial state '{}' not found", m_InitialState);
			return;
		}

		m_CurrentState = m_InitialState;
		m_IsStarted = true;
		it->second->OnEnter(entity, blackboard);
	}

	void StateMachine::Update(Entity entity, BTBlackboard& blackboard, f32 dt)
	{
		if (!m_IsStarted || m_CurrentState.empty())
		{
			return;
		}

		// Check transitions from current state
		for (auto const& transition : m_Transitions)
		{
			if (transition.FromState != m_CurrentState)
			{
				continue;
			}
			if (transition.Condition && transition.Condition(entity, blackboard))
			{
				ForceTransition(transition.ToState, entity, blackboard);
				return;
			}
		}

		// Update current state
		auto it = m_States.find(m_CurrentState);
		if (it != m_States.end())
		{
			it->second->OnUpdate(entity, blackboard, dt);
		}
	}

	void StateMachine::ForceTransition(const StateID& stateId, Entity entity, BTBlackboard& blackboard)
	{
		if (stateId == m_CurrentState)
		{
			return;
		}

		auto newIt = m_States.find(stateId);
		if (newIt == m_States.end())
		{
			OLO_CORE_WARN("[FSM] Target state '{}' not found", stateId);
			return;
		}

		// Exit current state
		if (!m_CurrentState.empty())
		{
			auto curIt = m_States.find(m_CurrentState);
			if (curIt != m_States.end())
			{
				curIt->second->OnExit(entity, blackboard);
			}
		}

		m_CurrentState = stateId;
		newIt->second->OnEnter(entity, blackboard);
	}
} // namespace OloEngine
