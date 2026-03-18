#include "OloEnginePCH.h"
#include "BehaviorTree.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
	BTStatus BehaviorTree::Tick(f32 dt, BTBlackboard& blackboard, Entity entity)
	{
		if (!m_Root)
		{
			return BTStatus::Failure;
		}
		return m_Root->Tick(dt, blackboard, entity);
	}
} // namespace OloEngine
