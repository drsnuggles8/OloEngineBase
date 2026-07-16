#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// ProgressionFullLoopTest — Functional Test.
//
// Cross-subsystem seam under test (the issue #635 acceptance loop):
//   AssetManager (memory-only ExperienceCurve / SkillTreeDatabase /
//   CharacterClassDatabase) × ProgressionSystem (class-ensure, XP drain,
//   level-up growth, attribute points, skill-tree unlocks) × QuestSystem
//   (completion-reward XP + Level>=N requirement gates fed through
//   QuestJournal) × GameplayAbilitySystem (passive-effect modifiers, ability
//   activation) × SaveGameSerializer / SceneSerializer round-trips — all
//   driven through real Scene::OnUpdateRuntime ticks (the "Progression"
//   scheduler node in the physics shadow, after "Quest").
//
// Scenario: a "warrior" class database wired to a table experience curve
// {100, 200, 300} capped at level 4 and a two-node skill tree (passive
// "toughness" +5 AttackPower -> ability "power_strike", level 2, prereq
// toughness). The player entity carries QuestJournal + Ability +
// Progression components. Each test case is self-contained (ctest runs
// every case in its own process) and fast-forwards via the shared helpers.
//
// Derived numbers used throughout (table curve, class grants 5 AP / 2 SP):
//   150 XP at L1 -> pay 100 -> Level 2, carry 50, +5 AP, +2 SP.
//   Growth at level L: MaxHealth +10*(L-1), AttackPower +2*(L-1).
//   AttackPower at L2 = 10 base + 2 growth (+1/point spent, +5 toughness).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Gameplay/Abilities/AbilityComponents.h"
#include "OloEngine/Gameplay/Abilities/GameplayAbilitySystem.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Progression/CharacterClassDatabase.h"
#include "OloEngine/Gameplay/Progression/ExperienceCurve.h"
#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
#include "OloEngine/Gameplay/Progression/ProgressionEvents.h"
#include "OloEngine/Gameplay/Progression/ProgressionSystem.h"
#include "OloEngine/Gameplay/Progression/SkillTreeDatabase.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"
#include "OloEngine/Gameplay/Quest/QuestRequirement.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // Single-stage kill quest; empty CompletionChoices => auto-completes when
    // the stage clears (same shape as QuestEventsEmittedTest's helper).
    QuestDefinition MakeKillQuest(const std::string& id, const std::string& objId,
                                  const std::string& target, i32 count)
    {
        QuestDefinition def;
        def.QuestID = id;
        def.Title = id;

        QuestStage stage;
        stage.StageID = "stage0";
        stage.RequireAllObjectives = true;
        QuestObjective obj;
        obj.ObjectiveID = objId;
        obj.ObjectiveType = QuestObjective::Type::Kill;
        obj.TargetID = target;
        obj.RequiredCount = count;
        stage.Objectives.push_back(std::move(obj));
        def.Stages.push_back(std::move(stage));
        return def;
    }
} // namespace

class ProgressionFullLoopTest : public FunctionalTest
{
  protected:
    static constexpr const char* kClassID = "warrior";
    static constexpr const char* kToughness = "toughness";
    static constexpr const char* kPowerStrike = "power_strike";
    static constexpr const char* kAbilityTag = "Ability.PowerStrike";

