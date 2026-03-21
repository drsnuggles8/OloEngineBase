#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"

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

        [[nodiscard]] const std::vector<ActiveEffect>& GetActiveEffects() const { return m_ActiveEffects; }
        [[nodiscard]] bool HasAnyEffects() const { return !m_ActiveEffects.empty(); }

        auto operator==(const ActiveEffectsContainer&) const -> bool = default;

    private:
        void ApplyInstantEffect(const GameplayEffect& effect, AttributeSet& attributes);
        void ApplyPeriodicTick(const ActiveEffect& ae, AttributeSet& attributes);
        void AddGrantedTags(const GameplayEffect& effect, GameplayTagContainer& ownerTags);
        void RemoveGrantedTags(const GameplayEffect& effect, GameplayTagContainer& ownerTags);

        std::vector<ActiveEffect> m_ActiveEffects;
    };

} // namespace OloEngine
