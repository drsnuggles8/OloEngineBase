#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    struct NoiseAnimationComponent;
    class Skeleton;
} // namespace OloEngine

namespace OloEngine::Animation
{
    struct NoiseAnimationState;

    // Applies the procedural noise animator to a skeleton's local transforms.
    // Runs *before* the IK post-pass: the noise produces the organic "intent"
    // pose (breathing / idle sway) that IK then corrects, so end-effector
    // constraints (planted feet, hands on target) stay satisfied while the body
    // sways. Shared implementation called by both AnimationSystem and
    // AnimationGraphSystem. The noise phase is advanced by deltaTime into
    // `state`, making the motion frame-rate independent.
    void ApplyNoisePostPass(
        Skeleton& skeleton,
        const NoiseAnimationComponent& noise,
        NoiseAnimationState& state,
        f32 deltaTime);
} // namespace OloEngine::Animation
