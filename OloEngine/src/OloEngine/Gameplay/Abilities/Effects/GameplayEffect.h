#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeModifier.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"

#include <string>
#include <vector>

namespace OloEngine
{

    struct GameplayEffectPolicy
    {
        enum class Duration : u8
        {
            Instant,
            HasDuration,
            Infinite
        };

        Duration DurationType = Duration::Instant;
        f32 DurationSeconds = 0.0f;

        bool IsPeriodic = false;
        f32 PeriodSeconds = 1.0f;

        auto operator==(const GameplayEffectPolicy&) const -> bool = default;
    };

    struct GameplayEffect
    {
        std::string Name;
        GameplayEffectPolicy Policy;

        struct AttributeMod
        {
            std::string AttributeName;
            AttributeModifier::Operation Op = AttributeModifier::Operation::Add;
            f32 Magnitude = 0.0f;

            auto operator==(const AttributeMod&) const -> bool = default;
        };
        std::vector<AttributeMod> Modifiers;

        // Tags granted while this effect is active
        GameplayTagContainer GrantedTags;

        // Requirements to apply this effect
        GameplayTagContainer RequiredTags;
        GameplayTagContainer BlockedTags;

        // Stacking
        i32 MaxStacks = 1;
        bool RefreshDurationOnStack = true;

        auto operator==(const GameplayEffect&) const -> bool = default;
    };

} // namespace OloEngine
