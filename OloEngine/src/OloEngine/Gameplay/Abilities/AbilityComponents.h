#pragma once

#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"
#include "OloEngine/Gameplay/Abilities/Effects/ActiveEffectsContainer.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"
#include "OloEngine/Gameplay/Abilities/CooldownManager.h"
#include "OloEngine/Scene/ComponentReflection.h" // OLO_SERIALIZE(Skip) to mark runtime GAS state

#include <vector>

namespace OloEngine
{

    struct AbilityComponent
    {
        AttributeSet Attributes;
        GameplayTagContainer OwnedTags;
        std::vector<ActiveAbility> Abilities;
        // Runtime GAS state — applied effects & cooldown timers, rebuilt at runtime; not scene-serialized.
        OLO_SERIALIZE(Skip)
        ActiveEffectsContainer ActiveEffects;
        OLO_SERIALIZE(Skip)
        CooldownManager Cooldowns;

        void InitializeDefaultRPGAttributes(f32 maxHealth, f32 maxMana, f32 attackPower, f32 defense)
        {
            Attributes.DefineAttribute("MaxHealth", maxHealth);
            Attributes.DefineAttribute("Health", maxHealth);
            Attributes.DefineAttribute("MaxMana", maxMana);
            Attributes.DefineAttribute("Mana", maxMana);
            Attributes.DefineAttribute("AttackPower", attackPower);
            Attributes.DefineAttribute("Defense", defense);
        }

        // Authored-state equality only: ActiveEffects and Cooldowns are runtime GAS state
        // (OLO_SERIALIZE(Skip)'d, rebuilt at runtime), so they are excluded — mirroring how the
        // runtime-token fields elsewhere are kept out of operator==.
        auto operator==(const AbilityComponent& other) const -> bool
        {
            return Attributes == other.Attributes && OwnedTags == other.OwnedTags && Abilities == other.Abilities;
        }
    };

} // namespace OloEngine
