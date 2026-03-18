#pragma once

#include "OloEngine/AI/BehaviorTree/BTNode.h"
#include "OloEngine/AI/BehaviorTree/BTBlackboard.h"

#include <optional>
#include <string>

namespace OloEngine
{
    // Waits for a specified duration, then returns Success
    class BTWait : public BTNode
    {
      public:
        f32 Duration = 1.0f;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
        void Reset() override;

      private:
        f32 m_Elapsed = 0.0f;
    };

    // Sets a blackboard value and returns Success
    class BTSetBlackboardValue : public BTNode
    {
      public:
        std::string Key;
        BTBlackboard::Value ValueToSet;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
    };

    // Logs a message and returns Success
    class BTLog : public BTNode
    {
      public:
        std::string Message;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
    };

    // Checks if a blackboard key exists (and optionally matches ExpectedValue)
    class BTCheckBlackboardKey : public BTNode
    {
      public:
        std::string Key;
        std::optional<BTBlackboard::Value> ExpectedValue;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
    };

    // Sets target on NavAgentComponent and returns Running/Success/Failure
    class BTMoveTo : public BTNode
    {
      public:
        std::string TargetBlackboardKey;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
    };

    // Triggers an animation state and returns Success
    class BTPlayAnimation : public BTNode
    {
      public:
        std::string AnimationName;

        BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) override;
    };
} // namespace OloEngine
