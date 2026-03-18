#include "OloEnginePCH.h"
#include "AISystem.h"
#include "AIComponents.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{
	void AISystem::OnUpdate(Scene* scene, f32 dt)
	{
		// Tick all BehaviorTreeComponents
		{
			auto btView = scene->GetAllEntitiesWith<BehaviorTreeComponent>();
			for (auto entityId : btView)
			{
				auto& bt = btView.get<BehaviorTreeComponent>(entityId);
				if (bt.RuntimeTree)
				{
					Entity entity{ entityId, scene };
					bt.RuntimeTree->Tick(dt, bt.Blackboard, entity);
				}
			}
		}

		// Tick all StateMachineComponents
		{
			auto fsmView = scene->GetAllEntitiesWith<StateMachineComponent>();
			for (auto entityId : fsmView)
			{
				auto& fsm = fsmView.get<StateMachineComponent>(entityId);
				if (fsm.RuntimeFSM)
				{
					Entity entity{ entityId, scene };
					if (!fsm.RuntimeFSM->IsStarted())
					{
						fsm.RuntimeFSM->Start(entity, fsm.Blackboard);
					}
					fsm.RuntimeFSM->Update(entity, fsm.Blackboard, dt);
				}
			}
		}
	}
} // namespace OloEngine
