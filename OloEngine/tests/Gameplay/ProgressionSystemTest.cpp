// OLO_TEST_LAYER: unit
// =============================================================================
// ProgressionSystemTest.cpp
//
// Unit tests for ProgressionSystem (issue #635) against the ENGINE-DEFAULT
// experience curve (no asset manager mounted, every asset handle 0, so
// ResolveAsset degrades to nullptr and XPForLevelUp uses
// ExperienceCurve::DefaultXPForLevelUp — a linear 100 XP per current level,
// max level 99).
//
// Covered here: PendingXP drain with single- and multi-level-up overflow
// carry, per-level point grants (defaults 5 AP / 1 SP), the batched
// ExperienceGained / LevelUp events, heal-on-level-up, the QuestJournal
// level/class sync, the attribute-point spend/refund/respec invariants
// (including exact "Progression.AllocatedPoints" modifier bookkeeping and
// composition with a foreign "Buff.Test" modifier), AddPendingXP saturation
// and the default-curve level-99 cap discard.
//
// ProgressionSystem::OnUpdate is called directly (the same entry point the
// "Progression" scheduler node drives every Scene::OnUpdateRuntime tick);
// the full scheduler path is covered by the Functional progression tests.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Progression/ExperienceCurve.h"
#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
#include "OloEngine/Gameplay/Progression/ProgressionEvents.h"
#include "OloEngine/Gameplay/Progression/ProgressionSystem.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    i32 CountModifiersFromSource(const AttributeSet& attrs, const std::string& attribute, const char* source)
    {
        i32 count = 0;
        for (const auto& mod : attrs.GetModifiers(attribute))
        {
            if (mod.Source.MatchesExact(GameplayTag{ source }))
            {
                ++count;
            }
        }
        return count;
    }

    f32 SumModifierMagnitudeFromSource(const AttributeSet& attrs, const std::string& attribute, const char* source)
    {
        f32 sum = 0.0f;
        for (const auto& mod : attrs.GetModifiers(attribute))
        {
            if (mod.Source.MatchesExact(GameplayTag{ source }))
            {
                sum += mod.Magnitude;
            }
        }
        return sum;
    }
} // namespace

class ProgressionSystemTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Scene = Scene::Create();
        m_Scene->SetRenderingEnabled(false);
    }

    void Tick()
    {
        ProgressionSystem::OnUpdate(m_Scene.Raw(), 1.0f / 60.0f);
    }

    Entity MakeCharacter(const char* name = "Character")
    {
        Entity entity = m_Scene->CreateEntity(name);
        entity.AddComponent<ProgressionComponent>();
        return entity;
    }

    Ref<Scene> m_Scene;
};

// ============================================================================
// XP drain + level-up resolution
// ============================================================================

TEST_F(ProgressionSystemTest, SingleLevelUpCarriesOverflowXP)
{
    Entity entity = MakeCharacter();
    auto& comp = entity.GetComponent<ProgressionComponent>();

    // Default curve: L1 -> L2 needs 100. Grant 150: level up, carry 50.
    ProgressionSystem::GrantExperience(m_Scene.Raw(), entity, 150);
    EXPECT_EQ(comp.PendingXP, 150) << "GrantExperience must queue into PendingXP, not resolve inline";
    EXPECT_EQ(comp.Level, 1) << "no level-up may happen before the Progression tick";

    Tick();

    EXPECT_EQ(comp.PendingXP, 0) << "the tick must drain PendingXP completely";
    EXPECT_EQ(comp.Level, 2) << "150 XP at L1 (needs 100) must reach level 2";
    EXPECT_EQ(comp.CurrentXP, 50) << "the 50 XP overflow past the level-up must carry";
    EXPECT_EQ(comp.AttributePoints, ProgressionSystem::kDefaultAttributePointsPerLevel)
        << "one class-less level-up must grant the default 5 attribute points";
    EXPECT_EQ(comp.SkillPoints, ProgressionSystem::kDefaultSkillPointsPerLevel)
        << "one class-less level-up must grant the default 1 skill point";
}

