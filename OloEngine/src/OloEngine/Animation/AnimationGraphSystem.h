#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/Skeleton.h"
#include <glm/mat4x4.hpp>

namespace OloEngine
{
    struct IKTargetComponent;
}

namespace OloEngine::Animation
{
    class AnimationGraphSystem
    {
      public:
        // entityWorldTransform converts IK targets from world space to model space.
        static void Update(
            AnimationGraphComponent& graphComp,
            Skeleton& skeleton,
            f32 deltaTime,
            const IKTargetComponent* ikTarget = nullptr,
            const glm::mat4& entityWorldTransform = glm::mat4(1.0f));
    };
} // namespace OloEngine::Animation
