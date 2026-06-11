#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/Skeleton.h"
#include <glm/mat4x4.hpp>

namespace OloEngine
{
    struct IKTargetComponent;
    struct SpringBoneComponent;
} // namespace OloEngine

namespace OloEngine::Animation
{
    struct SpringBoneState;

    class AnimationGraphSystem
    {
      public:
        // entityWorldTransform converts IK targets from world space to model space.
        // springBone/springBoneState enable the spring-bone post-pass (runs
        // after IK); both must be non-null for the pass to run.
        static void Update(
            AnimationGraphComponent& graphComp,
            Skeleton& skeleton,
            f32 deltaTime,
            const IKTargetComponent* ikTarget = nullptr,
            const glm::mat4& entityWorldTransform = glm::mat4(1.0f),
            const SpringBoneComponent* springBone = nullptr,
            SpringBoneState* springBoneState = nullptr);
    };
} // namespace OloEngine::Animation
