#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// KillGrantsExperienceViaDamageTest — Functional Test.
//
// Cross-subsystem seam under test:
//   GameplayAbilitySystem::ApplyDamage (the single damage choke point) ×
//   ProgressionComponent (the victim's XPBounty flowing into the killer's
//   PendingXP) × GameplayEventBus (EntityKilledEvent{victim, killer, xp}) ×
//   the Progression tick that resolves the bounty into a level-up.
//   ApplyDamage is the ONE place attacker and victim are simultaneously
//   known, so the kill hook lives there: on a >0 -> <=0 Health crossing it
//   flips State.Alive -> State.Dead immediately (when the victim opted into
//   the tag convention), publishes EntityKilledEvent, and credits the
//   victim's XPBounty to the killer — resolved by the next scene tick.
//
// Numbers (default DamageCalculation pipeline, no crit, no resistance):
//   finalDamage = RawDamage + killer AttackPower - victim Defense.
//   Killer AttackPower 20, victim Defense 0: Raw 50 -> 70 (lethal vs 30 HP),
//   Raw 5 -> 25 (non-lethal vs 30 HP).
//   Bounty 250 on the default curve: L1 needs 100 -> Level 2, carry 150.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Damage/CombatEvents.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

class KillGrantsExperienceViaDamageTest : public FunctionalTest
{
  protected:
    GameplayTag m_AliveTag{ "State.Alive" };
    GameplayTag m_DeadTag{ "State.Dead" };

    void BuildScene() override
    {
        m_Killer = GetScene().CreateEntity("Killer");
        auto& killerAC = m_Killer.AddComponent<AbilityComponent>();
        killerAC.InitializeDefaultRPGAttributes(/*maxHealth=*/100.0f, /*maxMana=*/0.0f,
                                                /*attackPower=*/20.0f, /*defense=*/0.0f);
        killerAC.OwnedTags.AddTag(m_AliveTag);
        m_Killer.AddComponent<ProgressionComponent>(); // handles 0 => default curve

        m_Victim = GetScene().CreateEntity("Victim");
        auto& victimAC = m_Victim.AddComponent<AbilityComponent>();
        victimAC.InitializeDefaultRPGAttributes(/*maxHealth=*/30.0f, /*maxMana=*/0.0f,
                                                /*attackPower=*/0.0f, /*defense=*/0.0f);
        victimAC.OwnedTags.AddTag(m_AliveTag);
        auto& victimProgression = m_Victim.AddComponent<ProgressionComponent>();
        victimProgression.XPBounty = 250;
    }

    [[nodiscard]] f32 Damage(Entity target, f32 rawDamage)
    {
        DamageEvent event;
        event.Source = m_Killer;
        event.Target = target;
        event.RawDamage = rawDamage;
        return GameplayAbilitySystem::ApplyDamage(&GetScene(), event);
    }

    Entity m_Killer;
    Entity m_Victim;
};

TEST_F(KillGrantsExperienceViaDamageTest, LethalDamageFlipsDeathTagPublishesKillEventAndGrantsBounty)
{
    std::vector<EntityKilledEvent> kills;
    GetScene().GetGameplayEvents().Subscribe<EntityKilledEvent>(
        [&](const EntityKilledEvent& e)
        { kills.push_back(e); });

    // finalDamage = 50 raw + 20 killer AttackPower - 0 defense = 70 > 30 HP.
    const f32 finalDamage = Damage(m_Victim, 50.0f);
    EXPECT_NEAR(finalDamage, 70.0f, 1e-3f)
        << "ApplyDamage must return the final calculated damage (raw + attack power)";

    // The tag flip is IMMEDIATE inside ApplyDamage (no tick needed).
    const auto& victimAC = m_Victim.GetComponent<AbilityComponent>();
    EXPECT_FALSE(victimAC.OwnedTags.HasTagExact(m_AliveTag))
        << "State.Alive must be removed immediately on the lethal hit";
    EXPECT_TRUE(victimAC.OwnedTags.HasTagExact(m_DeadTag))
        << "State.Dead must be added immediately on the lethal hit";

    ASSERT_EQ(kills.size(), 1u) << "exactly one EntityKilledEvent per kill";
    EXPECT_EQ(static_cast<u64>(kills[0].VictimID), static_cast<u64>(m_Victim.GetUUID()));
    EXPECT_EQ(static_cast<u64>(kills[0].KillerID), static_cast<u64>(m_Killer.GetUUID()));
    EXPECT_EQ(kills[0].ExperienceGranted, 250) << "the event must carry the granted bounty";

    const auto& killerProgression = m_Killer.GetComponent<ProgressionComponent>();
    EXPECT_EQ(killerProgression.PendingXP, 250)
        << "the victim's XPBounty must land in the killer's PendingXP";
    EXPECT_EQ(killerProgression.Level, 1) << "no level-up may happen before the Progression tick";

    // One scene tick resolves: default curve needs 100 at L1 -> Level 2, 150 carried.
    RunFrames(1);
    EXPECT_EQ(killerProgression.Level, 2) << "250 bounty XP must reach level 2 on the default curve";
    EXPECT_EQ(killerProgression.CurrentXP, 150) << "250 - 100 = 150 XP must carry";
    EXPECT_EQ(killerProgression.PendingXP, 0);
}

