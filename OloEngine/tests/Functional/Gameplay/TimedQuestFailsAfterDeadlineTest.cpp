#include "OloEnginePCH.h"

// =============================================================================
// TimedQuestFailsAfterDeadlineTest — Functional Test.
//
// Cross-subsystem seam under test:
//   Scene tick × QuestSystem::OnUpdate × QuestJournal::UpdateTimers.
//   Scene::OnUpdateRuntime calls QuestSystem::OnUpdate which forwards the
//   per-tick `dt` to QuestJournal::UpdateTimers, which increments per-quest
//   ElapsedTime and transitions Active → Failed when the QuestDefinition
//   marks the quest CanFail with a positive TimeLimit. A regression that
//   stops invoking QuestSystem from OnUpdateRuntime, mis-orders the call so
//   `dt` is zero, or trips over a copied-quest-state path silently lets
//   timed quests live forever.
//
// Scenario: accept one quest with TimeLimit=0.5s, tick the scene for ~1s.
// After the deadline passes, the journal must report the quest as Failed
// (and not Active or Completed).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class TimedQuestFailsAfterDeadlineTest : public FunctionalTest
{
  protected:
    static constexpr const char* kQuestID = "QT_Timed";

    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Player");
        m_Player.AddComponent<QuestJournalComponent>();

        // Build a single-stage quest with a 0.5s deadline. CanFail must be true,
        // otherwise UpdateTimers short-circuits before reading TimeLimit.
        QuestDefinition def;
        def.QuestID = kQuestID;
        def.Title = "Be quick";
        def.CanFail = true;
        def.TimeLimit = 0.5f;
        QuestStage stage;
        stage.StageID = "S0";
        QuestObjective obj;
        obj.ObjectiveID = "wait";
        obj.ObjectiveType = QuestObjective::Type::Interact;
        obj.TargetID = "anything";
        obj.RequiredCount = 1;
        stage.Objectives.push_back(std::move(obj));
        def.Stages.push_back(std::move(stage));

        auto& jc = m_Player.GetComponent<QuestJournalComponent>();
        const bool accepted = jc.Journal.AcceptQuest(kQuestID, def);
        ASSERT_TRUE(accepted) << "AcceptQuest rejected a fresh quest — preconditions are wrong in the test setup.";
        ASSERT_EQ(jc.Journal.GetQuestStatus(kQuestID), QuestStatus::Active);
    }

    Entity m_Player;
};

TEST_F(TimedQuestFailsAfterDeadlineTest, ActiveQuestTransitionsToFailedAfterTimeLimitElapses)
{
    const auto& jc = m_Player.GetComponent<QuestJournalComponent>();

    // Halfway through the deadline: still Active.
    TickFor(/*seconds=*/0.25f);
    EXPECT_EQ(jc.Journal.GetQuestStatus(kQuestID), QuestStatus::Active)
        << "quest moved to a non-Active state before its deadline — "
           "UpdateTimers is double-counting or failing on the wrong condition.";

    // Past the deadline.
    TickFor(/*seconds=*/0.6f);
    EXPECT_EQ(jc.Journal.GetQuestStatus(kQuestID), QuestStatus::Failed)
        << "TimeLimit deadline elapsed but the quest did not transition to "
           "Failed — Scene::OnUpdateRuntime is either skipping QuestSystem or "
           "the per-tick dt being forwarded is zero.";
}
