#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/mat4x4.hpp>

namespace OloEngine
{
    struct SpringBoneComponent;
    class Skeleton;
} // namespace OloEngine

namespace OloEngine::Animation
{
    struct SpringBoneState;

    // Applies the spring-bone post-pass to a skeleton's local transforms.
    // Runs after the IK post-pass, before forward kinematics. Shared
    // implementation called by both AnimationSystem and AnimationGraphSystem.
    // The simulation runs in model space; entityWorldTransform is only used
    // to rotate the component's world-space gravity into model space.
    void ApplySpringBonePostPass(
        Skeleton& skeleton,
        const SpringBoneComponent& springBone,
        SpringBoneState& state,
        const glm::mat4& entityWorldTransform,
        f32 deltaTime);
} // namespace OloEngine::Animation
