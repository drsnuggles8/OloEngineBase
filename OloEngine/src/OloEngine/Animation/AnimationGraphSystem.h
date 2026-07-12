#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/AnimationGraphComponent.h"
#include "OloEngine/Animation/Skeleton.h"
#include <glm/mat4x4.hpp>

namespace OloEngine
{
    struct IKTargetComponent;
    struct SpringBoneComponent;
    struct NoiseAnimationComponent;
    struct MorphTargetComponent;
    struct FootIKComponent;
    struct FootIKStateComponent;
} // namespace OloEngine

namespace OloEngine::Animation
{
    struct SpringBoneState;
    struct NoiseAnimationState;

    class AnimationGraphSystem
    {
      public:
        // entityWorldTransform converts IK targets from world space to model space.
        // noise/noiseState enable the procedural noise post-pass (runs *before*
        // IK); springBone/springBoneState enable the spring-bone post-pass (runs
        // after IK); footIK/footIKState enable the ground-adaptation pass (runs
        // between IK and spring bones — issue #631 part 3). Each pair must be
        // non-null for its pass to run.
        // morphTarget, when non-null, receives morph-target (blend-shape) weights
        // sampled from the active clip(s) of the graph this frame; CPU evaluation
        // and mesh deformation happen later in the global morph pass.
        static void Update(
            AnimationGraphComponent& graphComp,
            Skeleton& skeleton,
            f32 deltaTime,
            const IKTargetComponent* ikTarget = nullptr,
            const glm::mat4& entityWorldTransform = glm::mat4(1.0f),
            const SpringBoneComponent* springBone = nullptr,
            SpringBoneState* springBoneState = nullptr,
            const NoiseAnimationComponent* noise = nullptr,
            NoiseAnimationState* noiseState = nullptr,
            MorphTargetComponent* morphTarget = nullptr,
            const FootIKComponent* footIK = nullptr,
            FootIKStateComponent* footIKState = nullptr);
    };
} // namespace OloEngine::Animation
