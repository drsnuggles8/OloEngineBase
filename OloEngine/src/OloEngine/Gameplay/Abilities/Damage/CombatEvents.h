#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // POD-style notification payload published on the per-Scene
    // GameplayEventBus (mirrors QuestEvents.h / InventoryEvents.h).

    /// Published by GameplayAbilitySystem::ApplyDamage when a damage
    /// application drops the victim's Health current value from > 0 to <= 0 —
    /// the one place attacker and victim are simultaneously known.
    /// KillerID is 0 when the damage source entity was invalid.
    struct EntityKilledEvent
    {
        UUID VictimID;
        UUID KillerID;
        i32 ExperienceGranted = 0; ///< XP bounty granted to the killer (0 if none)
    };

    /// Published after a weapon impact has passed through the shared gameplay
    /// damage pipeline. Presentation systems can subscribe without duplicating
    /// hit detection or health mutation.
    struct WeaponImpactEvent
    {
        UUID SourceID;
        UUID TargetID;
        glm::vec3 Position{ 0.0f };
        glm::vec3 Normal{ 0.0f };
        f32 AppliedDamage = 0.0f;
    };
} // namespace OloEngine
