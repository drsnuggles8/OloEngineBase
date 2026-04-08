#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/BlendNode.h"
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <span>

namespace OloEngine::Animation
{
    struct AimIKParams
    {
        u32 TargetBoneIndex = 0;
        glm::vec3 TargetPosition{ 0.0f };         // model-space target to aim at
        glm::vec3 AimAxis{ 0.0f, 0.0f, 1.0f };    // bone-local axis that should point at target
        glm::vec3 AimOffset{ 0.0f };              // bone-local offset from bone origin
        glm::vec3 PoleVector{ 0.0f, 1.0f, 0.0f }; // up reference to prevent twist
        u32 ChainLength = 1;                      // how many ancestors participate
        f32 ChainFactor = 0.5f;                   // rotation distribution (0=all on end, 1=all on root)
        f32 Weight = 1.0f;                        // 0..1 blend between original pose and IK result
    };

    class AimIKSolver
    {
      public:
        // Modifies local-space bone rotations in-place so the aim axis of the
        // target bone points toward TargetPosition in model space.
        static void Solve(
            std::span<BoneTransform> pose,
            std::span<const int> parentIndices,
            const AimIKParams& params,
            std::span<const glm::mat4> preTransforms = {});
    };
} // namespace OloEngine::Animation