TEST_F(ProgressionSystemTest, MultiLevelUpCarriesOverflowAcrossLevels)
{
    Entity entity = MakeCharacter();
    auto& comp = entity.GetComponent<ProgressionComponent>();

    // Default curve 100*L: grant 300 at L1 -> pay 100 (L2, 200 left)
    // -> pay 200 (L3, 0 left) -> L3 needs 300 > 0, stop.
    ProgressionSystem::GrantExperience(m_Scene.Raw(), entity, 300);
    Tick();

    EXPECT_EQ(comp.Level, 3) << "300 XP must chain L1->L2 (100) then L2->L3 (200)";
    EXPECT_EQ(comp.CurrentXP, 0) << "300 - 100 - 200 leaves exactly 0 carried XP";
    EXPECT_EQ(comp.AttributePoints, 10) << "2 level-ups x 5 default AP = 10";
    EXPECT_EQ(comp.SkillPoints, 2) << "2 level-ups x 1 default SP = 2";
}

TEST_F(ProgressionSystemTest, EventsPublishedOnceAfterBatchResolve)
{
    Entity entity = MakeCharacter();
    const UUID entityId = entity.GetUUID();

    std::vector<ExperienceGainedEvent> xpEvents;
    std::vector<LevelUpEvent> levelEvents;
    m_Scene->GetGameplayEvents().Subscribe<ExperienceGainedEvent>(
        [&](const ExperienceGainedEvent& e)
        { xpEvents.push_back(e); });
    m_Scene->GetGameplayEvents().Subscribe<LevelUpEvent>(
        [&](const LevelUpEvent& e)
        { levelEvents.push_back(e); });

    ProgressionSystem::GrantExperience(m_Scene.Raw(), entity, 300);
    Tick();

    // One ExperienceGained with post-resolution values.
    ASSERT_EQ(xpEvents.size(), 1u) << "exactly one ExperienceGainedEvent per drained grant batch";
    EXPECT_EQ(static_cast<u64>(xpEvents[0].EntityID), static_cast<u64>(entityId));
    EXPECT_EQ(xpEvents[0].Amount, 300) << "Amount is the raw drained XP";
    EXPECT_EQ(xpEvents[0].Level, 3) << "Level is the post-resolution level";
    EXPECT_EQ(xpEvents[0].CurrentXP, 0) << "CurrentXP is the post-resolution carry";

    // One LevelUpEvent covering the whole multi-level jump.
    ASSERT_EQ(levelEvents.size(), 1u) << "a multi-level jump must publish ONE LevelUpEvent, not one per level";
    EXPECT_EQ(static_cast<u64>(levelEvents[0].EntityID), static_cast<u64>(entityId));
    EXPECT_EQ(levelEvents[0].PreviousLevel, 1);
    EXPECT_EQ(levelEvents[0].NewLevel, 3);
    EXPECT_EQ(levelEvents[0].AttributePointsGained, 10) << "the event carries the batch's total AP";
    EXPECT_EQ(levelEvents[0].SkillPointsGained, 2) << "the event carries the batch's total SP";

    // A tick with no pending XP must publish nothing further.
    Tick();
    EXPECT_EQ(xpEvents.size(), 1u) << "an idle tick must not publish ExperienceGained";
    EXPECT_EQ(levelEvents.size(), 1u) << "an idle tick must not publish LevelUp";
}

TEST_F(ProgressionSystemTest, GrantExperienceRejectsNonPositiveAmounts)
{
    Entity entity = MakeCharacter();
    auto& comp = entity.GetComponent<ProgressionComponent>();

    ProgressionSystem::GrantExperience(m_Scene.Raw(), entity, 0);
    ProgressionSystem::GrantExperience(m_Scene.Raw(), entity, -50);
    EXPECT_EQ(comp.PendingXP, 0) << "zero/negative grants must be complete no-ops";

    Tick();
    EXPECT_EQ(comp.Level, 1) << "nothing to resolve";
    EXPECT_EQ(comp.CurrentXP, 0);
}

TEST_F(ProgressionSystemTest, AddPendingXPSaturatesAtTwoBillion)
{
    ProgressionComponent comp;
    comp.AddPendingXP(2000000000);
    EXPECT_EQ(comp.PendingXP, 2000000000);
    comp.AddPendingXP(2000000000);
    EXPECT_EQ(comp.PendingXP, 2000000000)
        << "a second 2e9 grant must saturate at the 2e9 ceiling, not overflow i32";
    comp.AddPendingXP(0);
    comp.AddPendingXP(-1);
    EXPECT_EQ(comp.PendingXP, 2000000000) << "non-positive amounts are no-ops even at the ceiling";
}

