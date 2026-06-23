#pragma once

#include "OloEngine/AI/BehaviorTree/BTNode.h"

namespace OloEngine
{
    // Condition / guard node that succeeds while the entity's PerceptionComponent
    // reports a currently-visible target. With a child it behaves as a guard —
    // the child ticks only while a target is in sight (patrol → chase). With no
    // child it is a leaf condition returning Success/Failure, usable under a
    // Sequence/Selector. Reads the component's runtime result directly (the same
    // result PerceptionSystem mirrors into the blackboard under
    // PerceptionKeys::CanSeeTarget for scripts / GOAP), so it needs no authored
    // blackboard key.
    class BTCanSeeTarget : public BTNode
    {
      public:
        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
        void Reset() override;
    };
} // namespace OloEngine
