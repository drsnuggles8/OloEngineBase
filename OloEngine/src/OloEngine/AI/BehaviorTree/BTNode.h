#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <string>
#include <vector>

namespace OloEngine
{
    class BTBlackboard;
    class Entity;

    enum class BTStatus : u8
    {
        Running,
        Success,
        Failure
    };

    class BTNode : public RefCounted
    {
      public:
        ~BTNode() override = default;

        virtual BTStatus Tick(f32 dt, BTBlackboard& blackboard, Entity entity) = 0;
        virtual void Reset() {}

        std::string Name;
        std::vector<Ref<BTNode>> Children;
    };
} // namespace OloEngine
