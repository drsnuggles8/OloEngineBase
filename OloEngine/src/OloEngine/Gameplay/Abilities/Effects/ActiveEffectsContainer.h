#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace OloEngine
{

    struct ActiveEffect
    {
        GameplayEffect Definition;
        f32 RemainingDuration = 0.0f;
        f32 PeriodTimer = 0.0f;
        i32 CurrentStacks = 1;
        GameplayTag SourceTag; // Who applied this effect
        bool ModifiersApplied = false;
        bool TagsApplied = false;

        auto operator==(const ActiveEffect&) const -> bool = default;
    };

    class ActiveEffectsContainer
    {
      public:
        ActiveEffectsContainer() = default;

        // Returns true if the effect was successfully applied
        bool ApplyEffect(const GameplayEffect& effect, const GameplayTagContainer& targetTags, const GameplayTag& sourceTag);
        void RemoveEffectsBySource(const GameplayTag& sourceTag);
        void RemoveEffectByName(const std::string& effectName);
        void Clear();

        void Tick(f32 dt, AttributeSet& attributes, GameplayTagContainer& ownerTags);

        [[nodiscard]] const std::vector<ActiveEffect>& GetActiveEffects() const
        {
            return m_ActiveEffects;
        }
        [[nodiscard]] const std::unordered_map<GameplayTag, i32>& GetTagGrantCounts() const
        {
            return m_TagGrantCounts;
        }
        [[nodiscard]] bool HasAnyEffects() const
        {
            return !m_ActiveEffects.empty();
        }

        // For deserialization — overwrites internal state. Caller must ensure the
        // AttributeSet / GameplayTagContainer that consumes Tick() afterwards is
        // already restored to a consistent base state.
        void RestoreFromSnapshot(std::vector<ActiveEffect> effects,
                                 std::unordered_map<GameplayTag, i32> tagGrantCounts)
        {
            m_ActiveEffects = std::move(effects);
            m_TagGrantCounts = std::move(tagGrantCounts);
        }

        auto operator==(const ActiveEffectsContainer&) const -> bool = default;

      private:
        void ApplyInstantEffect(const GameplayEffect& effect, AttributeSet& attributes) const;
        void ApplyPeriodicTick(const ActiveEffect& ae, AttributeSet& attributes) const;
        void AddGrantedTags(const GameplayEffect& effect, GameplayTagContainer& ownerTags);
        void RemoveGrantedTags(const GameplayEffect& effect, GameplayTagContainer& ownerTags);

        std::vector<ActiveEffect> m_ActiveEffects;
        std::unordered_map<GameplayTag, i32> m_TagGrantCounts; // Ref-counted tag grants
    };

} // namespace OloEngine