    void BuildScene() override
    {
        EnableAssetManager({});

        // --- Experience curve: table {100, 200, 300}, max level 4. ---
        auto curve = Ref<ExperienceCurve>::Create();
        curve->m_Mode = ExperienceCurve::CurveMode::Table;
        curve->m_Table = { 100, 200, 300 };
        curve->m_MaxLevel = 4;
        m_CurveHandle = AssetManager::AddMemoryOnlyAsset<ExperienceCurve>(curve);
        ASSERT_NE(static_cast<u64>(m_CurveHandle), 0u) << "memory-only curve got a zero handle";

        // --- Skill tree: toughness (passive +5 AttackPower) -> power_strike. ---
        auto tree = Ref<SkillTreeDatabase>::Create();
        tree->m_TreeID = "warrior_tree";
        {
            SkillTreeNode toughness;
            toughness.NodeID = kToughness;
            toughness.LevelRequirement = 1;
            toughness.SkillPointCost = 1;
            toughness.Payload = SkillTreeNode::PayloadKind::PassiveEffect;
            // Effect Name left EMPTY on purpose: unlock must default it to
            // "Skill.toughness" (the kSkillSourcePrefix contract).
            toughness.PassiveEffect.Policy.DurationType = GameplayEffectPolicy::Duration::Infinite;
            toughness.PassiveEffect.Modifiers.push_back(
                { "AttackPower", AttributeModifier::Operation::Add, 5.0f });
            tree->m_Nodes.push_back(std::move(toughness));

            SkillTreeNode strike;
            strike.NodeID = kPowerStrike;
            strike.LevelRequirement = 2;
            strike.SkillPointCost = 1;
            strike.Prerequisites = { kToughness };
            strike.Payload = SkillTreeNode::PayloadKind::Ability;
            strike.GrantedAbility.Name = "Power Strike";
            strike.GrantedAbility.AbilityTag = GameplayTag(kAbilityTag);
            strike.GrantedAbility.ResourceCost = 0.0f; // zero cost => activation always succeeds
            tree->m_Nodes.push_back(std::move(strike));
        }
        tree->RebuildIndex();
        if (std::string error; !tree->Validate(&error))
        {
            FAIL() << "fixture skill tree is invalid: " << error;
        }
        m_TreeHandle = AssetManager::AddMemoryOnlyAsset<SkillTreeDatabase>(tree);

        // --- Class database: the "warrior" archetype. ---
        auto classes = Ref<CharacterClassDatabase>::Create();
        {
            CharacterClassDefinition warrior;
            warrior.ClassID = kClassID;
            //                              base    growth  value/point
            warrior.Attributes.push_back({ "MaxHealth", 100.0f, 10.0f, 5.0f });
            warrior.Attributes.push_back({ "Health", 100.0f, 0.0f, 0.0f });
            warrior.Attributes.push_back({ "AttackPower", 10.0f, 2.0f, 1.0f });
            warrior.Attributes.push_back({ "MaxMana", 50.0f, 0.0f, 0.0f });
            warrior.Attributes.push_back({ "Mana", 50.0f, 0.0f, 0.0f });
            warrior.StartingTags = { "State.Alive" };
            warrior.SkillTrees = { m_TreeHandle };
            warrior.ExperienceCurve = m_CurveHandle;
            warrior.AttributePointsPerLevel = 5;
            warrior.SkillPointsPerLevel = 2;
            warrior.LevelCap = 0; // 0 => the curve's MaxLevel (4) governs
            classes->m_Classes.push_back(std::move(warrior));
        }
        classes->RebuildIndex();
        if (std::string error; !classes->Validate(&error))
        {
            FAIL() << "fixture class database is invalid: " << error;
        }
        m_ClassDbHandle = AssetManager::AddMemoryOnlyAsset<CharacterClassDatabase>(classes);

        // --- The player. ---
        m_Player = GetScene().CreateEntity("Hero");
        m_Player.AddComponent<QuestJournalComponent>();
        m_Player.AddComponent<AbilityComponent>();
        auto& pc = m_Player.AddComponent<ProgressionComponent>();
        pc.ClassDatabaseHandle = m_ClassDbHandle;
        pc.ClassID = kClassID;
    }

    ProgressionComponent& Progress()
    {
        return m_Player.GetComponent<ProgressionComponent>();
    }
    AbilityComponent& Abilities()
    {
        return m_Player.GetComponent<AbilityComponent>();
    }

    // Fast-forward: 150 XP at L1 (table needs 100) -> level 2 with 50 carry,
    // +5 AP, +2 SP. Runs the class-ensure first tick as a side effect.
    void LevelUpToTwo()
    {
        ProgressionSystem::GrantExperience(&GetScene(), m_Player, 150);
        RunFrames(1);
        ASSERT_EQ(Progress().Level, 2) << "fast-forward helper failed: 150 XP must reach level 2";
        ASSERT_EQ(Progress().CurrentXP, 50) << "fast-forward helper failed: 50 XP must carry";
    }

