#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/BlendNode.h"
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <span>

namespace OloEngine::Animation
{
    struct FABRIKParams
    {
        u32 TargetBoneIndex = 0;          // tip / end-effector bone of the chain
        glm::vec3 TargetPosition{ 0.0f }; // model-space target position
        u32 ChainLength = 3;              // number of bones in IK chain (>= 2)
        glm::vec3 PoleVector{ 0.0f };     // model-space bend hint; zero-length = disabled
        u32 MaxIterations = 10;
        f32 Tolerance = 0.001f;           // model-space distance at which the target counts as reached
        f32 Weight = 1.0f;                // 0..1 blend between original and IK result
    };

    // General N-bone FABRIK chain solver for long chains (spines, tails,
    // tentacles). Unlike LimbIKSolver (tuned for short limb chains with fixed
    // internal iteration settings), this solver exposes the iteration cap and
    // convergence tolerance, short-circuits unreachable targets by
    // straightening the chain toward them, and supports an optional pole
    // vector that biases which side intermediate joints bend toward.
    class FABRIKSolver
    {
      public:
        // Modifies local-space bone rotations in-place so the end-effector
        // (TargetBoneIndex) reaches toward the target position.
        static void Solve(
            std::span<BoneTransform> pose,
            std::span<const int> parentIndices,
            const FABRIKParams& params,
            std::span<const glm::mat4> preTransforms = {});
    };
} // namespace OloEngine::Animation
