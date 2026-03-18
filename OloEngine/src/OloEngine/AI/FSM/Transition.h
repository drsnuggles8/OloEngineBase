#pragma once

#include "OloEngine/AI/FSM/State.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"

#include <functional>
#include <string>

namespace OloEngine
{
    class Entity;

    struct FSMTransition
    {
        StateID FromState;
        StateID ToState;
        std::function<bool(Entity, const BTBlackboard&)> Condition;
    };
} // namespace OloEngine
