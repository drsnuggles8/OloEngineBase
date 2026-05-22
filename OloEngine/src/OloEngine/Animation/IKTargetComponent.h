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

        // Manual operator== — UUID's implicit operator u64() causes C2666 on MSVC
        // with defaulted ==; float members use Math::BitwiseEqual for bitwise-exact undo detection.
        auto operator==(const IKTargetComponent& o) const -> bool
        {
            return AimIKEnabled == o.AimIKEnabled && AimBoneIndex == o.AimBoneIndex && Math::BitwiseEqual(AimTarget, o.AimTarget) && Math::BitwiseEqual(AimAxis, o.AimAxis) && Math::BitwiseEqual(AimOffset, o.AimOffset) && Math::BitwiseEqual(AimPoleVector, o.AimPoleVector) && AimChainLength == o.AimChainLength && Math::BitwiseEqual(AimChainFactor, o.AimChainFactor) && Math::BitwiseEqual(AimWeight, o.AimWeight) && static_cast<u64>(AimTargetEntity) == static_cast<u64>(o.AimTargetEntity) && LimbIKEnabled == o.LimbIKEnabled && LimbBoneIndex == o.LimbBoneIndex && Math::BitwiseEqual(LimbTarget, o.LimbTarget) && LimbChainLength == o.LimbChainLength && Math::BitwiseEqual(LimbWeight, o.LimbWeight) && static_cast<u64>(LimbTargetEntity) == static_cast<u64>(o.LimbTargetEntity);
        }
    };
} // namespace OloEngine
