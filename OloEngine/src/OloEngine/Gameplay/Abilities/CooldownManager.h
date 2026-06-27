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

        [[nodiscard("cooldown state must be checked")]] bool IsOnCooldown(const GameplayTag& abilityTag) const;
        [[nodiscard("remaining cooldown must be used")]] f32 GetRemainingCooldown(const GameplayTag& abilityTag) const;
        [[nodiscard("cooldown fraction must be used")]] f32 GetCooldownFraction(const GameplayTag& abilityTag) const;

        void ResetCooldown(const GameplayTag& abilityTag);
        void ResetAll();

        // For deserialization round-trip: (tag, duration, remaining) triples.
        // Cleaner than exposing the private CooldownEntry struct.
        void RestoreFromSnapshot(const GameplayTag& abilityTag, f32 duration, f32 remaining);

        // For serialization: enumerate (tag, duration, remaining) without
        // leaking the private CooldownEntry struct to the caller.
        template<typename Fn>
        void ForEachCooldown(Fn&& fn) const
        {
            for (auto const& [tag, entry] : m_Cooldowns)
            {
                fn(tag, entry.Duration, entry.Remaining);
            }
        }
        [[nodiscard("active-cooldown count must be used")]] std::size_t Size() const
        {
            return m_Cooldowns.size();
        }

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
