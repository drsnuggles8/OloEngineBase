#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageCalculation.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Entity.h"

namespace OloEngine
{

    void GameplayAbilitySystem::OnUpdate(Scene* scene, f32 dt)
    {
        auto view = scene->GetAllEntitiesWith<AbilityComponent>();
        for (auto entityId : view)
        {
            auto& ac = view.get<AbilityComponent>(entityId);

            // Tick cooldowns
            ac.Cooldowns.Tick(dt);

            // Tick active abilities
            for (auto& ability : ac.Abilities)
            {
                if (!ability.IsActive)
                {
                    continue;
                }
                ability.ActiveTime += dt;

                // Handle channeled ability duration
                if (ability.Definition.IsChanneled)
                {
                    ability.ChannelRemaining -= dt;
                    if (ability.ChannelRemaining <= 0.0f)
                    {
                        ability.IsActive = false;
                        // Remove activation granted tags
                        for (auto const& tag : ability.Definition.ActivationGrantedTags.GetTags())
                        {
                            ac.OwnedTags.RemoveTag(tag);
                        }
                    }
                }
            }

            // Tick active effects (duration, periodic application)
            ac.ActiveEffects.Tick(dt, ac.Attributes, ac.OwnedTags);
        }
    }

    bool GameplayAbilitySystem::TryActivateAbility(Scene* scene, Entity owner, const GameplayTag& abilityTag)
    {
        if (!owner.HasComponent<AbilityComponent>())
        {
            return false;
        }

        auto& ac = owner.GetComponent<AbilityComponent>();

        // Find the ability by tag
        ActiveAbility* target = nullptr;
        for (auto& ability : ac.Abilities)
        {
            if (ability.Definition.AbilityTag.MatchesExact(abilityTag))
            {
                target = &ability;
                break;
            }
        }

        if (!target)
        {
            return false;
        }

        auto const& def = target->Definition;

        // Already active?
        if (target->IsActive)
        {
            return false;
        }

        // Check cooldown
        if (ac.Cooldowns.IsOnCooldown(def.AbilityTag))
        {
            return false;
        }

        // Check required tags
        if (!def.RequiredTags.IsEmpty() && !ac.OwnedTags.HasAll(def.RequiredTags))
        {
            return false;
        }

        // Check blocked tags
        if (!def.BlockedTags.IsEmpty() && ac.OwnedTags.HasAny(def.BlockedTags))
        {
            return false;
        }

        // Check resource cost
        if (def.ResourceCost > 0.0f && !def.CostAttribute.empty())
        {
            f32 current = ac.Attributes.GetCurrentValue(def.CostAttribute);
            if (current < def.ResourceCost)
            {
                return false;
            }
        }

        // Commit: deduct cost
        if (def.ResourceCost > 0.0f && !def.CostAttribute.empty())
        {
            f32 current = ac.Attributes.GetBaseValue(def.CostAttribute);
            ac.Attributes.SetBaseValue(def.CostAttribute, current - def.ResourceCost);
        }

        // Start cooldown
        if (def.CooldownDuration > 0.0f)
        {
            ac.Cooldowns.StartCooldown(def.AbilityTag, def.CooldownDuration);
        }

        // Activate
        target->IsActive = true;
        target->ActiveTime = 0.0f;
        target->ChannelRemaining = def.ChannelDuration;

        // Add activation granted tags
        for (auto const& tag : def.ActivationGrantedTags.GetTags())
        {
            ac.OwnedTags.AddTag(tag);
        }

        // Apply activation effects
        for (auto const& effect : def.ActivationEffects)
        {
            ac.ActiveEffects.ApplyEffect(effect, ac.OwnedTags, def.AbilityTag);
        }

        // For non-channeled abilities, immediately deactivate
        if (!def.IsChanneled)
        {
            target->IsActive = false;
            for (auto const& tag : def.ActivationGrantedTags.GetTags())
            {
                ac.OwnedTags.RemoveTag(tag);
            }
        }

        return true;
    }

    void GameplayAbilitySystem::CancelAbility(Scene* scene, Entity owner, const GameplayTag& abilityTag)
    {
        if (!owner.HasComponent<AbilityComponent>())
        {
            return;
        }

        auto& ac = owner.GetComponent<AbilityComponent>();

        for (auto& ability : ac.Abilities)
        {
            if (ability.Definition.AbilityTag.MatchesExact(abilityTag) && ability.IsActive)
            {
                ability.IsActive = false;
                for (auto const& tag : ability.Definition.ActivationGrantedTags.GetTags())
                {
                    ac.OwnedTags.RemoveTag(tag);
                }
                break;
            }
        }
    }

    void GameplayAbilitySystem::ApplyDamage(Scene* scene, const DamageEvent& event)
    {
        if (!event.Target || !event.Target->HasComponent<AbilityComponent>())
        {
            return;
        }

        auto& targetAC = event.Target->GetComponent<AbilityComponent>();

        AttributeSet sourceAttribs;
        if (event.Source && event.Source->HasComponent<AbilityComponent>())
        {
            sourceAttribs = event.Source->GetComponent<AbilityComponent>().Attributes;
        }

        f32 finalDamage = DamageCalculation::Calculate(event, sourceAttribs, targetAC.Attributes);

        // Apply damage to Health
        if (targetAC.Attributes.HasAttribute("Health"))
        {
            f32 currentHealth = targetAC.Attributes.GetBaseValue("Health");
            f32 newHealth = std::max(currentHealth - finalDamage, 0.0f);
            targetAC.Attributes.SetBaseValue("Health", newHealth);
        }
    }

} // namespace OloEngine