    [[nodiscard]] bool PlayerHasAbility(const char* tag)
    {
        for (const auto& ability : Abilities().Abilities)
        {
            if (ability.Definition.AbilityTag.MatchesExact(GameplayTag(tag)))
            {
                return true;
            }
        }
        return false;
    }

    Entity m_Player;
    AssetHandle m_CurveHandle{};
    AssetHandle m_TreeHandle{};
    AssetHandle m_ClassDbHandle{};
};

// ----------------------------------------------------------------------------
// (a) First tick: non-destructive class ensure.
// ----------------------------------------------------------------------------
TEST_F(ProgressionFullLoopTest, FirstTickDefinesClassAttributesTagsAndJournalClass)
{
    RunFrames(1);

    const auto& ac = Abilities();
    ASSERT_TRUE(ac.Attributes.HasAttribute("MaxHealth")) << "class ensure did not define MaxHealth";
    ASSERT_TRUE(ac.Attributes.HasAttribute("Health"));
    ASSERT_TRUE(ac.Attributes.HasAttribute("AttackPower"));
    ASSERT_TRUE(ac.Attributes.HasAttribute("Mana"));
    ASSERT_TRUE(ac.Attributes.HasAttribute("MaxMana"));
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("MaxHealth"), 100.0f, 1e-3f)
        << "level 1 => zero growth, MaxHealth must be the class base";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("AttackPower"), 10.0f, 1e-3f)
        << "level 1 => zero growth, AttackPower must be the class base";

    EXPECT_TRUE(ac.OwnedTags.HasTagExact(GameplayTag("State.Alive")))
        << "the class's StartingTags were not granted on the first tick";

    const auto& journal = m_Player.GetComponent<QuestJournalComponent>().Journal;
    EXPECT_EQ(journal.GetPlayerClass(), kClassID)
        << "the Progression tick must push the class id into the quest journal";
    EXPECT_EQ(journal.GetPlayerLevel(), 1) << "the Progression tick must push the level";

    EXPECT_TRUE(Progress().RuntimeInitialized) << "EnsureRuntimeState must mark the session initialized";
}

// ----------------------------------------------------------------------------
// (b) Quest-reward XP levels up and opens a Level>=2 requirement gate —
//     the issue's explicit acceptance criterion.
// ----------------------------------------------------------------------------
TEST_F(ProgressionFullLoopTest, QuestRewardXPLevelsUpAndUnlocksLevelGatedQuest)
{
    RunFrames(1); // class ensure + journal level 1

    // A quest gated on Level >= 2 must NOT be acceptable at level 1.
    QuestDefinition gated = MakeKillQuest("Q_Gated", "obj_g", "dragon", 1);
    {
        QuestRequirement levelReq;
        levelReq.Type = QuestRequirementType::Level;
        levelReq.Value = 2; // Comparison defaults to GreaterThanOrEqual
        gated.Requirements.push_back(std::move(levelReq));
    }
    EXPECT_FALSE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q_Gated", gated))
        << "a Level>=2 quest must be rejected while the journal reports level 1";

    // Complete a kill quest worth 150 XP through the IncrementObjective
    // auto-complete cascade (NotifyKill -> objective -> stage -> complete).
    std::vector<LevelUpEvent> levelUps;
    GetScene().GetGameplayEvents().Subscribe<LevelUpEvent>(
        [&](const LevelUpEvent& e)
        { levelUps.push_back(e); });

    QuestDefinition wolfQuest = MakeKillQuest("Q_Wolf", "obj_k", "wolf", 1);
    wolfQuest.CompletionRewards.ExperiencePoints = 150;
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q_Wolf", wolfQuest));

    QuestSystem::NotifyKill(&GetScene(), m_Player, "wolf");
    EXPECT_TRUE(m_Player.GetComponent<QuestJournalComponent>().Journal.HasCompletedQuest("Q_Wolf"))
        << "one wolf kill must auto-complete the quest";
    EXPECT_EQ(Progress().PendingXP, 150)
        << "the completion reward must land in PendingXP, resolved by the next Progression tick";

    RunFrames(1);

    const auto& comp = Progress();
    EXPECT_EQ(comp.Level, 2) << "150 XP on the {100,200,300} table must reach level 2";
    EXPECT_EQ(comp.CurrentXP, 50) << "150 - 100 = 50 XP must carry";
    EXPECT_EQ(comp.AttributePoints, 5) << "class grants 5 AP per level";
    EXPECT_EQ(comp.SkillPoints, 2) << "class grants 2 SP per level";

    const auto& ac = Abilities();
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("MaxHealth"), 110.0f, 1e-3f)
        << "MaxHealth growth: 100 base + 10 * (2 - 1)";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("Health"), 110.0f, 1e-3f)
        << "HealOnLevelUp must set Health to MaxHealth's post-growth current value";

    ASSERT_EQ(levelUps.size(), 1u) << "exactly one LevelUpEvent for the single-level jump";
    EXPECT_EQ(levelUps[0].PreviousLevel, 1);
    EXPECT_EQ(levelUps[0].NewLevel, 2);
    EXPECT_EQ(levelUps[0].AttributePointsGained, 5);
    EXPECT_EQ(levelUps[0].SkillPointsGained, 2);

    EXPECT_EQ(m_Player.GetComponent<QuestJournalComponent>().Journal.GetPlayerLevel(), 2)
        << "the journal must see the new level";
    EXPECT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q_Gated", gated))
        << "the Level>=2 quest must be acceptable now — the issue's acceptance criterion";
}

