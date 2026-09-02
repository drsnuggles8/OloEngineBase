#include "OloEngine/Gameplay/Combat/WeaponDamage.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    f32 ComputeWeaponDamage(const WeaponDefinition& definition, f32 distance)
    {
        const f32 damage = std::isfinite(definition.Damage) ? std::max(0.0f, definition.Damage) : 0.0f;
        const f32 minimumMultiplier = std::isfinite(definition.MinimumDamageMultiplier)
                                          ? std::clamp(definition.MinimumDamageMultiplier, 0.0f, 1.0f)
                                          : 1.0f;
        const f32 falloffStart = std::isfinite(definition.FalloffStart) ? std::max(0.0f, definition.FalloffStart) : 0.0f;
        const f32 falloffEnd = std::isfinite(definition.FalloffEnd) ? std::max(falloffStart, definition.FalloffEnd) : falloffStart;
        const f32 safeDistance = std::isfinite(distance) ? std::max(0.0f, distance) : 0.0f;

        if (safeDistance <= falloffStart)
        {
            return damage;
        }
        if (falloffEnd <= falloffStart || safeDistance >= falloffEnd)
        {
            return damage * minimumMultiplier;
        }

        const f32 alpha = (safeDistance - falloffStart) / (falloffEnd - falloffStart);
        return damage * std::lerp(1.0f, minimumMultiplier, alpha);
    }
} // namespace OloEngine
