#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

#include <string>
#include <unordered_map>

namespace OloEngine
{

    class CooldownManager
    {
    public:
        CooldownManager() = default;

        void StartCooldown(const GameplayTag& abilityTag, f32 duration);
        void Tick(f32 dt);

        [[nodiscard]] bool IsOnCooldown(const GameplayTag& abilityTag) const;
        [[nodiscard]] f32 GetRemainingCooldown(const GameplayTag& abilityTag) const;
        [[nodiscard]] f32 GetCooldownFraction(const GameplayTag& abilityTag) const;

        void ResetCooldown(const GameplayTag& abilityTag);
        void ResetAll();

        auto operator==(const CooldownManager&) const -> bool = default;

    private:
        struct CooldownEntry
        {
            f32 Duration = 0.0f;
            f32 Remaining = 0.0f;

            auto operator==(const CooldownEntry&) const -> bool = default;
        };

        std::unordered_map<GameplayTag, CooldownEntry> m_Cooldowns;
    };

} // namespace OloEngine