// ----------------------------------------------------------------------------
// (c) Attribute-point spend / respec compose with class growth.
// ----------------------------------------------------------------------------
TEST_F(ProgressionFullLoopTest, SpendAndRespecAttributePointsComposeWithGrowth)
{
    LevelUpToTwo();
    ASSERT_EQ(Progress().AttributePoints, 5);

    const auto& ac = Abilities();
    ASSERT_NEAR(ac.Attributes.GetCurrentValue("AttackPower"), 12.0f, 1e-3f)
        << "pre-spend: 10 base + 2 growth at level 2";

    EXPECT_TRUE(ProgressionSystem::SpendAttributePoint(&GetScene(), m_Player, "AttackPower", 2));
    EXPECT_EQ(Progress().AttributePoints, 3);
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("AttackPower"), 14.0f, 1e-3f)
        << "10 base + 2 growth + 2 points * 1.0 ValuePerPoint";

    // The class marks Health non-spendable (ValuePerPoint 0).
    EXPECT_FALSE(ProgressionSystem::SpendAttributePoint(&GetScene(), m_Player, "Health", 1))
        << "an attribute with ValuePerPoint <= 0 must reject spends when a class is resolved";

    const i32 refunded = ProgressionSystem::RespecAttributes(&GetScene(), m_Player);
    EXPECT_EQ(refunded, 2) << "respec must report both spent points";
    EXPECT_EQ(Progress().AttributePoints, 5) << "respec must restore the pool";
    EXPECT_TRUE(Progress().AllocatedPoints.empty());
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("AttackPower"), 12.0f, 1e-3f)
        << "respec must restore base + growth, leaving the growth modifier intact";
}

