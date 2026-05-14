#include "OloEnginePCH.h"

// =============================================================================
// DamageKillsEntityAndFlipsDeathTagTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayAbilitySystem::ApplyDamage × AttributeSet (Health) × Scene tick
//   × GameplayAbilitySystem::OnUpdate (death-state transition).
//   ApplyDamage routes through DamageCalculation to deduct Health from the
//   target's AttributeSet base value. The PER-TICK update in GAS::OnUpdate
//   then notices Health <= 0 + State.Alive, removes State.Alive, and adds
//   State.Dead. Both halves must agree: a regression that drops the
//   tag-flip block silently turns "killed" enemies into immortal ragdolls.
//
// Scenario: Player and Goblin each have AbilityComponent + the four RPG
// attributes via InitializeDefaultRPGAttributes. Both start with
// State.Alive. ApplyDamage from Player → Goblin deducts MaxHealth + 10
// in one event (overkill). After one Scene tick:
//   - Goblin.Health == 0
//   - State.Alive is gone from Goblin
//   - State.Dead is present on Goblin
//   - Player tags are unchanged (sanity)
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class DamageKillsEntityAndFlipsDeathTagTest : public FunctionalTest
{
  protected:
    GameplayTag m_AliveTag{ "State.Alive" };
    GameplayTag m_DeadTag{ "State.Dead" };

    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Player");
        auto& playerAC = m_Player.AddComponent<AbilityComponent>();
        playerAC.InitializeDefaultRPGAttributes(/*maxHealth=*/100.0f, /*maxMana=*/50.0f,
                                                 /*attackPower=*/20.0f, /*defense=*/5.0f);
        playerAC.OwnedTags.AddTag(m_AliveTag);

        m_Goblin = GetScene().CreateEntity("Goblin");
        auto& goblinAC = m_Goblin.AddComponent<AbilityComponent>();
        goblinAC.InitializeDefaultRPGAttributes(/*maxHealth=*/50.0f, /*maxMana=*/0.0f,
                                                 /*attackPower=*/8.0f, /*defense=*/0.0f);
        goblinAC.OwnedTags.AddTag(m_AliveTag);
    }

    Entity m_Player;
    Entity m_Goblin;
};

TEST_F(DamageKillsEntityAndFlipsDeathTagTest, OverkillDamageDropsHealthAndScheduleNextTickFlipsTags)
{
    // Sanity preconditions.
    auto& goblinAC = m_Goblin.GetComponent<AbilityComponent>();
    ASSERT_NEAR(goblinAC.Attributes.GetCurrentValue("Health"), 50.0f, 1e-3f);
    ASSERT_TRUE(goblinAC.OwnedTags.HasTagExact(m_AliveTag));
    ASSERT_FALSE(goblinAC.OwnedTags.HasTagExact(m_DeadTag));

    // Deliver an overkill damage event. Defense=0 so the post-mitigation
    // value should equal the raw amount; we add slack via the +10 buffer
    // so this test isn't tightly coupled to DamageCalculation tuning.
    DamageEvent event;
    event.Source = m_Player;
    event.Target = m_Goblin;
    event.RawDamage = 60.0f;
    GameplayAbilitySystem::ApplyDamage(&GetScene(), event);

    // Right after ApplyDamage, Health is already <= 0 (base-value setter
    // clamps at 0 inside the system). But the tag-flip runs only inside
    // OnUpdate, which is what Scene::OnUpdateRuntime drives. Pre-tick
    // assertion: tags haven't flipped yet.
    EXPECT_LE(goblinAC.Attributes.GetCurrentValue("Health"), 0.0f)
        << "ApplyDamage didn't deduct enough Health — DamageCalculation may "
           "be applying unexpected mitigation, or BaseValue setter clamped "
           "the input incorrectly.";

    // One tick is enough — the death-check block runs unconditionally inside
    // GameplayAbilitySystem::OnUpdate, no time-based delay.
    RunFrames(1);

    EXPECT_FALSE(goblinAC.OwnedTags.HasTagExact(m_AliveTag))
        << "State.Alive was not removed from a 0-HP entity after one tick — "
           "GAS::OnUpdate skipped the death-transition block, or the death "
           "check no longer fires when ApplyDamage already clamped Health.";
    EXPECT_TRUE(goblinAC.OwnedTags.HasTagExact(m_DeadTag))
        << "State.Dead was not added after the Alive→Dead transition; the "
           "block is incomplete (removes one tag without adding the other).";

    // Player wasn't damaged, so its tags should be untouched.
    auto& playerAC = m_Player.GetComponent<AbilityComponent>();
    EXPECT_TRUE(playerAC.OwnedTags.HasTagExact(m_AliveTag));
    EXPECT_FALSE(playerAC.OwnedTags.HasTagExact(m_DeadTag));
    EXPECT_NEAR(playerAC.Attributes.GetCurrentValue("Health"), 100.0f, 1e-3f);
}
