#pragma once

#include "OloEngine/AI/BehaviorTree/BehaviorTree.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"
#include "OloEngine/AI/FSM/StateMachine.h"
#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

namespace OloEngine
{
	struct BehaviorTreeComponent
	{
		AssetHandle BehaviorTreeAssetHandle = 0;
		BTBlackboard Blackboard;

		// Runtime (not serialized)
		Ref<BehaviorTree> RuntimeTree = nullptr;
		bool IsRunning = false;

		BehaviorTreeComponent() = default;
		BehaviorTreeComponent(const BehaviorTreeComponent& other)
			: BehaviorTreeAssetHandle(other.BehaviorTreeAssetHandle)
		{
		}
		BehaviorTreeComponent& operator=(const BehaviorTreeComponent& other)
		{
			if (this != &other)
			{
				BehaviorTreeAssetHandle = other.BehaviorTreeAssetHandle;
				Blackboard.Clear();
				RuntimeTree = nullptr;
				IsRunning = false;
			}
			return *this;
		}
	};

	struct StateMachineComponent
	{
		AssetHandle StateMachineAssetHandle = 0;
		BTBlackboard Blackboard;

		// Runtime (not serialized)
		Ref<StateMachine> RuntimeFSM = nullptr;

		StateMachineComponent() = default;
		StateMachineComponent(const StateMachineComponent& other)
			: StateMachineAssetHandle(other.StateMachineAssetHandle)
		{
		}
		StateMachineComponent& operator=(const StateMachineComponent& other)
		{
			if (this != &other)
			{
				StateMachineAssetHandle = other.StateMachineAssetHandle;
				Blackboard.Clear();
				RuntimeFSM = nullptr;
			}
			return *this;
		}
	};
} // namespace OloEngine