// ----------------------------------------------------------------------------
// (d) Skill tree: unlock, payload application, activation, refund gates.
// ----------------------------------------------------------------------------
TEST_F(ProgressionFullLoopTest, SkillNodesUnlockActivateRefundAndRespec)
{
    LevelUpToTwo();
    ASSERT_EQ(Progress().SkillPoints, 2);

    std::vector<SkillNodeUnlockedEvent> unlockedEvents;
    GetScene().GetGameplayEvents().Subscribe<SkillNodeUnlockedEvent>(
        [&](const SkillNodeUnlockedEvent& e)
        { unlockedEvents.push_back(e); });

    EXPECT_FALSE(ProgressionSystem::CanUnlockSkillNode(&GetScene(), m_Player, kPowerStrike))
        << "power_strike must be locked while its toughness prerequisite is not unlocked";

    ASSERT_TRUE(ProgressionSystem::UnlockSkillNode(&GetScene(), m_Player, kToughness));
    EXPECT_EQ(Progress().SkillPoints, 1) << "toughness costs 1 SP";
    EXPECT_TRUE(Progress().UnlockedNodes.contains(kToughness));

    // Passive modifiers land when ActiveEffects.Tick next runs (one tick).
    const auto& ac = Abilities();
    RunFrames(1);
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("AttackPower"), 17.0f, 1e-3f)
        << "10 base + 2 growth + 5 toughness passive after one effect tick";

    EXPECT_TRUE(ProgressionSystem::CanUnlockSkillNode(&GetScene(), m_Player, kPowerStrike))
        << "level 2 + 1 SP + toughness unlocked satisfies every power_strike gate";
    ASSERT_TRUE(ProgressionSystem::UnlockSkillNode(&GetScene(), m_Player, kPowerStrike));
    EXPECT_EQ(Progress().SkillPoints, 0);
    EXPECT_TRUE(PlayerHasAbility(kAbilityTag)) << "the ability payload must land in AbilityComponent";

    EXPECT_TRUE(GameplayAbilitySystem::TryActivateAbility(&GetScene(), m_Player, GameplayTag(kAbilityTag)))
        << "the granted zero-cost ability must activate";

    ASSERT_EQ(unlockedEvents.size(), 2u) << "each unlock must publish SkillNodeUnlockedEvent";
    EXPECT_EQ(unlockedEvents[0].NodeID, kToughness);
    EXPECT_EQ(unlockedEvents[1].NodeID, kPowerStrike);

    EXPECT_FALSE(ProgressionSystem::RefundSkillNode(&GetScene(), m_Player, kToughness))
        << "refunding toughness must be refused while unlocked power_strike lists it as prerequisite";
    EXPECT_TRUE(Progress().UnlockedNodes.contains(kToughness)) << "the refused refund must not mutate";

    const i32 refunded = ProgressionSystem::RespecSkills(&GetScene(), m_Player);
    EXPECT_EQ(refunded, 2) << "respec must return toughness (1) + power_strike (1) costs";
    EXPECT_TRUE(Progress().UnlockedNodes.empty());
    EXPECT_EQ(Progress().SkillPoints, 2);
    EXPECT_FALSE(PlayerHasAbility(kAbilityTag)) << "respec must revoke the granted ability";
    EXPECT_NEAR(ac.Attributes.GetCurrentValue("AttackPower"), 12.0f, 1e-3f)
        << "respec must revert the toughness passive immediately (cleanup overload) — back to base + growth";
}

// ----------------------------------------------------------------------------
// (e) XP past the curve's max level is discarded.
// ----------------------------------------------------------------------------
TEST_F(ProgressionFullLoopTest, XPBeyondCurveMaxLevelIsDiscarded)
{
    ProgressionSystem::GrantExperience(&GetScene(), m_Player, 1000000);
    RunFrames(1);

    const auto& comp = Progress();
    EXPECT_EQ(comp.Level, 4) << "the table curve's MaxLevel (4) caps the level";
    EXPECT_EQ(comp.CurrentXP, 0)
        << "the 1'000'000 - (100+200+300) leftover must be DISCARDED at the cap, not banked";
    EXPECT_EQ(ProgressionSystem::GetMaxLevel(&GetScene(), m_Player), 4);
    EXPECT_EQ(ProgressionSystem::GetXPToNextLevel(&GetScene(), m_Player), 0) << "no next level at the cap";
    EXPECT_EQ(comp.AttributePoints, 15) << "3 level-ups x 5 AP";
    EXPECT_EQ(comp.SkillPoints, 6) << "3 level-ups x 2 SP";
}