TEST_F(ProgressionSystemTest, DefaultCurveCapDiscardsLeftoverXPAtMaxLevel)
{
    Entity entity = MakeCharacter();
    auto& comp = entity.GetComponent<ProgressionComponent>();

    // Total XP from L1 to the default cap L99: sum_{L=1}^{98} 100*L
    // = 100 * (98 * 99 / 2) = 485100. Grant 777 beyond that.
    ProgressionSystem::GrantExperience(m_Scene.Raw(), entity, 485100 + 777);
    Tick();

    EXPECT_EQ(comp.Level, ExperienceCurve::kDefaultMaxLevel)
        << "the default curve caps at level 99";
    EXPECT_EQ(comp.CurrentXP, 0)
        << "the 777 XP past the cap must be DISCARDED (zeroed), not banked";
    EXPECT_EQ(ProgressionSystem::GetXPToNextLevel(m_Scene.Raw(), entity), 0)
        << "at the cap there is no next level";
    EXPECT_EQ(ProgressionSystem::GetMaxLevel(m_Scene.Raw(), entity), ExperienceCurve::kDefaultMaxLevel);
    EXPECT_EQ(comp.AttributePoints, 98 * ProgressionSystem::kDefaultAttributePointsPerLevel)
        << "98 level-ups x 5 AP";
    EXPECT_EQ(comp.SkillPoints, 98 * ProgressionSystem::kDefaultSkillPointsPerLevel)
        << "98 level-ups x 1 SP";
}

TEST_F(ProgressionSystemTest, GetXPToNextLevelReflectsRemainingXP)
{
    Entity entity = MakeCharacter();

    EXPECT_EQ(ProgressionSystem::GetXPToNextLevel(m_Scene.Raw(), entity), 100)
        << "fresh L1 character on the default curve needs 100 XP";

    ProgressionSystem::GrantExperience(m_Scene.Raw(), entity, 30);
    Tick();

    EXPECT_EQ(entity.GetComponent<ProgressionComponent>().Level, 1) << "30 XP is not a level-up";
    EXPECT_EQ(ProgressionSystem::GetXPToNextLevel(m_Scene.Raw(), entity), 70)
        << "100 needed - 30 banked = 70 remaining";
}

// ============================================================================
// Heal-on-level-up
// ============================================================================

TEST_F(ProgressionSystemTest, HealOnLevelUpRestoresHealthToMaxHealthCurrent)
{
    // Healing entity: HealOnLevelUp defaults to true.
    Entity healer = MakeCharacter("Healer");
    auto& healerAC = healer.AddComponent<AbilityComponent>();
    healerAC.Attributes.DefineAttribute("MaxHealth", 100.0f);
    healerAC.Attributes.DefineAttribute("Health", 100.0f);
    healerAC.Attributes.SetBaseValue("Health", 40.0f); // wounded

    // Control entity: identical but opted out.
    Entity stoic = MakeCharacter("Stoic");
    stoic.GetComponent<ProgressionComponent>().HealOnLevelUp = false;
    auto& stoicAC = stoic.AddComponent<AbilityComponent>();
    stoicAC.Attributes.DefineAttribute("MaxHealth", 100.0f);
    stoicAC.Attributes.DefineAttribute("Health", 100.0f);
    stoicAC.Attributes.SetBaseValue("Health", 40.0f);

    ProgressionSystem::GrantExperience(m_Scene.Raw(), healer, 150);
    ProgressionSystem::GrantExperience(m_Scene.Raw(), stoic, 150);
    Tick();

    ASSERT_EQ(healer.GetComponent<ProgressionComponent>().Level, 2);
    ASSERT_EQ(stoic.GetComponent<ProgressionComponent>().Level, 2);
    EXPECT_NEAR(healerAC.Attributes.GetBaseValue("Health"), 100.0f, 1e-4f)
        << "HealOnLevelUp must set Health's BASE to MaxHealth's CURRENT value on level-up";
    EXPECT_NEAR(stoicAC.Attributes.GetBaseValue("Health"), 40.0f, 1e-4f)
        << "HealOnLevelUp=false must leave Health untouched by the level-up";
}

// ============================================================================
// QuestJournal sync
// ============================================================================

