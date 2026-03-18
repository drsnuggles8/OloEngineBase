#include "OloEnginePCH.h"
#include "BTDecorators.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
	// --- BTInverter ---

	BTStatus BTInverter::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
	{
		if (Children.empty())
		{
			return BTStatus::Failure;
		}

		BTStatus status = Children[0]->Tick(dt, blackboard, entity);
		if (status == BTStatus::Success)
		{
			return BTStatus::Failure;
		}
		if (status == BTStatus::Failure)
		{
			return BTStatus::Success;
		}
		return BTStatus::Running;
	}

	// --- BTRepeater ---

	BTStatus BTRepeater::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
	{
		if (Children.empty())
		{
			return BTStatus::Failure;
		}

		BTStatus status = Children[0]->Tick(dt, blackboard, entity);

		if (status == BTStatus::Failure && AbortOnFailure)
		{
			m_CurrentIteration = 0;
			return BTStatus::Failure;
		}

		if (status == BTStatus::Running)
		{
			return BTStatus::Running;
		}

		// Child completed (success or failure without abort)
		++m_CurrentIteration;
		Children[0]->Reset();

		if (RepeatCount > 0 && m_CurrentIteration >= RepeatCount)
		{
			m_CurrentIteration = 0;
			return BTStatus::Success;
		}

		return BTStatus::Running;
	}

	void BTRepeater::Reset()
	{
		m_CurrentIteration = 0;
		for (auto& child : Children)
		{
			child->Reset();
		}
	}

	// --- BTCooldown ---

	BTStatus BTCooldown::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
	{
		if (m_IsOnCooldown)
		{
			m_TimeRemaining -= dt;
			if (m_TimeRemaining > 0.0f)
			{
				return BTStatus::Failure;
			}
			m_IsOnCooldown = false;
		}

		if (Children.empty())
		{
			return BTStatus::Failure;
		}

		BTStatus status = Children[0]->Tick(dt, blackboard, entity);
		if (status != BTStatus::Running)
		{
			m_IsOnCooldown = true;
			m_TimeRemaining = CooldownTime;
		}
		return status;
	}

	void BTCooldown::Reset()
	{
		m_TimeRemaining = 0.0f;
		m_IsOnCooldown = false;
		for (auto& child : Children)
		{
			child->Reset();
		}
	}

	// --- BTConditionalGuard ---

	BTStatus BTConditionalGuard::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
	{
		if (!blackboard.Has(BlackboardKey))
		{
			return BTStatus::Failure;
		}

		// Compare the variant values
		auto const& allData = blackboard.GetAll();
		auto it = allData.find(BlackboardKey);
		if (it == allData.end() || it->second != ExpectedValue)
		{
			return BTStatus::Failure;
		}

		if (Children.empty())
		{
			return BTStatus::Success;
		}

		return Children[0]->Tick(dt, blackboard, entity);
	}
} // namespace OloEngine