// ----------------------------------------------------------------------------
// (f) Save-game round trip (binary): full fidelity incl. PendingXP; the
//     first tick after load re-applies modifier sources idempotently.
// ----------------------------------------------------------------------------
TEST_F(ProgressionFullLoopTest, SaveGameRoundTripPreservesProgressionAndReappliesModifiersIdempotently)
{
    LevelUpToTwo();
    ASSERT_TRUE(ProgressionSystem::UnlockSkillNode(&GetScene(), m_Player, kToughness));
    ASSERT_TRUE(ProgressionSystem::SpendAttributePoint(&GetScene(), m_Player, "AttackPower", 2));
    RunFrames(1); // let the passive-effect modifiers land

    ASSERT_NEAR(Abilities().Attributes.GetCurrentValue("AttackPower"), 19.0f, 1e-3f)
        << "pre-capture: 10 base + 2 growth + 2 allocated + 5 toughness";

    ProgressionSystem::GrantExperience(&GetScene(), m_Player, 33); // deliberately left pending

    auto payload = SaveGameSerializer::CaptureSceneState(GetScene());
    ASSERT_GT(payload.size(), 0u) << "CaptureSceneState produced an empty blob";

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*restored, payload));

    Entity hero = restored->FindEntityByName("Hero");
    ASSERT_TRUE(hero) << "player entity missing from the restored scene";
    ASSERT_TRUE(hero.HasComponent<ProgressionComponent>())
        << "ProgressionComponent dropped by the save-game round trip";

    const auto& comp = hero.GetComponent<ProgressionComponent>();
    EXPECT_EQ(comp.Level, 2);
    EXPECT_EQ(comp.CurrentXP, 50);
    EXPECT_EQ(comp.AttributePoints, 3) << "5 granted - 2 spent";
    EXPECT_EQ(comp.SkillPoints, 1) << "2 granted - 1 toughness";
    EXPECT_TRUE(comp.HealOnLevelUp);
    EXPECT_TRUE(comp.UnlockedNodes.contains(kToughness)) << "UnlockedNodes set dropped";
    EXPECT_EQ(comp.UnlockedNodes.size(), 1u);
    ASSERT_TRUE(comp.AllocatedPoints.contains("AttackPower")) << "AllocatedPoints map dropped";
    EXPECT_EQ(comp.AllocatedPoints.at("AttackPower"), 2);
    EXPECT_EQ(comp.ClassID, kClassID);
    EXPECT_EQ(static_cast<u64>(comp.ClassDatabaseHandle), static_cast<u64>(m_ClassDbHandle));
    // The component's own curve handle stays 0 ("defer to the class default"):
    // only InitializeFromClass seeds it. Deferred resolution must survive the
    // round trip — the cap test (e) proves the class-default curve actually
    // resolves through this path.
    EXPECT_EQ(static_cast<u64>(comp.ExperienceCurveHandle), 0u)
        << "an authored 0 must stay 0 — deferred class-default curve resolution";
    EXPECT_EQ(comp.PendingXP, 33) << "save-games persist PendingXP (unlike scene YAML)";
    EXPECT_FALSE(comp.RuntimeInitialized)
        << "RuntimeInitialized must reset on load so the first tick re-applies runtime state";

    ASSERT_TRUE(hero.HasComponent<AbilityComponent>());
    const auto& restoredAC = hero.GetComponent<AbilityComponent>();
    EXPECT_NEAR(restoredAC.Attributes.GetCurrentValue("AttackPower"), 19.0f, 1e-3f)
        << "the AttributeSet snapshot restores every modifier before any tick";

    // Tick the restored scene: EnsureRuntimeState re-applies the growth /
    // allocated-point sources and the toughness payload — idempotently — and
    // the Progression drain resolves the 33 pending XP (50 + 33 = 83 < 200,
    // no further level-up). The memory-only assets registered in BuildScene
    // are process-wide on the same asset manager, so they still resolve.
    const Timestep ts{ 1.0f / 60.0f };
    restored->OnUpdateRuntime(ts);
    restored->OnUpdateRuntime(ts);

    EXPECT_NEAR(restoredAC.Attributes.GetCurrentValue("AttackPower"), 19.0f, 1e-3f)
        << "reapplication after load must be idempotent — no double-applied modifiers";
    EXPECT_EQ(comp.Level, 2) << "83 XP banked at level 2 (needs 200) is not a level-up";
    EXPECT_EQ(comp.CurrentXP, 83) << "50 carried + 33 pending drained";
    EXPECT_EQ(comp.PendingXP, 0);
    EXPECT_TRUE(comp.RuntimeInitialized);
}

