#include "OloEnginePCH.h"

// =============================================================================
// PeriodicEffectStackingScalesDamageTest — Functional Test.
//
// Cross-subsystem seam under test:
//   ActiveEffectsContainer::ApplyEffect (stacking branch) × Scene tick ×
//   ApplyPeriodicTick (CurrentStacks-scaled magnitude). Re-applying an
//   effect with the same Name bumps CurrentStacks up to MaxStacks. On
//   each PeriodSeconds boundary, ApplyPeriodicTick scales the modifier
//   magnitude by `CurrentStacks`. So three stacks of a Bleed (Add -5
//   Health, period 0.1s) deduct 15 per period, not 5. A regression
//   that forgets to multiply by stack count makes every DOT cap at
//   single-stack damage regardless of stacks.
//
// Scenario: apply the same Bleed effect three times in a row (each
// bumps CurrentStacks by 1 up to MaxStacks=3). Tick one period
// (0.12s, slightly past 0.1s boundary). Health should drop by 15
// (3 stacks × 5 magnitude), NOT 5.
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

class PeriodicEffectStackingScalesDamageTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Victim = GetScene().CreateEntity("Victim");
        auto& ac = m_Victim.AddComponent<AbilityComponent>();
        ac.Attributes.DefineAttribute("Health", 100.0f);

        // Bleed: periodic Add(-5) Health, period 0.1s, duration 5s, stacks
        // up to 3. RefreshDurationOnStack defaults to true so the duration
        // resets to 5s on each re-application — keeps the test from being
        // sensitive to ordering.
        GameplayEffect bleed;
        bleed.Name = "Bleed";
        bleed.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
        bleed.Policy.DurationSeconds = 5.0f;
        bleed.Policy.IsPeriodic = true;
        bleed.Policy.PeriodSeconds = 0.1f;
        bleed.MaxStacks = 3;

        GameplayEffect::AttributeMod mod;
        mod.AttributeName = "Health";
        mod.Op = AttributeModifier::Operation::Add;
        mod.Magnitude = -5.0f;
        bleed.Modifiers.push_back(mod);

        const GameplayTag source{ "Source.Wound" };

        // Apply three times → CurrentStacks goes 1, 2, 3.
        ASSERT_TRUE(ac.ActiveEffects.ApplyEffect(bleed, ac.OwnedTags, source));
        ASSERT_TRUE(ac.ActiveEffects.ApplyEffect(bleed, ac.OwnedTags, source));
        ASSERT_TRUE(ac.ActiveEffects.ApplyEffect(bleed, ac.OwnedTags, source));
    }

    Entity m_Victim;
};

TEST_F(PeriodicEffectStackingScalesDamageTest, ThreeStacksDeductFifteenPerPeriodNotFive)
{
    auto& ac = m_Victim.GetComponent<AbilityComponent>();
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), 100.0f, 1e-3f)
        << "ApplyEffect itself deducted health — periodic effects should "
           "not apply on application, only on subsequent period boundaries.";

    // Tick past one period boundary (0.1s). The single ApplyPeriodicTick
    // should subtract 3 stacks × 5 magnitude = 15. Allow slack for
    // accumulator drift: at 1/60s per frame, ~7 frames covers 0.117s, so
    // exactly one period boundary triggers.
    TickFor(/*seconds=*/0.12f);
    const f32 health = ac.Attributes.GetCurrentValue("Health");

    EXPECT_LE(health, 100.0f - 14.0f)
        << "after one period, Health dropped by less than expected (got "
        << health << ", expected ≈ 85). Either CurrentStacks isn't being "
           "scaled into the magnitude, or fewer ticks fired than expected.";
    EXPECT_GE(health, 100.0f - 16.0f)
        << "Health dropped by MORE than expected (got " << health
        << "). More than one period boundary fired in 0.12s — period "
           "accounting wraps without the `<= PeriodTimer` guard.";
}
