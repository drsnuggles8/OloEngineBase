#include "OloEnginePCH.h"

// =============================================================================
// PeriodicDamageEffectTicksHealthDownTest — Functional Test.
//
// Cross-subsystem seam under test:
//   ActiveEffectsContainer.ApplyEffect × ActiveEffectsContainer.Tick (via
//   GameplayAbilitySystem::OnUpdate) × Scene tick × AttributeSet base value.
//   A periodic effect (DOT) doesn't apply on activation — its modifiers
//   land on each `Tick(dt)` boundary spaced `PeriodSeconds` apart. The
//   Scene tick is what drives that timer forward. If the scene loop stops
//   calling AbilityComponent.ActiveEffects.Tick, or the period logic
//   miscounts overflow, the DOT either does nothing or one-shots.
//
// Scenario: apply a periodic Add(-5) Health effect with Period=0.1s and
// HasDuration=0.5s. Tick the scene for ~0.55s. The DOT should have fired
// 5 ticks (one per period boundary), deducting 25 from a 100-Health
// entity → final Health ≈ 75. We assert against a window because the
// last partial period may or may not have fired depending on float
// arithmetic.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Effects/GameplayEffect.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeModifier.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTagContainer.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class PeriodicDamageEffectTicksHealthDownTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Victim = GetScene().CreateEntity("Victim");
        auto& ac = m_Victim.AddComponent<AbilityComponent>();
        ac.InitializeDefaultRPGAttributes(/*maxHealth=*/100.0f, /*maxMana=*/0.0f,
                                          /*attackPower=*/0.0f, /*defense=*/0.0f);

        // Build the DOT: Add -5 Health per 0.1s for 0.5s total.
        GameplayEffect dot;
        dot.Name = "Poison";
        dot.Policy.DurationType = GameplayEffectPolicy::Duration::HasDuration;
        dot.Policy.DurationSeconds = 0.5f;
        dot.Policy.IsPeriodic = true;
        dot.Policy.PeriodSeconds = 0.1f;

        GameplayEffect::AttributeMod healthMod;
        healthMod.AttributeName = "Health";
        healthMod.Op = AttributeModifier::Operation::Add;
        healthMod.Magnitude = -5.0f;
        dot.Modifiers.push_back(healthMod);

        // ApplyEffect needs the owner's tag container and a source tag for
        // bookkeeping. The target-tag check is empty so the effect always
        // applies; SourceTag identifies who applied it.
        const GameplayTag sourceTag{ "Source.PoisonTrap" };
        const bool applied = ac.ActiveEffects.ApplyEffect(dot, ac.OwnedTags, sourceTag);
        ASSERT_TRUE(applied) << "ApplyEffect refused the DOT — RequiredTags / BlockedTags rejected it.";
    }

    Entity m_Victim;
};

TEST_F(PeriodicDamageEffectTicksHealthDownTest, HealthDropsByOneStackPerPeriodAcrossTicks)
{
    auto& ac = m_Victim.GetComponent<AbilityComponent>();

    // Initially full health — periodic effects don't fire on application,
    // only on the first period boundary inside Tick.
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), 100.0f, 1e-3f);

    // Tick a hair past 0.55s. At 1/60s per frame, 33 frames ≈ 0.55s. The
    // effect's duration is 0.5s, so by ~0.5s it expires; the last partial
    // period is bounded by Duration. We expect 4–6 periodic ticks to have
    // fired (allowing slack for accumulation precision near the boundary).
    TickFor(/*seconds=*/0.55f);

    const f32 health = ac.Attributes.GetCurrentValue("Health");
    EXPECT_LE(health, 100.0f - 4.0f * 5.0f) // at least 4 ticks → ≤ 80
        << "DOT did not deduct enough Health (got " << health
        << "); fewer than 4 periodic ticks fired across 0.55s of simulated time. "
           "Either Scene tick isn't driving ActiveEffects.Tick, or PeriodSeconds "
           "is being interpreted wrong (e.g., one tick per second).";
    EXPECT_GE(health, 100.0f - 6.0f * 5.0f) // no more than 6 ticks → ≥ 70
        << "DOT deducted MORE than it should have (got " << health
        << "); periodic effect is firing faster than its declared PeriodSeconds.";

    // After the effect's duration expires, no further ticks should fire.
    const f32 healthAfterExpiry = ac.Attributes.GetCurrentValue("Health");
    TickFor(/*seconds=*/0.5f);
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), healthAfterExpiry, 1e-3f)
        << "DOT kept firing after RemainingDuration hit zero — the effect "
           "wasn't removed from m_ActiveEffects on expiry.";
}