TEST_F(ProgressionSystemTest, JournalSyncsLevelAndClassEachTick)
{
    Entity entity = MakeCharacter();
    auto& comp = entity.GetComponent<ProgressionComponent>();
    comp.ClassID = "warrior"; // sync uses the raw ClassID even with no class DB resolvable
    auto& journal = entity.AddComponent<QuestJournalComponent>().Journal;

    ASSERT_EQ(journal.GetPlayerLevel(), 0) << "journal starts un-fed";
    ASSERT_TRUE(journal.GetPlayerClass().empty());

    Tick();
    EXPECT_EQ(journal.GetPlayerLevel(), 1) << "the Progression tick must push the level into the journal";
    EXPECT_EQ(journal.GetPlayerClass(), "warrior") << "the Progression tick must push the class id";

    ProgressionSystem::GrantExperience(m_Scene.Raw(), entity, 150);
    Tick();
    EXPECT_EQ(journal.GetPlayerLevel(), 2)
        << "the journal must track the earned level so Level>=N quest gates open";
}

// ============================================================================
// Attribute-point spend / refund / respec
// ============================================================================

TEST_F(ProgressionSystemTest, SpendAttributePointAppliesSingleModifierPerSourceAndComposesWithForeignModifier)
{
    Entity entity = MakeCharacter();
    auto& comp = entity.GetComponent<ProgressionComponent>();
    auto& ac = entity.AddComponent<AbilityComponent>();
    ac.Attributes.DefineAttribute("Strength", 10.0f);
    comp.AttributePoints = 10;

    // Foreign modifier from an unrelated source must never be disturbed.
    AttributeModifier buff;
    buff.Op = AttributeModifier::Operation::Add;
    buff.Magnitude = 5.0f;
    buff.Source = GameplayTag{ "Buff.Test" };
    ac.Attributes.AddModifier("Strength", buff);

    EXPECT_TRUE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Strength", 3));

    EXPECT_EQ(comp.AttributePoints, 7) << "spend must deduct from the pool";
    ASSERT_TRUE(comp.AllocatedPoints.contains("Strength"));
    EXPECT_EQ(comp.AllocatedPoints.at("Strength"), 3) << "allocation record must track the spend";
    // Class-less spend rate is 1.0 per point: exactly ONE Add modifier of
    // magnitude 3 from the allocated-points source (remove-then-add).
    EXPECT_EQ(CountModifiersFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource), 1)
        << "spend must maintain exactly one modifier from Progression.AllocatedPoints";
    EXPECT_NEAR(SumModifierMagnitudeFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource),
                3.0f, 1e-4f)
        << "3 points x 1.0/point (class-less rate)";
    EXPECT_EQ(CountModifiersFromSource(ac.Attributes, "Strength", "Buff.Test"), 1)
        << "the foreign Buff.Test modifier must be untouched by the spend";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Strength"), 18.0f, 1e-4f)
        << "10 base + 3 allocated + 5 buff";

    // Spend again: still exactly one modifier (remove-then-add), magnitude 5.
    EXPECT_TRUE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Strength", 2));
    EXPECT_EQ(comp.AttributePoints, 5);
    EXPECT_EQ(comp.AllocatedPoints.at("Strength"), 5);
    EXPECT_EQ(CountModifiersFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource), 1)
        << "a second spend must not stack a second modifier from the same source";
    EXPECT_NEAR(SumModifierMagnitudeFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource),
                5.0f, 1e-4f);
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Strength"), 20.0f, 1e-4f) << "10 + 5 + 5";
}

