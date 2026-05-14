#include "OloEnginePCH.h"

// =============================================================================
// AbilityActivationEffectAppliesToCasterTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayAbilitySystem::TryActivateAbility (ActivationEffects loop) ×
//   ActiveEffectsContainer.ApplyEffect (Instant branch) × Scene tick ×
//   ActiveEffectsContainer::Tick (ApplyInstantEffect + erase). When an
//   ability with `ActivationEffects = [Instant -10 Health]` is activated:
//     1. TryActivateAbility loops the effects and calls
//        `ac.ActiveEffects.ApplyEffect(...)`. For Instant effects this
//        pushes a transient entry into m_ActiveEffects.
//     2. Next Tick, ActiveEffects::Tick sees the Instant policy,
//        invokes ApplyInstantEffect (mutates BaseValue via SetBaseValue),
//        and erases the entry.
//   A regression in either the loop in TryActivateAbility or the
//   Instant branch in Tick silently makes activation cost-free (and
//   self-damage abilities don't damage anything).
//
// Scenario: caster has Health=100. Ability `BloodSacrifice` has an
// Instant ActivationEffect that Adds -25 Health. After
// TryActivateAbility:
//   - Pre-tick: Health still 100 (Instant hasn't been processed yet).
//   - Post one tick: Health = 75, and the effect entry is gone from the container.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbility.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeModifier.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class AbilityActivationEffectAppliesToCasterTest : public FunctionalTest
{
  protected:
    GameplayTag m_AbilityTag{ "Ability.BloodSacrifice" };

    void BuildScene() override
    {
        m_Caster = GetScene().CreateEntity("Cleric");
        auto& ac = m_Caster.AddComponent<AbilityComponent>();
        ac.Attributes.DefineAttribute("Health", 100.0f);

        // Instant -25 Health.
        GameplayEffect cost;
        cost.Name = "BloodSacrificeCost";
        cost.Policy.DurationType = GameplayEffectPolicy::Duration::Instant;
        GameplayEffect::AttributeMod mod;
        mod.AttributeName = "Health";
        mod.Op = AttributeModifier::Operation::Add;
        mod.Magnitude = -25.0f;
        cost.Modifiers.push_back(mod);

        GameplayAbilityDef def;
        def.Name = "BloodSacrifice";
        def.AbilityTag = m_AbilityTag;
        def.CooldownDuration = 0.0f;
        def.ResourceCost = 0.0f; // we exercise the activation-effect path, not the resource gate
        def.ActivationEffects.push_back(std::move(cost));

        ActiveAbility ability;
        ability.Definition = std::move(def);
        ac.Abilities.push_back(std::move(ability));
    }

    Entity m_Caster;
};

TEST_F(AbilityActivationEffectAppliesToCasterTest, InstantSelfDamageActivationEffectDeductsHealthOnNextTick)
{
    auto& ac = m_Caster.GetComponent<AbilityComponent>();
    ASSERT_NEAR(ac.Attributes.GetBaseValue("Health"), 100.0f, 1e-3f);

    ASSERT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Caster, m_AbilityTag));

    // Right after activation: the Instant effect is in m_ActiveEffects but
    // hasn't been processed by Tick yet, so Health is unchanged.
    EXPECT_NEAR(ac.Attributes.GetBaseValue("Health"), 100.0f, 1e-3f)
        << "TryActivateAbility processed the Instant effect inline — that "
           "couples activation to attribute mutation before any system "
           "boundary, which breaks event-driven gameplay (audio, FX) that "
           "watches for the per-tick attribute change.";

    // One Scene tick → ActiveEffects::Tick processes the Instant entry,
    // calls ApplyInstantEffect (BaseValue -= 25), and erases the entry.
    RunFrames(1);

    EXPECT_NEAR(ac.Attributes.GetBaseValue("Health"), 75.0f, 1e-3f)
        << "Instant activation effect didn't deduct Health on the next tick "
           "— ActiveEffects::Tick's Instant branch is skipping ApplyInstantEffect "
           "or routing to the periodic branch by mistake.";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), 75.0f, 1e-3f)
        << "BaseValue moved but CurrentValue didn't — the dirty flag on the "
           "AttributeSet wasn't set after SetBaseValue.";
}