TEST_F(KillGrantsExperienceViaDamageTest, NonLethalDamagePublishesNothingAndGrantsNoXP)
{
    std::vector<EntityKilledEvent> kills;
    GetScene().GetGameplayEvents().Subscribe<EntityKilledEvent>(
        [&](const EntityKilledEvent& e)
        { kills.push_back(e); });

    // finalDamage = 5 raw + 20 AttackPower = 25 < 30 HP: victim survives.
    const f32 finalDamage = Damage(m_Victim, 5.0f);
    EXPECT_NEAR(finalDamage, 25.0f, 1e-3f);

    const auto& victimAC = m_Victim.GetComponent<AbilityComponent>();
    EXPECT_NEAR(victimAC.Attributes.GetCurrentValue("Health"), 5.0f, 1e-3f)
        << "30 - 25 = 5 HP must remain";
    EXPECT_TRUE(victimAC.OwnedTags.HasTagExact(m_AliveTag)) << "a surviving victim keeps State.Alive";
    EXPECT_FALSE(victimAC.OwnedTags.HasTagExact(m_DeadTag));

    EXPECT_TRUE(kills.empty()) << "a non-lethal hit must not publish EntityKilledEvent";
    EXPECT_EQ(m_Killer.GetComponent<ProgressionComponent>().PendingXP, 0)
        << "no bounty may be granted for a non-lethal hit";

    RunFrames(1);
    EXPECT_EQ(m_Killer.GetComponent<ProgressionComponent>().Level, 1) << "nothing to resolve";
}

TEST_F(KillGrantsExperienceViaDamageTest, VictimWithoutAliveTagDiesSilentlyButStillGrantsXP)
{
    // An entity that never opted into the State.Alive convention: the kill
    // detection is driven by the Health crossing, so the bounty + event still
    // fire, but NO death tag is flipped (the flip is gated on holding
    // State.Alive — verified against GameplayAbilitySystem::ApplyDamage).
    Entity tagless = GetScene().CreateEntity("TaglessVictim");
    auto& taglessAC = tagless.AddComponent<AbilityComponent>();
    taglessAC.InitializeDefaultRPGAttributes(/*maxHealth=*/30.0f, /*maxMana=*/0.0f,
                                             /*attackPower=*/0.0f, /*defense=*/0.0f);
    auto& taglessProgression = tagless.AddComponent<ProgressionComponent>();
    taglessProgression.XPBounty = 250;

    std::vector<EntityKilledEvent> kills;
    GetScene().GetGameplayEvents().Subscribe<EntityKilledEvent>(
        [&](const EntityKilledEvent& e)
        { kills.push_back(e); });

    const f32 finalDamage = Damage(tagless, 50.0f);
    EXPECT_NEAR(finalDamage, 70.0f, 1e-3f);

    EXPECT_NEAR(taglessAC.Attributes.GetCurrentValue("Health"), 0.0f, 1e-3f) << "the victim is dead";
    EXPECT_FALSE(taglessAC.OwnedTags.HasTagExact(m_DeadTag))
        << "no State.Dead may be added when the victim never held State.Alive (opt-in convention)";
    EXPECT_FALSE(taglessAC.OwnedTags.HasTagExact(m_AliveTag));

    ASSERT_EQ(kills.size(), 1u) << "the kill event fires regardless of the tag convention";
    EXPECT_EQ(static_cast<u64>(kills[0].VictimID), static_cast<u64>(tagless.GetUUID()));
    EXPECT_EQ(kills[0].ExperienceGranted, 250);
    EXPECT_EQ(m_Killer.GetComponent<ProgressionComponent>().PendingXP, 250)
        << "the bounty flows regardless of the tag convention";
}
