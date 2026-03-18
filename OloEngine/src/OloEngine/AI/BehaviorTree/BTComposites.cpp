#include "OloEnginePCH.h"
#include "BTComposites.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
	// --- BTSequence ---

	BTStatus BTSequence::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
	{
		for (; m_CurrentChild < static_cast<u32>(Children.size()); ++m_CurrentChild)
		{
			BTStatus status = Children[m_CurrentChild]->Tick(dt, blackboard, entity);
			if (status == BTStatus::Running)
			{
				return BTStatus::Running;
			}
			if (status == BTStatus::Failure)
			{
				m_CurrentChild = 0;
				return BTStatus::Failure;
			}
		}
		m_CurrentChild = 0;
		return BTStatus::Success;
	}

	void BTSequence::Reset()
	{
		m_CurrentChild = 0;
		for (auto& child : Children)
		{
			child->Reset();
		}
	}

	// --- BTSelector ---

	BTStatus BTSelector::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
	{
		for (; m_CurrentChild < static_cast<u32>(Children.size()); ++m_CurrentChild)
		{
			BTStatus status = Children[m_CurrentChild]->Tick(dt, blackboard, entity);
			if (status == BTStatus::Running)
			{
				return BTStatus::Running;
			}
			if (status == BTStatus::Success)
			{
				m_CurrentChild = 0;
				return BTStatus::Success;
			}
		}
		m_CurrentChild = 0;
		return BTStatus::Failure;
	}

	void BTSelector::Reset()
	{
		m_CurrentChild = 0;
		for (auto& child : Children)
		{
			child->Reset();
		}
	}

	// --- BTParallel ---

	BTStatus BTParallel::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
	{
		u32 successCount = 0;
		u32 failureCount = 0;

		for (auto& child : Children)
		{
			BTStatus status = child->Tick(dt, blackboard, entity);
			if (status == BTStatus::Success)
			{
				++successCount;
			}
			else if (status == BTStatus::Failure)
			{
				++failureCount;
			}
		}

		if (FailurePolicy == Policy::RequireOne && failureCount > 0)
		{
			return BTStatus::Failure;
		}
		if (FailurePolicy == Policy::RequireAll && failureCount == static_cast<u32>(Children.size()))
		{
			return BTStatus::Failure;
		}
		if (SuccessPolicy == Policy::RequireOne && successCount > 0)
		{
			return BTStatus::Success;
		}
		if (SuccessPolicy == Policy::RequireAll && successCount == static_cast<u32>(Children.size()))
		{
			return BTStatus::Success;
		}

		return BTStatus::Running;
	}

	void BTParallel::Reset()
	{
		for (auto& child : Children)
		{
			child->Reset();
		}
	}
} // namespace OloEngine
