#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Abilities/Effects/ActiveEffectsContainer.h"

namespace OloEngine
{

    bool ActiveEffectsContainer::ApplyEffect(const GameplayEffect& effect, const GameplayTagContainer& targetTags, const GameplayTag& sourceTag)
    {
        // Check requirements
        if (!effect.RequiredTags.IsEmpty() && !targetTags.HasAll(effect.RequiredTags))
        {
            return false;
        }
        if (!effect.BlockedTags.IsEmpty() && targetTags.HasAny(effect.BlockedTags))
        {
            return false;
        }

        // For instant effects there's nothing to track in the container
        // They are applied directly via ApplyInstantEffect during Tick
        // But we still add them temporarily so the system can process them
        if (effect.Policy.DurationType == GameplayEffectPolicy::Duration::Instant)
        {
            // Instant effects will be applied and removed in the same tick
            ActiveEffect ae;
            ae.Definition = effect;
            ae.RemainingDuration = 0.0f;
            ae.CurrentStacks = 1;
            ae.SourceTag = sourceTag;
            m_ActiveEffects.push_back(std::move(ae));
            return true;
        }

        // Check for stacking on existing same-name effects
        for (auto& existing : m_ActiveEffects)
        {
            if (existing.Definition.Name == effect.Name)
            {
                if (existing.CurrentStacks < effect.MaxStacks)
                {
                    ++existing.CurrentStacks;
                }
                if (effect.RefreshDurationOnStack)
                {
                    existing.RemainingDuration = effect.Policy.DurationSeconds;
                }
                return true;
            }
        }

        ActiveEffect ae;
        ae.Definition = effect;
        ae.RemainingDuration = effect.Policy.DurationSeconds;
        ae.PeriodTimer = 0.0f;
        ae.CurrentStacks = 1;
        ae.SourceTag = sourceTag;
        m_ActiveEffects.push_back(std::move(ae));
        return true;
    }

    void ActiveEffectsContainer::RemoveEffectsBySource(const GameplayTag& sourceTag)
    {
        std::erase_if(m_ActiveEffects, [&sourceTag](const ActiveEffect& ae)
                      { return ae.SourceTag.MatchesExact(sourceTag); });
    }

    void ActiveEffectsContainer::RemoveEffectByName(const std::string& effectName)
    {
        std::erase_if(m_ActiveEffects, [&effectName](const ActiveEffect& ae)
                      { return ae.Definition.Name == effectName; });
    }

    void ActiveEffectsContainer::Clear()
    {
        m_ActiveEffects.clear();
    }

    void ActiveEffectsContainer::Tick(f32 dt, AttributeSet& attributes, GameplayTagContainer& ownerTags)
    {
        for (auto it = m_ActiveEffects.begin(); it != m_ActiveEffects.end();)
        {
            auto& ae = *it;

            // Handle instant effects - apply once and remove
            if (ae.Definition.Policy.DurationType == GameplayEffectPolicy::Duration::Instant)
            {
                ApplyInstantEffect(ae.Definition, attributes);
                it = m_ActiveEffects.erase(it);
                continue;
            }

            // Handle periodic effects
            if (ae.Definition.Policy.IsPeriodic)
            {
                ae.PeriodTimer += dt;
                while (ae.PeriodTimer >= ae.Definition.Policy.PeriodSeconds)
                {
                    ae.PeriodTimer -= ae.Definition.Policy.PeriodSeconds;
                    ApplyPeriodicTick(ae, attributes);
                }
            }
            else if (!ae.ModifiersApplied)
            {
                // Non-periodic duration/infinite effects: apply modifiers once as persistent modifiers
                for (auto const& mod : ae.Definition.Modifiers)
                {
                    AttributeModifier attrMod;
                    attrMod.Op = mod.Op;
                    attrMod.Magnitude = mod.Magnitude;
                    attrMod.Source = ae.SourceTag;
                    attributes.AddModifier(mod.AttributeName, attrMod);
                }
                ae.ModifiersApplied = true;
            }

            // Add granted tags (only once)
            if (!ae.TagsApplied)
            {
                AddGrantedTags(ae.Definition, ownerTags);
                ae.TagsApplied = true;
            }

            // Update duration for HasDuration effects
            if (ae.Definition.Policy.DurationType == GameplayEffectPolicy::Duration::HasDuration)
            {
                ae.RemainingDuration -= dt;
                if (ae.RemainingDuration <= 0.0f)
                {
                    // Remove modifiers applied by this effect
                    for (auto const& mod : ae.Definition.Modifiers)
                    {
                        attributes.RemoveModifiersBySource(mod.AttributeName, ae.SourceTag);
                    }
                    RemoveGrantedTags(ae.Definition, ownerTags);
                    it = m_ActiveEffects.erase(it);
                    continue;
                }
            }

            ++it;
        }
    }

    void ActiveEffectsContainer::ApplyInstantEffect(const GameplayEffect& effect, AttributeSet& attributes)
    {
        for (auto const& mod : effect.Modifiers)
        {
            if (mod.Op == AttributeModifier::Operation::Add)
            {
                f32 current = attributes.GetBaseValue(mod.AttributeName);
                attributes.SetBaseValue(mod.AttributeName, current + mod.Magnitude);
            }
            else if (mod.Op == AttributeModifier::Operation::Override)
            {
                attributes.SetBaseValue(mod.AttributeName, mod.Magnitude);
            }
            else if (mod.Op == AttributeModifier::Operation::Multiply)
            {
                f32 current = attributes.GetBaseValue(mod.AttributeName);
                attributes.SetBaseValue(mod.AttributeName, current * mod.Magnitude);
            }
        }
    }

    void ActiveEffectsContainer::ApplyPeriodicTick(const ActiveEffect& ae, AttributeSet& attributes)
    {
        // Periodic effects apply their modifiers as instant modifications
        // scaled by stack count
        for (auto const& mod : ae.Definition.Modifiers)
        {
            f32 magnitude = mod.Magnitude * static_cast<f32>(ae.CurrentStacks);
            if (mod.Op == AttributeModifier::Operation::Add)
            {
                f32 current = attributes.GetBaseValue(mod.AttributeName);
                attributes.SetBaseValue(mod.AttributeName, current + magnitude);
            }
            else if (mod.Op == AttributeModifier::Operation::Override)
            {
                attributes.SetBaseValue(mod.AttributeName, magnitude);
            }
            else if (mod.Op == AttributeModifier::Operation::Multiply)
            {
                f32 current = attributes.GetBaseValue(mod.AttributeName);
                attributes.SetBaseValue(mod.AttributeName, current * magnitude);
            }
        }
    }

    void ActiveEffectsContainer::AddGrantedTags(const GameplayEffect& effect, GameplayTagContainer& ownerTags)
    {
        for (auto const& tag : effect.GrantedTags.GetTags())
        {
            ownerTags.AddTag(tag);
        }
    }

    void ActiveEffectsContainer::RemoveGrantedTags(const GameplayEffect& effect, GameplayTagContainer& ownerTags)
    {
        for (auto const& tag : effect.GrantedTags.GetTags())
        {
            ownerTags.RemoveTag(tag);
        }
    }

} // namespace OloEngine
