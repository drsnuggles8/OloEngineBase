#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"

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
} // namespace OloEngine
