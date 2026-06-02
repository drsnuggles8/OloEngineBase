#pragma once

#include "OloEngine/AI/BehaviorTree/BehaviorTree.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"
#include "OloEngine/AI/FSM/StateMachine.h"
#include "OloEngine/AI/GOAP/GoapAgent.h"
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

        // Copy = duplicate the static asset reference only; runtime state is
        // rebuilt from the asset later. (Used by entt's emplace_or_replace and
        // by serialization round-trips.)
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

        // Move = transfer ownership including runtime state. Without these,
        // user-defined copy ops disable the implicit move and `std::move(...)`
        // silently falls back to copy-which-clears-runtime — surprising
        // callers that just built a programmatic tree and expected it to
        // survive into the registry.
        BehaviorTreeComponent(BehaviorTreeComponent&&) noexcept = default;
        BehaviorTreeComponent& operator=(BehaviorTreeComponent&&) noexcept = default;
    };

    struct StateMachineComponent
    {
        AssetHandle StateMachineAssetHandle = 0;
        BTBlackboard Blackboard;

        // Runtime (not serialized)
        Ref<StateMachine> RuntimeFSM = nullptr;

        StateMachineComponent() = default;

        // Copy = duplicate static asset reference only (rebuilt from asset
        // later). See BehaviorTreeComponent above for the rationale.
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

        // Move = transfer ownership including runtime state.
        StateMachineComponent(StateMachineComponent&&) noexcept = default;
        StateMachineComponent& operator=(StateMachineComponent&&) noexcept = default;
    };

    // Deliberative GOAP planner attached to an entity. Like the BT/FSM
    // components, the heavy runtime brain (RuntimeAgent: its actions, goals and
    // world state) is built programmatically by gameplay/scripting code and is
    // NOT serialized — only the authored Enabled flag and a script-facing
    // Blackboard persist. AISystem ticks RuntimeAgent each frame when Enabled.
    struct GoapAgentComponent
    {
        bool Enabled = true;
        BTBlackboard Blackboard; // sensor/script bridge, mirrors BT/FSM

        // Runtime (not serialized); rebuilt after load by gameplay code.
        Ref<GoapAgent> RuntimeAgent = nullptr;

        GoapAgentComponent() = default;

        // Copy = duplicate the authored config only; the runtime brain is rebuilt
        // later. See BehaviorTreeComponent above for the rationale.
        GoapAgentComponent(const GoapAgentComponent& other)
            : Enabled(other.Enabled)
        {
        }
        GoapAgentComponent& operator=(const GoapAgentComponent& other)
        {
            if (this != &other)
            {
                Enabled = other.Enabled;
                Blackboard.Clear();
                RuntimeAgent = nullptr;
            }
            return *this;
        }

        // Move = transfer ownership including runtime state.
        GoapAgentComponent(GoapAgentComponent&&) noexcept = default;
        GoapAgentComponent& operator=(GoapAgentComponent&&) noexcept = default;
    };
} // namespace OloEngine
