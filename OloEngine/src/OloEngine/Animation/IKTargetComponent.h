#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Math/Math.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    struct IKTargetComponent
    {
        // --- Aim IK ---
        bool AimIKEnabled = false;
        u32 AimBoneIndex = 0;
        glm::vec3 AimTarget{ 0.0f };
        glm::vec3 AimAxis{ 0.0f, 0.0f, 1.0f };
        glm::vec3 AimOffset{ 0.0f };
        glm::vec3 AimPoleVector{ 0.0f, 1.0f, 0.0f };
        u32 AimChainLength = 1;
        f32 AimChainFactor = 0.5f;
        f32 AimWeight = 1.0f;

        // Optional entity whose world position overrides AimTarget each frame.
        // When non-zero, the scene update reads this entity's TransformComponent
        // and writes its translation to AimTarget before running the IK solver.
        UUID AimTargetEntity = 0;

        // --- Limb IK ---
        bool LimbIKEnabled = false;
        u32 LimbBoneIndex = 0;
        glm::vec3 LimbTarget{ 0.0f };
        u32 LimbChainLength = 2;
        f32 LimbWeight = 1.0f;

        // Optional entity whose world position overrides LimbTarget each frame.
        UUID LimbTargetEntity = 0;

        // --- Chain IK (FABRIK full N-bone chain — spines, tails, tentacles) ---
        bool ChainIKEnabled = false;
        u32 ChainBoneIndex = 0; // tip / end-effector bone of the chain
        glm::vec3 ChainTarget{ 0.0f };
        glm::vec3 ChainPoleVector{ 0.0f }; // world-space bend hint; zero = disabled
        u32 ChainLength = 3;               // number of bones in the chain (>= 2)
        u32 ChainIterations = 10;
        f32 ChainTolerance = 0.001f;
        f32 ChainWeight = 1.0f;

        // Optional entity whose world position overrides ChainTarget each frame.
        UUID ChainTargetEntity = 0;

        // Trivially-copyable POD component (UUID is a trivially-copyable u64
        // wrapper). A single whole-struct bitwise compare avoids the per-member
        // UUID C2666 ambiguity and matches the editor's tier-1 memcmp undo
        // detection (see docs/agent-rules/cpp-coding-quality.md §7).
        auto operator==(const IKTargetComponent& o) const -> bool
        {
            return Math::BitwiseEqual(*this, o);
        }
    };
} // namespace OloEngine
