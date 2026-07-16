#include "OloEnginePCH.h"

// OLO_TEST_LAYER: Functional
// =============================================================================
// QuestRewardGrantsExperienceTest — Functional Test.
//
// Cross-subsystem seam under test:
//   QuestSystem::PublishQuestChanges (the one choke point that sees both the
//   completing entity and the reward payload) × ProgressionComponent
//   (CompletionRewards.ExperiencePoints -> PendingXP) × GameplayEventBus
//   (QuestCompletedEvent now carries i32 ExperiencePoints) × the Progression
//   tick resolving the reward into a level-up. BOTH completion paths flow
//   through the same QuestCompleted change record: the direct
//   QuestSystem::CompleteQuest call and the IncrementObjective / NotifyKill
//   auto-complete cascade — a regression on either silently strips quest XP.
//
// Numbers (engine-default curve, no assets): 150 reward XP at L1 (needs
// 100) -> Level 2 with 50 carried.
//
// Note: a quest can only be DIRECTLY completed once every stage is done
// (QuestJournal::CompleteQuest validates CurrentStageIndex), so the direct
// path uses a branch-choice quest — the branch blocks the auto-complete,
// leaving CompleteQuest as the explicit second step.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Progression/ProgressionComponents.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestEvents.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <string>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    QuestDefinition MakeKillQuest(const std::string& id, const std::string& objId,
                                  const std::string& target, i32 count, i32 rewardXP)
    {
        QuestDefinition def;
        def.QuestID = id;
        def.Title = id;
        def.CompletionRewards.ExperiencePoints = rewardXP;

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

class QuestRewardGrantsExperienceTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Player");
        m_Player.AddComponent<QuestJournalComponent>();
        m_Player.AddComponent<ProgressionComponent>(); // handles 0 => default curve
    }

    Entity m_Player;
};

TEST_F(QuestRewardGrantsExperienceTest, AutoCompleteCascadeGrantsRewardXPAndLevelsUp)
{
    std::vector<QuestCompletedEvent> completed;
    GetScene().GetGameplayEvents().Subscribe<QuestCompletedEvent>(
        [&](const QuestCompletedEvent& e)
        { completed.push_back(e); });

    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q_Wolf",
                                         MakeKillQuest("Q_Wolf", "obj", "wolf", 1, /*rewardXP=*/150)));

    // NotifyKill -> objective complete -> stage clears -> auto QuestCompleted,
    // all synchronously inside this call.
    QuestSystem::NotifyKill(&GetScene(), m_Player, "wolf");

    ASSERT_EQ(completed.size(), 1u) << "the kill must auto-complete the quest";
    EXPECT_EQ(completed[0].QuestID, "Q_Wolf");
    EXPECT_EQ(completed[0].ExperiencePoints, 150)
        << "QuestCompletedEvent must carry the CompletionRewards XP";

    const auto& comp = m_Player.GetComponent<ProgressionComponent>();
    EXPECT_EQ(comp.PendingXP, 150)
        << "the cascade path must credit the reward XP into PendingXP";

    RunFrames(1);
    EXPECT_EQ(comp.Level, 2) << "150 XP at L1 (default curve needs 100) must reach level 2";
    EXPECT_EQ(comp.CurrentXP, 50) << "150 - 100 = 50 XP must carry";
    EXPECT_EQ(comp.PendingXP, 0);
}

TEST_F(QuestRewardGrantsExperienceTest, DirectCompleteQuestGrantsRewardXP)
{
    std::vector<QuestCompletedEvent> completed;
    GetScene().GetGameplayEvents().Subscribe<QuestCompletedEvent>(
        [&](const QuestCompletedEvent& e)
        { completed.push_back(e); });

    // The branch choice blocks the auto-complete, so the reward can only flow
    // through the explicit QuestSystem::CompleteQuest call.
    QuestDefinition def = MakeKillQuest("Q_Branch", "obj", "wolf", 1, /*rewardXP=*/150);
    QuestBranchChoice choice;
    choice.ChoiceID = "spare";
    def.CompletionChoices.push_back(std::move(choice));
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q_Branch", def));

    QuestSystem::IncrementObjective(&GetScene(), m_Player, "Q_Branch", "obj", 1);
    ASSERT_TRUE(completed.empty()) << "the branch choice must block auto-completion";
    const auto& comp = m_Player.GetComponent<ProgressionComponent>();
    EXPECT_EQ(comp.PendingXP, 0) << "no XP may be granted before the quest actually completes";

    ASSERT_TRUE(QuestSystem::CompleteQuest(&GetScene(), m_Player, "Q_Branch", "spare"))
        << "explicit completion with a valid branch choice must succeed";

    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].QuestID, "Q_Branch");
    EXPECT_EQ(completed[0].BranchChoice, "spare");
    EXPECT_EQ(completed[0].ExperiencePoints, 150)
        << "the direct CompleteQuest path must carry the reward XP on the event";
    EXPECT_EQ(comp.PendingXP, 150) << "the direct path must credit PendingXP just like the cascade";

    RunFrames(1);
    EXPECT_EQ(comp.Level, 2);
    EXPECT_EQ(comp.CurrentXP, 50);
}

TEST_F(QuestRewardGrantsExperienceTest, ZeroXPQuestGrantsNothing)
{
    std::vector<QuestCompletedEvent> completed;
    GetScene().GetGameplayEvents().Subscribe<QuestCompletedEvent>(
        [&](const QuestCompletedEvent& e)
        { completed.push_back(e); });

    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q_Free",
                                         MakeKillQuest("Q_Free", "obj", "rat", 1, /*rewardXP=*/0)));
    QuestSystem::NotifyKill(&GetScene(), m_Player, "rat");

    ASSERT_EQ(completed.size(), 1u) << "the zero-XP quest must still complete";
    EXPECT_EQ(completed[0].ExperiencePoints, 0);

    const auto& comp = m_Player.GetComponent<ProgressionComponent>();
    EXPECT_EQ(comp.PendingXP, 0) << "a zero-XP reward must not touch PendingXP";

    RunFrames(1);
    EXPECT_EQ(comp.Level, 1) << "nothing to resolve";
    EXPECT_EQ(comp.CurrentXP, 0);
}

TEST_F(QuestRewardGrantsExperienceTest, EntityWithoutProgressionComponentCompletesWithoutCrash)
{
    // A quest-capable entity that never opted into progression: the reward
    // grant must degrade to a no-op (ProgressionSystem::GrantExperience
    // guards on the component) while the completion event still fires.
    Entity npc = GetScene().CreateEntity("QuestNPC");
    npc.AddComponent<QuestJournalComponent>();

    std::vector<QuestCompletedEvent> completed;
    GetScene().GetGameplayEvents().Subscribe<QuestCompletedEvent>(
        [&](const QuestCompletedEvent& e)
        { completed.push_back(e); });

    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), npc, "Q_NPC",
                                         MakeKillQuest("Q_NPC", "obj", "wolf", 1, /*rewardXP=*/150)));
    QuestSystem::NotifyKill(&GetScene(), npc, "wolf"); // must not crash

    ASSERT_EQ(completed.size(), 1u) << "completion must still publish for a progression-less entity";
    EXPECT_EQ(completed[0].ExperiencePoints, 150)
        << "the event still reports the authored reward even when nobody can bank it";
    EXPECT_FALSE(npc.HasComponent<ProgressionComponent>())
        << "the grant must not implicitly add a ProgressionComponent";

    RunFrames(1); // and ticking afterwards must not crash either
}
