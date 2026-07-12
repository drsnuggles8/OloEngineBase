#pragma once

#include "OloEngine/Core/Base.h"
#include <glm/mat4x4.hpp>

namespace OloEngine
{
    struct FootIKComponent;
    struct FootIKStateComponent;
    class Skeleton;
} // namespace OloEngine

namespace OloEngine::Animation
{
    // Ground-adaptation foot & hand IK post-pass (issue #631 part 3). Mutates
    // skeleton local transforms only; runs after ApplyIKPostPass and before the
    // spring-bone pass in both animation systems. Consumes the ground cache /
    // resolved hand targets in `state` (filled Scene-side from Jolt raycasts —
    // this pass itself never touches physics, which keeps it unit-testable with
    // injected ground data). Writes back plant locks, pelvis smoothing and the
    // per-foot world-position history into `state`.
    void ApplyFootIKPostPass(
        Skeleton& skeleton,
        const FootIKComponent& footIK,
        FootIKStateComponent& state,
        const glm::mat4& entityWorldTransform,
        f32 deltaTime);
} // namespace OloEngine::Animation
