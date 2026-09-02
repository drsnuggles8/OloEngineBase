#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Scene/ComponentReflection.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    struct IKTargetComponent
    {
        // --- Aim IK ---
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
        u32 LimbBoneIndex = 0;
        glm::vec3 LimbTarget{ 0.0f };
        u32 LimbChainLength = 2;
        f32 LimbWeight = 1.0f;

        // Optional entity whose world position overrides LimbTarget each frame.
        UUID LimbTargetEntity = 0;

        // --- Chain IK (FABRIK full N-bone chain — spines, tails, tentacles) ---
        u32 ChainBoneIndex = 0; // tip / end-effector bone of the chain
        glm::vec3 ChainTarget{ 0.0f };
        glm::vec3 ChainPoleVector{ 0.0f }; // world-space bend hint; zero = disabled
        u32 ChainLength = 3;               // number of bones in the chain (>= 2)
        u32 ChainIterations = 10;
        f32 ChainTolerance = 0.001f;

        // Optional entity whose world position overrides ChainTarget each frame.
        // Declared before ChainWeight so the UUID lands on an 8-byte boundary
        // without a padding hole (issue #1019).
        UUID ChainTargetEntity = 0;
        f32 ChainWeight = 1.0f;

        // --- Flags ---
        // Every bool sits here, after the 4- and 8-byte members, so the layout has
        // no alignment holes (issue #1019): operator== below is a whole-object
        // memcmp and unnamed padding is unspecified.
        bool AimIKEnabled = false;
        bool LimbIKEnabled = false;
        bool ChainIKEnabled = false;
        OLO_SERIALIZE(Skip)
        u8 Pad0 = 0;

        // Trivially-copyable POD component (UUID is a trivially-copyable u64
        // wrapper). A single whole-struct bitwise compare avoids the per-member
        // UUID C2666 ambiguity and matches the editor's tier-1 memcmp undo
        // detection (see docs/agent-rules/cpp-coding-quality.md §7).
        auto operator==(const IKTargetComponent& o) const -> bool
        {
            return Math::BitwiseEqual(*this, o);
        }
    };
    static_assert(sizeof(IKTargetComponent) == 160, "IKTargetComponent must have no padding: see BitwiseEqualLayoutTest");
} // namespace OloEngine
