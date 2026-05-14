#include "OloEnginePCH.h"

// =============================================================================
// QuestObjectiveCompletionAdvancesStageTest — Functional Test.
//
// Cross-subsystem seam under test:
//   QuestJournalComponent × QuestJournal::IncrementObjective ×
//   QuestJournal::TryAdvanceStage × Scene tick. The "objective increment"
//   path:
//     1. IncrementObjective(quest, objective, amount) bumps CurrentCount.
//     2. When CurrentCount hits RequiredCount, IsCompleted flips.
//     3. TryAdvanceStage walks the active stage's objectives, sees all
//        required are complete, and increments CurrentStageIndex.
//     4. After the final stage advances past Stages.size(), CompleteQuest
//        is auto-invoked (because CompletionChoices is empty), which moves
//        the quest from m_ActiveQuests into m_CompletedQuestIDs.
//   This whole chain runs *outside* the per-frame QuestSystem::OnUpdate
//   (which only tracks timed-quest deadlines), so the test asserts on it
//   as a synchronous side-effect of the increment call — no ticking
//   required to see stage advancement, but a Scene tick before the
//   assertions confirms no other system mutates the journal mid-frame.
//
// Scenario: a single-stage quest with one Kill objective requiring 3 kills.
// Increment three times. After the third call:
//   - Objective.IsCompleted is true
//   - The quest no longer appears in GetActiveQuests
//   - The quest appears in GetCompletedQuests (auto-complete on stage clear)
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"

#include <algorithm>

using namespace OloEngine;
using namespace OloEngine::Functional;

class QuestObjectiveCompletionAdvancesStageTest : public FunctionalTest
{
  protected:
    static constexpr const char* kQuestID = "Q_KillThree";
    static constexpr const char* kObjectiveID = "kill_wolves";

    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Player");
        m_Player.AddComponent<QuestJournalComponent>();

        QuestDefinition def;
        def.QuestID = kQuestID;
        def.Title = "Kill three wolves";
        // CompletionChoices intentionally empty → auto-complete on final stage advance.

        QuestStage stage;
        stage.StageID = "stage0";
        stage.RequireAllObjectives = true;
        QuestObjective obj;
        obj.ObjectiveID = kObjectiveID;
        obj.ObjectiveType = QuestObjective::Type::Kill;
        obj.TargetID = "wolf";
        obj.RequiredCount = 3;
        stage.Objectives.push_back(std::move(obj));
        def.Stages.push_back(std::move(stage));

        auto& jc = m_Player.GetComponent<QuestJournalComponent>();
        ASSERT_TRUE(jc.Journal.AcceptQuest(kQuestID, def));
        ASSERT_EQ(jc.Journal.GetQuestStatus(kQuestID), QuestStatus::Active);
    }

    Entity m_Player;
};

TEST_F(QuestObjectiveCompletionAdvancesStageTest, IncrementToTargetMarksCompleteAndAutoFinalizesQuest)
{
    auto& jc = m_Player.GetComponent<QuestJournalComponent>();

    // Two increments — objective not yet complete.
    jc.Journal.IncrementObjective(kQuestID, kObjectiveID, 1);
    jc.Journal.IncrementObjective(kQuestID, kObjectiveID, 1);
    {
        const auto* obj = jc.Journal.GetObjective(kQuestID, kObjectiveID);
        ASSERT_NE(obj, nullptr);
        EXPECT_EQ(obj->CurrentCount, 2);
        EXPECT_FALSE(obj->IsCompleted);
    }
    EXPECT_EQ(jc.Journal.GetQuestStatus(kQuestID), QuestStatus::Active);

    // Third increment crosses the threshold. IncrementObjective itself calls
    // TryAdvanceStage, which in turn calls CompleteQuest because the quest
    // has no further stages and no branch choices. So the quest moves to
    // Completed in this one synchronous call.
    jc.Journal.IncrementObjective(kQuestID, kObjectiveID, 1);

    // A Scene tick afterwards should be a no-op for journal contents (the
    // per-frame QuestSystem::OnUpdate only ticks timers, doesn't touch
    // completed entries). If something in OnUpdate mutates the wrong state
    // post-completion, the asserts below catch it.
    RunFrames(1);

    EXPECT_NE(jc.Journal.GetQuestStatus(kQuestID), QuestStatus::Active)
        << "quest still Active after final-stage objective hit its required count;"
           " TryAdvanceStage didn't fire or CompleteQuest didn't transition.";
    EXPECT_TRUE(jc.Journal.HasCompletedQuest(kQuestID))
        << "quest didn't land in m_CompletedQuestIDs after auto-completion.";

    // Active list should no longer contain the quest.
    const auto active = jc.Journal.GetActiveQuests();
    EXPECT_TRUE(std::find(active.begin(), active.end(), kQuestID) == active.end())
        << "completed quest still listed as active — CompleteQuest didn't erase from m_ActiveQuests.";
}
