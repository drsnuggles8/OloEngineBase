#pragma once

#include "OloEngine/AI/BehaviorTree/BTNode.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"

#include <string>

namespace OloEngine
{
    // Flips Success <-> Failure; Running passes through
    class BTInverter : public BTNode
    {
      public:
        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
    };

    // Repeats child N times or forever (RepeatCount == 0 means infinite)
    class BTRepeater : public BTNode
    {
      public:
        u32 RepeatCount = 0; // 0 = infinite
        bool AbortOnFailure = false;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
        void Reset() override;

      private:
        u32 m_CurrentIteration = 0;
    };

    // Blocks re-entry for CooldownTime seconds after child completes
    class BTCooldown : public BTNode
    {
      public:
        f32 CooldownTime = 1.0f;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
        void Reset() override;

      private:
        f32 m_TimeRemaining = 0.0f;
        bool m_IsOnCooldown = false;
    };

    // Only runs child if blackboard key matches expected value
    class BTConditionalGuard : public BTNode
    {
      public:
        std::string BlackboardKey;
        BTBlackboard::Value ExpectedValue;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
    };
} // namespace OloEngine
