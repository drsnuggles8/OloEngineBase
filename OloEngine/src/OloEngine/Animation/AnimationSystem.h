#pragma once

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include <vector>

namespace OloEngine::Animation
{
    // AnimationSystem: Updates animation state and computes bone transforms for animated entities.
    class AnimationSystem
    {
    public:
        // Call once per frame to update all animated entities.
        // In a real ECS, this would take a registry or scene; for now, we use direct component refs.
        static void Update(
            AnimationStateComponent& animState,
            Skeleton& skeleton,
            f32 deltaTime
        );
    };
}
