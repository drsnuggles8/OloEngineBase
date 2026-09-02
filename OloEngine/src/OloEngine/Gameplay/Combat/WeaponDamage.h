#pragma once

#include "OloEngine/Gameplay/Combat/WeaponDefinition.h"

namespace OloEngine
{
    [[nodiscard]] f32 ComputeWeaponDamage(const WeaponDefinition& definition, f32 distance);
} // namespace OloEngine
