#pragma once

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include <glm/mat4x4.hpp>
#include <vector>

namespace OloEngine
{
    struct IKTargetComponent;
    struct SpringBoneComponent;
    struct NoiseAnimationComponent;
    struct FootIKComponent;
    struct FootIKStateComponent;
} // namespace OloEngine

namespace OloEngine::Animation
{
    struct SpringBoneState;
    struct NoiseAnimationState;

    // AnimationSystem: Updates animation state and computes bone transforms for animated entities.
    class AnimationSystem
    {
      public:
        // Call once per frame to update all animated entities.
        // entityWorldTransform is the entity's scene transform — used to convert
        // IK targets from world space to the model space expected by solvers.
        // noise/noiseState enable the procedural noise post-pass (runs *before*
        // IK); springBone/springBoneState enable the spring-bone post-pass (runs
        // after IK); footIK/footIKState enable the ground-adaptation pass (runs
        // between IK and spring bones — issue #631 part 3). Each pair must be
        // non-null for its pass to run.
        static void Update(
            AnimationStateComponent& animState,
            Skeleton& skeleton,
            f32 deltaTime,
            const IKTargetComponent* ikTarget = nullptr,
            const glm::mat4& entityWorldTransform = glm::mat4(1.0f),
            const SpringBoneComponent* springBone = nullptr,
            SpringBoneState* springBoneState = nullptr,
            const NoiseAnimationComponent* noise = nullptr,
            NoiseAnimationState* noiseState = nullptr,
            const FootIKComponent* footIK = nullptr,
            FootIKStateComponent* footIKState = nullptr);
    };
} // namespace OloEngine::Animation
