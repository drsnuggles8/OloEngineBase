#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    class Scene;
} // namespace OloEngine

namespace OloEngine::Animation
{
    // Velocity-driven locomotion controller pass (issue #631 part 4): for every
    // entity with an enabled LocomotionComponent + AnimationGraphComponent it
    // measures (or takes) the character velocity, smooths speed/turn-rate,
    // selects a gait with hysteresis, writes the graph parameters, and
    // stride-warps the active base-layer state's playback rate. Runs as the
    // "Locomotion" gameplay-scheduler node before the animation systems (RAW
    // edge on the AnimationParams channel).
    class LocomotionSystem
    {
      public:
        static void OnUpdate(Scene* scene, f32 deltaTime);
    };
} // namespace OloEngine::Animation
