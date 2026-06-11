#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Animation/BlendNode.h"
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <span>
#include <vector>

namespace OloEngine::Animation
{
    struct SpringBoneParams
    {
        u32 EndBoneIndex = 0;                    // chain tip bone; the chain walks up via parentIndices
        u32 ChainLength = 3;                     // number of bones in the chain including the tip (min 2)
        f32 Stiffness = 80.0f;                   // acceleration toward the animated pose per unit offset (1/s^2)
        f32 Damping = 12.0f;                     // velocity damping rate (1/s)
        glm::vec3 Gravity{ 0.0f, -9.81f, 0.0f }; // model-space acceleration (units/s^2)
        f32 Weight = 1.0f;                       // 0..1 blend between animated and simulated pose
    };

    // Per-chain simulation state: model-space positions of the simulated
    // joints (one per chain bone below the chain root). Runtime-only —
    // never serialized. Re-initialized to the animated pose whenever the
    // chain shape changes or the state contains non-finite values.
    struct SpringBoneState
    {
        std::vector<glm::vec3> CurrPositions;
        std::vector<glm::vec3> PrevPositions;
        bool Initialized = false;
    };

    class SpringBoneSolver
    {
      public:
        // Damped spring-mass simulation (Verlet integration) over a bone
        // chain in model space. Each simulated joint is pulled toward its
        // animated position by Stiffness, damped by Damping, accelerated by
        // Gravity, and constrained to keep its animated segment length.
        // Modifies local-space rotations of the chain bones in-place so each
        // bone points at its child's simulated position.
        // Deterministic given (pose, state, deltaTime).
        static void Solve(
            std::span<BoneTransform> pose,
            std::span<const int> parentIndices,
            const SpringBoneParams& params,
            SpringBoneState& state,
            f32 deltaTime,
            std::span<const glm::mat4> preTransforms = {});
    };
} // namespace OloEngine::Animation
