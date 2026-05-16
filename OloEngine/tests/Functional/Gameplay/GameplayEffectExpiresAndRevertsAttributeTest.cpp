#include "OloEnginePCH.h"

// =============================================================================
// GameplayEffectExpiresAndRevertsAttributeTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayEffect (HasDuration, non-periodic) × AttributeSet modifier
//   stack × ActiveEffectsContainer::Tick × Scene tick.
//   A duration effect with `IsPeriodic=false` lands its modifiers ONCE
//   (the first Tick after ApplyEffect) and removes them when
//   `RemainingDuration <= 0`. Between application and expiry the
//   attribute's *current* value reflects the modifier; *base* value
//   stays untouched (per the modifier-stack model). After expiry,
//   GetCurrentValue should snap back to GetBaseValue.
//
// Scenario: entity with Health base=80. Apply HasDuration effect
// "Bless" that Adds +20 to Health for 0.4s. Tick:
//   - Within the duration: GetCurrentValue("Health") == 100.
//   - Past expiry: GetCurrentValue("Health") == 80 again.
//   Base value stays at 80 throughout — duration effects don't shift
//   the persistent base.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeModifier.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class GameplayEffectExpiresAndRevertsAttributeTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Target = GetScene().CreateEntity("Target");
        auto& ac = m_Target.AddComponent<AbilityComponent>();
        ac.Attributes.DefineAttribute("Health", 80.0f);

        GameplayEffect bless;
        bless.Name = "Bless";
        bless.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
        bless.Policy.DurationSeconds = 0.4f;
        bless.Policy.IsPeriodic = false; // one-shot modifier for the duration

        GameplayEffect::AttributeMod mod;
        mod.AttributeName = "Health";
        mod.Op = AttributeModifier::Operation::Add;
        mod.Magnitude = 20.0f;
        bless.Modifiers.push_back(mod);

        const GameplayTag sourceTag{ "Source.Cleric" };
        ASSERT_TRUE(ac.ActiveEffects.ApplyEffect(bless, ac.OwnedTags, sourceTag));
    }

    Entity m_Target;
};

TEST_F(GameplayEffectExpiresAndRevertsAttributeTest, DurationEffectModifiesCurrentValueThenRevertsOnExpiry)
{
    auto& ac = m_Target.GetComponent<AbilityComponent>();

    // Before any tick: ApplyEffect added the entry but Tick hasn't run yet,
    // so the modifier hasn't been pushed onto the AttributeSet stack.
    // Current value still equals base.
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), 80.0f, 1e-3f);

    // Tick once: ModifiersApplied flips to true, +20 modifier lands.
    RunFrames(1);
    EXPECT_NEAR(ac.Attributes.GetBaseValue("Health"), 80.0f, 1e-3f)
        << "duration effect mutated the BASE value — modifier was applied "
           "via SetBaseValue instead of through AddModifier.";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), 100.0f, 1e-3f)
        << "duration effect didn't bump current value during the active window.";

    // Tick well past 0.4s. The effect should be removed and the modifier reverted.
    TickFor(0.5f);
    EXPECT_NEAR(ac.Attributes.GetBaseValue("Health"), 80.0f, 1e-3f);
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), 80.0f, 1e-3f)
        << "duration effect's modifier wasn't removed on expiry — current "
           "value still elevated. The cleanup path in ActiveEffectsContainer::"
           "Tick is failing to call RemoveModifiersBySource on expiry.";
}