TEST_F(ProgressionSystemTest, RefundAndRespecRestorePointsAndPreserveForeignModifier)
{
    Entity entity = MakeCharacter();
    auto& comp = entity.GetComponent<ProgressionComponent>();
    auto& ac = entity.AddComponent<AbilityComponent>();
    ac.Attributes.DefineAttribute("Strength", 10.0f);
    comp.AttributePoints = 10;

    AttributeModifier buff;
    buff.Op = AttributeModifier::Operation::Add;
    buff.Magnitude = 5.0f;
    buff.Source = GameplayTag{ "Buff.Test" };
    ac.Attributes.AddModifier("Strength", buff);

    ASSERT_TRUE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Strength", 3));
    ASSERT_EQ(comp.AttributePoints, 7);

    // Partial refund.
    EXPECT_TRUE(ProgressionSystem::RefundAttributePoint(m_Scene.Raw(), entity, "Strength", 1));
    EXPECT_EQ(comp.AttributePoints, 8) << "refund must return the point to the pool";
    EXPECT_EQ(comp.AllocatedPoints.at("Strength"), 2);
    EXPECT_EQ(CountModifiersFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource), 1);
    EXPECT_NEAR(SumModifierMagnitudeFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource),
                2.0f, 1e-4f)
        << "refund must shrink the single source modifier to the remaining 2 points";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Strength"), 17.0f, 1e-4f) << "10 + 2 + 5";

    // Refund the rest: the allocation entry and its modifier must vanish.
    EXPECT_TRUE(ProgressionSystem::RefundAttributePoint(m_Scene.Raw(), entity, "Strength", 2));
    EXPECT_EQ(comp.AttributePoints, 10);
    EXPECT_FALSE(comp.AllocatedPoints.contains("Strength"))
        << "a fully-refunded attribute must leave no stale allocation entry";
    EXPECT_EQ(CountModifiersFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource), 0)
        << "a fully-refunded attribute must leave no stale modifier";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Strength"), 15.0f, 1e-4f) << "10 base + 5 buff only";

    // Spend again, then respec everything.
    ASSERT_TRUE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Strength", 4));
    ASSERT_EQ(comp.AttributePoints, 6);
    const i32 refunded = ProgressionSystem::RespecAttributes(m_Scene.Raw(), entity);
    EXPECT_EQ(refunded, 4) << "respec must report the total points returned";
    EXPECT_EQ(comp.AttributePoints, 10) << "respec must return every allocated point";
    EXPECT_TRUE(comp.AllocatedPoints.empty()) << "respec must clear the allocation map";
    EXPECT_EQ(CountModifiersFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource), 0)
        << "respec must remove the whole allocated-points source";
    EXPECT_EQ(CountModifiersFromSource(ac.Attributes, "Strength", "Buff.Test"), 1)
        << "respec must never touch a foreign source's modifiers";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Strength"), 15.0f, 1e-4f) << "10 base + 5 buff";
}

TEST_F(ProgressionSystemTest, InvalidSpendAndRefundAreRejectedWithStateUnchanged)
{
    Entity entity = MakeCharacter();
    auto& comp = entity.GetComponent<ProgressionComponent>();
    auto& ac = entity.AddComponent<AbilityComponent>();
    ac.Attributes.DefineAttribute("Strength", 10.0f);
    comp.AttributePoints = 10;

    auto expectUnchanged = [&](const char* context)
    {
        EXPECT_EQ(comp.AttributePoints, 10) << context << ": pool must be unchanged";
        EXPECT_TRUE(comp.AllocatedPoints.empty()) << context << ": allocation map must be unchanged";
        EXPECT_EQ(CountModifiersFromSource(ac.Attributes, "Strength", ProgressionSystem::kAllocatedPointsSource), 0)
            << context << ": no modifier may appear";
        EXPECT_NEAR(ac.Attributes.GetCurrentValue("Strength"), 10.0f, 1e-4f)
            << context << ": current value must be unchanged";
    };

    EXPECT_FALSE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Strength", 20));
    expectUnchanged("overspend (20 > pool of 10)");

    EXPECT_FALSE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Agility", 1));
    expectUnchanged("spend on an attribute not defined on the AttributeSet");

    EXPECT_FALSE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Strength", 0));
    EXPECT_FALSE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Strength", -2));
    expectUnchanged("non-positive count");

    EXPECT_FALSE(ProgressionSystem::RefundAttributePoint(m_Scene.Raw(), entity, "Strength", 1));
    expectUnchanged("refund with nothing allocated");

    ASSERT_TRUE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), entity, "Strength", 3));
    EXPECT_FALSE(ProgressionSystem::RefundAttributePoint(m_Scene.Raw(), entity, "Strength", 5))
        << "refunding more than the 3 allocated must be rejected";
    EXPECT_EQ(comp.AttributePoints, 7) << "failed over-refund must not move the pool";
    EXPECT_EQ(comp.AllocatedPoints.at("Strength"), 3) << "failed over-refund must not move the allocation";

    // No AbilityComponent at all -> spend rejected.
    Entity noAC = MakeCharacter("NoAbilityComponent");
    noAC.GetComponent<ProgressionComponent>().AttributePoints = 5;
    EXPECT_FALSE(ProgressionSystem::SpendAttributePoint(m_Scene.Raw(), noAC, "Strength", 1))
        << "spend without an AbilityComponent must be rejected";
    EXPECT_EQ(noAC.GetComponent<ProgressionComponent>().AttributePoints, 5);

    // Respec with an empty allocation returns 0.
    EXPECT_EQ(ProgressionSystem::RespecAttributes(m_Scene.Raw(), noAC), 0)
        << "respec with nothing allocated must return 0";
}
