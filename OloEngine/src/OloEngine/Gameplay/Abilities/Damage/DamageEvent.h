#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

#include <functional>
#include <glm/glm.hpp>

namespace OloEngine
{
    class Entity;

    struct DamageEvent
    {
        Entity* Source = nullptr;
        Entity* Target = nullptr;
        f32 RawDamage = 0.0f;
        GameplayTag DamageType;
        bool IsCritical = false;
        f32 CritMultiplier = 2.0f;
        glm::vec3 HitLocation{ 0.0f };
        glm::vec3 HitNormal{ 0.0f, 1.0f, 0.0f };
    };

} // namespace OloEngine
