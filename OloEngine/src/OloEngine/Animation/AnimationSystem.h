#pragma once

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include <glm/mat4x4.hpp>
#include <vector>

namespace OloEngine
{
    struct IKTargetComponent;
    struct SpringBoneComponent;
} // namespace OloEngine

namespace OloEngine::Animation
{
    struct SpringBoneState;

    // AnimationSystem: Updates animation state and computes bone transforms for animated entities.
    class AnimationSystem
    {
      public:
        // Call once per frame to update all animated entities.
        // entityWorldTransform is the entity's scene transform — used to convert
        // IK targets from world space to the model space expected by solvers.
        // springBone/springBoneState enable the spring-bone post-pass (runs
        // after IK); both must be non-null for the pass to run.
        static void Update(
            AnimationStateComponent& animState,
            Skeleton& skeleton,
            f32 deltaTime,
            const IKTargetComponent* ikTarget = nullptr,
            const glm::mat4& entityWorldTransform = glm::mat4(1.0f),
            const SpringBoneComponent* springBone = nullptr,
            SpringBoneState* springBoneState = nullptr);
    };
} // namespace OloEngine::Animation
