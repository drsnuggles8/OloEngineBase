#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include <glm/glm.hpp>

namespace OloEngine
{
    struct IKTargetComponent
    {
        // --- Aim IK ---
        bool AimIKEnabled = false;
        u32 AimBoneIndex = 0;
        glm::vec3 AimTarget{0.0f};
        glm::vec3 AimAxis{0.0f, 0.0f, 1.0f};
        glm::vec3 AimOffset{0.0f};
        glm::vec3 AimPoleVector{0.0f, 1.0f, 0.0f};
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
        glm::vec3 LimbTarget{0.0f};
        u32 LimbChainLength = 2;
        f32 LimbWeight = 1.0f;

        // Optional entity whose world position overrides LimbTarget each frame.
        UUID LimbTargetEntity = 0;
    };
} // namespace OloEngine
