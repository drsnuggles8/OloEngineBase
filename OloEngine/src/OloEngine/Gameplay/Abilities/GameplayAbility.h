#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"

#include <string>
#include <vector>

namespace OloEngine
{

    struct GameplayAbilityDef
    {
        std::string Name;
        GameplayTag AbilityTag;

        // Tag requirements
        GameplayTagContainer RequiredTags;          // Owner must have these to activate
        GameplayTagContainer BlockedTags;           // Owner must NOT have these
        GameplayTagContainer ActivationGrantedTags; // Added to owner while active

        // Costs
        f32 CooldownDuration = 0.0f;
        f32 ResourceCost = 0.0f;
        std::string CostAttribute = "Mana";

        // Effects applied on activation (to the caster)
        std::vector<GameplayEffect> ActivationEffects;

        // Effects applied to the target when using TryActivateAbilityOnTarget.
        // If empty, ActivationEffects are used for the target as well (legacy behavior).
        std::vector<GameplayEffect> TargetActivationEffects;

        // Whether this is a channeled/toggled ability
        bool IsChanneled = false;
        bool IsToggled = false;
        f32 ChannelDuration = 0.0f;

        auto operator==(const GameplayAbilityDef&) const -> bool = default;
    };

    struct ActiveAbility
    {
        GameplayAbilityDef Definition;
        bool IsActive = false;
        f32 ActiveTime = 0.0f;
        f32 ChannelRemaining = 0.0f;

        auto operator==(const ActiveAbility&) const -> bool = default;
    };

} // namespace OloEngine