// ----------------------------------------------------------------------------
// (g) Scene YAML round trip: authored state survives, PendingXP resets.
// ----------------------------------------------------------------------------
TEST_F(ProgressionFullLoopTest, SceneYAMLRoundTripResetsPendingXPAndReappliesModifiers)
{
    LevelUpToTwo();
    ASSERT_TRUE(ProgressionSystem::UnlockSkillNode(&GetScene(), m_Player, kToughness));
    ASSERT_TRUE(ProgressionSystem::SpendAttributePoint(&GetScene(), m_Player, "AttackPower", 2));
    RunFrames(1);
    ProgressionSystem::GrantExperience(&GetScene(), m_Player, 33); // must NOT survive scene YAML

    SceneSerializer serializer(GetSceneRef());
    const std::string yaml = serializer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty());
    EXPECT_EQ(yaml.find("PendingXP"), std::string::npos)
        << "PendingXP carries OLO_SERIALIZE(Skip) and must never appear in scene YAML";
    EXPECT_EQ(yaml.find("RuntimeInitialized"), std::string::npos)
        << "RuntimeInitialized carries OLO_SERIALIZE(Skip) and must never appear in scene YAML";

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    SceneSerializer restoreSerializer(restored);
    ASSERT_TRUE(restoreSerializer.DeserializeFromYAML(yaml)) << "scene YAML round trip failed to load";

    Entity hero = restored->FindEntityByName("Hero");
    ASSERT_TRUE(hero);
    ASSERT_TRUE(hero.HasComponent<ProgressionComponent>())
        << "ProgressionComponent missing after scene YAML round trip";

    const auto& comp = hero.GetComponent<ProgressionComponent>();
    EXPECT_EQ(comp.Level, 2);
    EXPECT_EQ(comp.CurrentXP, 50);
    EXPECT_EQ(comp.AttributePoints, 3);
    EXPECT_EQ(comp.SkillPoints, 1);
    EXPECT_TRUE(comp.UnlockedNodes.contains(kToughness));
    ASSERT_TRUE(comp.AllocatedPoints.contains("AttackPower"));
    EXPECT_EQ(comp.AllocatedPoints.at("AttackPower"), 2);
    EXPECT_EQ(comp.ClassID, kClassID);
    EXPECT_EQ(static_cast<u64>(comp.ClassDatabaseHandle), static_cast<u64>(m_ClassDbHandle));
    // 0 = "defer to the class default curve" (only InitializeFromClass seeds
    // the component handle) — the authored 0 must survive scene YAML.
    EXPECT_EQ(static_cast<u64>(comp.ExperienceCurveHandle), 0u);
    EXPECT_EQ(comp.PendingXP, 0) << "PendingXP must reset to 0 through scene YAML (runtime-only field)";
    EXPECT_FALSE(comp.RuntimeInitialized) << "RuntimeInitialized must reset through scene YAML";

    // Scene YAML restores AbilityComponent base values only (no modifiers /
    // active effects) — the first ticks must re-derive growth + allocation +
    // the toughness payload from the restored ProgressionComponent. Two ticks:
    // one for EnsureRuntimeState (Progression node), one for the ability
    // system's effect tick to apply the re-applied passive's modifiers.
    ASSERT_TRUE(hero.HasComponent<AbilityComponent>());
    const auto& restoredAC = hero.GetComponent<AbilityComponent>();
    EXPECT_NEAR(restoredAC.Attributes.GetCurrentValue("AttackPower"), 10.0f, 1e-3f)
        << "pre-tick: scene YAML carries only the base value";

    const Timestep ts{ 1.0f / 60.0f };
    restored->OnUpdateRuntime(ts);
    restored->OnUpdateRuntime(ts);

    EXPECT_NEAR(restoredAC.Attributes.GetCurrentValue("AttackPower"), 19.0f, 1e-3f)
        << "post-tick: 10 base + 2 growth + 2 allocated + 5 toughness must be re-derived after load";
    EXPECT_TRUE(comp.RuntimeInitialized);
}
