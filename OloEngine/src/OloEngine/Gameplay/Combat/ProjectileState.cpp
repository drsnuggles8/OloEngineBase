#include "OloEngine/Gameplay/Combat/ProjectileState.h"

#include <cmath>

namespace OloEngine
{
    ProjectileState ProjectileState::Launch(UUID owner, const WeaponDefinition& definition,
                                            const glm::vec3& origin, const glm::vec3& direction)
    {
        ProjectileState state;
        state.Owner = owner;
        state.Definition = definition;
        state.Position = origin;
        state.LifetimeRemaining = definition.ProjectileLifetime;

        const f32 lengthSquared = glm::dot(direction, direction);
        if (owner != 0 && std::isfinite(lengthSquared) && lengthSquared > 1.0e-8f)
        {
            state.Direction = direction / std::sqrt(lengthSquared);
            state.Active = true;
        }
        return state;
    }
} // namespace OloEngine
