#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/mat4x4.hpp>

namespace OloEngine
{
    struct IKTargetComponent;
    class Skeleton;
} // namespace OloEngine

namespace OloEngine::Animation
{
    // Applies AimIK and LimbIK post-pass to a skeleton's local transforms.
    // Shared implementation called by both AnimationSystem and AnimationGraphSystem.
    void ApplyIKPostPass(
        Skeleton& skeleton,
        sizet boneCount,
        const IKTargetComponent& ikTarget,
        const glm::mat4& entityWorldTransform);
} // namespace OloEngine::Animation
