#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageCalculation.h"
#include "OloEngine/Gameplay/Abilities/Damage/CombatEvents.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
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

            // Death state: when Health drops to 0, remove State.Alive and add State.Dead
            if (ac.Attributes.HasAttribute("Health") && ac.Attributes.GetCurrentValue("Health") <= 0.0f && ac.OwnedTags.HasTagExact(GameplayTag("State.Alive")))
            {
                ac.OwnedTags.RemoveTag(GameplayTag("State.Alive"));
                ac.OwnedTags.AddTag(GameplayTag("State.Dead"));
            }
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

        // Already active? Toggled abilities can be deactivated by re-activating
        if (target->IsActive)
        {
            if (def.IsToggled)
            {
                CancelAbility(scene, owner, abilityTag);
                return true;
            }
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
            f32 current = ac.Attributes.GetCurrentValue(def.CostAttribute);
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

        // For non-channeled, non-toggled abilities, immediately deactivate
        if (!def.IsChanneled && !def.IsToggled)
        {
            target->IsActive = false;
            for (auto const& tag : def.ActivationGrantedTags.GetTags())
            {
                ac.OwnedTags.RemoveTag(tag);
            }
        }

        return true;
    }

    void GameplayAbilitySystem::CancelAbility([[maybe_unused]] Scene* scene, Entity owner, const GameplayTag& abilityTag)
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

    f32 GameplayAbilitySystem::ApplyDamage(Scene* scene, const DamageEvent& event)
    {
        Entity target = event.Target;
        if (!target || !target.HasComponent<AbilityComponent>())
        {
            return 0.0f;
        }

        auto& targetAC = target.GetComponent<AbilityComponent>();

        AttributeSet sourceAttribs;
        if (Entity source = event.Source; source && source.HasComponent<AbilityComponent>())
        {
            sourceAttribs = source.GetComponent<AbilityComponent>().Attributes;
        }

        f32 finalDamage = DamageCalculation::Calculate(event, sourceAttribs, targetAC.Attributes);

        // Apply damage to Health. Base-value read + write on purpose: the
        // current value is base + modifiers, so reading the *current* value
        // and writing it back into the base would double-count every active
        // Health modifier (the bug the old C#/Lua glue duplicates had).
        bool killed = false;
        if (targetAC.Attributes.HasAttribute("Health"))
        {
            f32 currentBefore = targetAC.Attributes.GetCurrentValue("Health");
            f32 baseHealth = targetAC.Attributes.GetBaseValue("Health");
            f32 newHealth = std::max(baseHealth - finalDamage, 0.0f);
            targetAC.Attributes.SetBaseValue("Health", newHealth);
            killed = currentBefore > 0.0f && targetAC.Attributes.GetCurrentValue("Health") <= 0.0f;
        }

        if (killed)
        {
            // Immediate death-tag flip (same opt-in State.Alive convention as
            // the OnUpdate sweep, which stays as the catch-all for non-damage
            // health writes).
            if (targetAC.OwnedTags.HasTagExact(GameplayTag("State.Alive")))
            {
                targetAC.OwnedTags.RemoveTag(GameplayTag("State.Alive"));
                targetAC.OwnedTags.AddTag(GameplayTag("State.Dead"));
            }

            // Kill-XP bounty: the one place attacker and victim are both known.
            Entity source = event.Source;
            i32 xpGranted = 0;
            if (source && source.HasComponent<ProgressionComponent>() && target.HasComponent<ProgressionComponent>())
            {
                i32 bounty = target.GetComponent<ProgressionComponent>().XPBounty;
                if (bounty > 0)
                {
                    source.GetComponent<ProgressionComponent>().AddPendingXP(bounty);
                    xpGranted = bounty;
                }
            }

            if (scene)
            {
                // Publish after all state mutation (JointBroke precedent).
                scene->GetGameplayEvents().Publish(EntityKilledEvent{ target.GetUUID(), source ? source.GetUUID() : UUID(0), xpGranted });
            }
        }

        return finalDamage;
    }

} // namespace OloEngine
