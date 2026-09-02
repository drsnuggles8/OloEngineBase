#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Gameplay/Combat/WeaponDefinition.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    struct ProjectileState
    {
        UUID Owner = 0;
        WeaponDefinition Definition;
        glm::vec3 Position{ 0.0f };
        glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
        f32 DistanceTraveled = 0.0f;
        f32 LifetimeRemaining = 0.0f;
        bool Active = false;

        [[nodiscard]] static ProjectileState Launch(UUID owner, const WeaponDefinition& definition,
                                                    const glm::vec3& origin, const glm::vec3& direction);
    };
} // namespace OloEngine
