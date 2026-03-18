#pragma once

#include "OloEngine/AI/BehaviorTree/BTNode.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"

namespace OloEngine
{
    // Runs children left-to-right; fails on first failure, succeeds when all succeed
    class BTSequence : public BTNode
    {
      public:
        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
        void Reset() override;

      private:
        u32 m_CurrentChild = 0;
    };

    // Runs children left-to-right; succeeds on first success, fails when all fail
    class BTSelector : public BTNode
    {
      public:
        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
        void Reset() override;

      private:
        u32 m_CurrentChild = 0;
    };

    // Runs all children simultaneously; configurable success/failure policies
    class BTParallel : public BTNode
    {
      public:
        enum class Policy : u8
        {
            RequireOne,
            RequireAll
        };

        Policy SuccessPolicy = Policy::RequireAll;
        Policy FailurePolicy = Policy::RequireOne;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
        void Reset() override;
    };
} // namespace OloEngine
