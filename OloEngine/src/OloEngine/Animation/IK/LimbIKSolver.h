#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/BlendNode.h"
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <span>
#include <vector>

namespace OloEngine::Animation
{
    struct LimbIKParams
    {
        u32 TargetBoneIndex = 0;
        glm::vec3 TargetPosition{ 0.0f }; // model-space target position
        u32 ChainLength = 2;              // number of bones in IK chain
        f32 Weight = 1.0f;                // 0..1 blend between original and IK result
        u32 MaxIterations = 15;
        f32 ConvergenceThreshold = 0.00001f;
    };

    class LimbIKSolver
    {
      public:
        // Modifies local-space bone rotations in-place so the end-effector
        // (TargetBoneIndex) reaches toward the target position using FABRIK.
        static void Solve(
            std::span<BoneTransform> pose,
            std::span<const int> parentIndices,
            const LimbIKParams& params,
            std::span<const glm::mat4> preTransforms = {});
    };
} // namespace OloEngine::Animation
