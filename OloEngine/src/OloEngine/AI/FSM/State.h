#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"

#include <string>

namespace OloEngine
{
    class Entity;

    using StateID = std::string;

    class FSMState : public RefCounted
    {
      public:
        ~FSMState() override = default;

        virtual void OnEnter(Entity entity, BTBlackboard& blackboard);
        virtual void OnUpdate(Entity entity, BTBlackboard& blackboard, f32 dt);
        virtual void OnExit(Entity entity, BTBlackboard& blackboard);

        StateID ID;
    };
} // namespace OloEngine
