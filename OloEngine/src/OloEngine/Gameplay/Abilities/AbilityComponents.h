#pragma once

#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"
#include "OloEngine/Gameplay/Abilities/Effects/ActiveEffectsContainer.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"
#include "OloEngine/Gameplay/Abilities/CooldownManager.h"

#include <vector>

namespace OloEngine
{

    struct AbilityComponent
    {
        AttributeSet Attributes;
        GameplayTagContainer OwnedTags;
        std::vector<ActiveAbility> Abilities;
        ActiveEffectsContainer ActiveEffects;
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

        auto operator==(const AbilityComponent&) const -> bool = default;
    };

} // namespace OloEngine
