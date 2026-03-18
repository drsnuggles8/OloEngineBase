#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/Skeleton.h"

namespace OloEngine::Animation
{
    class AnimationGraphSystem
    {
      public:
        static void Update(
            AnimationGraphComponent& graphComp,
            Skeleton& skeleton,
            f32 deltaTime);
    };
} // namespace OloEngine::Animation
