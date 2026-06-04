#include "OloEnginePCH.h"

// =============================================================================
// QuestEventsEmittedTest — Functional Test.
//
// Cross-subsystem seam under test:
//   QuestSystem (entity-aware service layer) × QuestJournal (pure value type,
//   reporting changes via its QuestEventSink) × GameplayEventBus × Scene tick.
//
// This pins the "finish-wire the quest event payloads" work: the journal is
// entity-agnostic, so QuestSystem is the layer that translates journal
// mutations — including the internal objective -> stage -> auto-complete
// cascade — into the entity-stamped QuestEvents.h payloads and publishes them
// on Scene::GetGameplayEvents(). A subscriber registered on the bus must see
// each payload with the right entity UUID and fields.
//
// The timed-quest case additionally proves the *real per-frame path*:
// QuestSystem::OnUpdate (driven by Scene::OnUpdateRuntime) fires QuestFailed
// when a deadline lapses, with no explicit service call.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Gameplay/Quest/QuestEvents.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"

#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    // A single-stage quest with one Kill objective requiring `count` kills.
    // CompletionChoices empty => auto-completes when the stage clears.
    QuestDefinition MakeKillQuest(const std::string& id, const std::string& objId, const std::string& target, i32 count)
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

class QuestEventsEmittedTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Player");
        m_Player.AddComponent<QuestJournalComponent>();
        m_PlayerUUID = m_Player.GetUUID();
    }

    Entity m_Player;
    UUID m_PlayerUUID;
};

TEST_F(QuestEventsEmittedTest, AcceptPublishesQuestStarted)
{
    std::vector<QuestStartedEvent> started;
    GetScene().GetGameplayEvents().Subscribe<QuestStartedEvent>([&](const QuestStartedEvent& e)
                                                                { started.push_back(e); });

    const bool ok = QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q1", MakeKillQuest("Q1", "obj", "wolf", 3));
    ASSERT_TRUE(ok);

    ASSERT_EQ(started.size(), 1u) << "AcceptQuest did not publish exactly one QuestStarted event.";
    EXPECT_EQ(started[0].QuestID, "Q1");
    EXPECT_EQ(static_cast<u64>(started[0].EntityID), static_cast<u64>(m_PlayerUUID))
        << "QuestStarted carried the wrong entity UUID.";
}

TEST_F(QuestEventsEmittedTest, ObjectiveProgressCompletionAndAutoCompleteCascade)
{
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q1", MakeKillQuest("Q1", "obj", "wolf", 3)));

    std::vector<ObjectiveProgressEvent> progress;
    std::vector<ObjectiveCompletedEvent> objDone;
    std::vector<QuestCompletedEvent> questDone;
    auto& bus = GetScene().GetGameplayEvents();
    bus.Subscribe<ObjectiveProgressEvent>([&](const ObjectiveProgressEvent& e) { progress.push_back(e); });
    bus.Subscribe<ObjectiveCompletedEvent>([&](const ObjectiveCompletedEvent& e) { objDone.push_back(e); });
    bus.Subscribe<QuestCompletedEvent>([&](const QuestCompletedEvent& e) { questDone.push_back(e); });

    // Two increments: progress only, no completion yet.
    QuestSystem::IncrementObjective(&GetScene(), m_Player, "Q1", "obj", 1);
    QuestSystem::IncrementObjective(&GetScene(), m_Player, "Q1", "obj", 1);
    ASSERT_EQ(progress.size(), 2u);
    EXPECT_EQ(progress[0].CurrentCount, 1);
    EXPECT_EQ(progress[0].RequiredCount, 3);
    EXPECT_EQ(progress[1].CurrentCount, 2);
    EXPECT_EQ(progress[0].ObjectiveID, "obj");
    EXPECT_TRUE(objDone.empty());
    EXPECT_TRUE(questDone.empty());

    // Third increment crosses the threshold: progress -> objective complete ->
    // (single stage clears) -> auto QuestCompleted, all in one synchronous call.
    QuestSystem::IncrementObjective(&GetScene(), m_Player, "Q1", "obj", 1);
    ASSERT_EQ(progress.size(), 3u);
    EXPECT_EQ(progress[2].CurrentCount, 3);
    ASSERT_EQ(objDone.size(), 1u) << "objective completing did not publish ObjectiveCompleted.";
    EXPECT_EQ(objDone[0].ObjectiveID, "obj");
    ASSERT_EQ(questDone.size(), 1u) << "final-stage clear did not auto-publish QuestCompleted.";
    EXPECT_EQ(questDone[0].QuestID, "Q1");
    EXPECT_EQ(static_cast<u64>(questDone[0].EntityID), static_cast<u64>(m_PlayerUUID));
}

TEST_F(QuestEventsEmittedTest, MultiStageAdvancePublishesStageAdvanced)
{
    // Two-stage quest: stage0 (1 kill) -> stage1 (1 collect). No branch choice.
    QuestDefinition def;
    def.QuestID = "Q2";
    {
        QuestStage s0;
        s0.StageID = "s0";
        QuestObjective o0;
        o0.ObjectiveID = "k";
        o0.ObjectiveType = QuestObjective::Type::Kill;
        o0.TargetID = "wolf";
        o0.RequiredCount = 1;
        s0.Objectives.push_back(o0);
        def.Stages.push_back(s0);

        QuestStage s1;
        s1.StageID = "s1";
        QuestObjective o1;
        o1.ObjectiveID = "c";
        o1.ObjectiveType = QuestObjective::Type::Collect;
        o1.TargetID = "herb";
        o1.RequiredCount = 1;
        s1.Objectives.push_back(o1);
        def.Stages.push_back(s1);
    }
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q2", def));

    std::vector<QuestStageAdvancedEvent> advanced;
    std::vector<QuestCompletedEvent> completed;
    auto& bus = GetScene().GetGameplayEvents();
    bus.Subscribe<QuestStageAdvancedEvent>([&](const QuestStageAdvancedEvent& e) { advanced.push_back(e); });
    bus.Subscribe<QuestCompletedEvent>([&](const QuestCompletedEvent& e) { completed.push_back(e); });

    // Clear stage 0 -> advances to stage 1 (one StageAdvanced, no completion).
    QuestSystem::IncrementObjective(&GetScene(), m_Player, "Q2", "k", 1);
    ASSERT_EQ(advanced.size(), 1u) << "clearing stage 0 did not publish QuestStageAdvanced.";
    EXPECT_EQ(advanced[0].NewStageIndex, 1);
    EXPECT_TRUE(completed.empty());

    // Clear stage 1 -> final stage, auto-completes (no extra StageAdvanced).
    QuestSystem::IncrementObjective(&GetScene(), m_Player, "Q2", "c", 1);
    EXPECT_EQ(advanced.size(), 1u) << "terminal stage clear should complete the quest, not advance a stage.";
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].QuestID, "Q2");
}

TEST_F(QuestEventsEmittedTest, AbandonPublishesQuestAbandoned)
{
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q1", MakeKillQuest("Q1", "obj", "wolf", 3)));

    std::vector<QuestAbandonedEvent> abandoned;
    GetScene().GetGameplayEvents().Subscribe<QuestAbandonedEvent>([&](const QuestAbandonedEvent& e)
                                                                  { abandoned.push_back(e); });

    ASSERT_TRUE(QuestSystem::AbandonQuest(&GetScene(), m_Player, "Q1"));
    ASSERT_EQ(abandoned.size(), 1u);
    EXPECT_EQ(abandoned[0].QuestID, "Q1");

    // Abandoning a non-active quest publishes nothing.
    EXPECT_FALSE(QuestSystem::AbandonQuest(&GetScene(), m_Player, "Q1"));
    EXPECT_EQ(abandoned.size(), 1u);
}

TEST_F(QuestEventsEmittedTest, ExplicitFailPublishesQuestFailed)
{
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q1", MakeKillQuest("Q1", "obj", "wolf", 3)));

    std::vector<QuestFailedEvent> failed;
    GetScene().GetGameplayEvents().Subscribe<QuestFailedEvent>([&](const QuestFailedEvent& e)
                                                               { failed.push_back(e); });

    ASSERT_TRUE(QuestSystem::FailQuest(&GetScene(), m_Player, "Q1"));
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_EQ(failed[0].QuestID, "Q1");
    EXPECT_EQ(static_cast<u64>(failed[0].EntityID), static_cast<u64>(m_PlayerUUID));
}

TEST_F(QuestEventsEmittedTest, TimedQuestDeadlinePublishesQuestFailedViaTick)
{
    // A failable quest with a short deadline; never progressed -> times out.
    QuestDefinition def = MakeKillQuest("QT", "obj", "wolf", 1);
    def.CanFail = true;
    def.TimeLimit = 0.25f; // seconds
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "QT", def));

    std::vector<QuestFailedEvent> failed;
    GetScene().GetGameplayEvents().Subscribe<QuestFailedEvent>([&](const QuestFailedEvent& e)
                                                               { failed.push_back(e); });

    // Tick past the deadline. QuestSystem::OnUpdate (inside OnUpdateRuntime)
    // calls UpdateTimers, which fails the quest and reports it for publishing.
    TickFor(0.5f);

    ASSERT_EQ(failed.size(), 1u) << "timed quest did not publish QuestFailed via the per-frame tick path.";
    EXPECT_EQ(failed[0].QuestID, "QT");
    EXPECT_EQ(static_cast<u64>(failed[0].EntityID), static_cast<u64>(m_PlayerUUID));
}

TEST_F(QuestEventsEmittedTest, NotifyKillDrivesObjectiveEventsAcrossActiveQuests)
{
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "Q1", MakeKillQuest("Q1", "obj", "wolf", 1)));

    std::vector<ObjectiveCompletedEvent> objDone;
    std::vector<QuestCompletedEvent> questDone;
    auto& bus = GetScene().GetGameplayEvents();
    bus.Subscribe<ObjectiveCompletedEvent>([&](const ObjectiveCompletedEvent& e) { objDone.push_back(e); });
    bus.Subscribe<QuestCompletedEvent>([&](const QuestCompletedEvent& e) { questDone.push_back(e); });

    // A wolf kill matches the objective by (type, target) — completes + auto-finishes.
    QuestSystem::NotifyKill(&GetScene(), m_Player, "wolf");
    ASSERT_EQ(objDone.size(), 1u);
    EXPECT_EQ(objDone[0].ObjectiveID, "obj");
    ASSERT_EQ(questDone.size(), 1u);
    EXPECT_EQ(questDone[0].QuestID, "Q1");
}

TEST_F(QuestEventsEmittedTest, CompleteWithBranchPublishesBranchChoice)
{
    // A quest that needs an explicit branch choice (so it doesn't auto-complete).
    QuestDefinition def = MakeKillQuest("QB", "obj", "wolf", 1);
    QuestBranchChoice choice;
    choice.ChoiceID = "spare";
    def.CompletionChoices.push_back(choice);
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, "QB", def));

    std::vector<QuestCompletedEvent> completed;
    GetScene().GetGameplayEvents().Subscribe<QuestCompletedEvent>([&](const QuestCompletedEvent& e)
                                                                  { completed.push_back(e); });

    // Clear the only objective: stage advances past the end, but the branch
    // choice blocks auto-complete, so no QuestCompleted yet.
    QuestSystem::IncrementObjective(&GetScene(), m_Player, "QB", "obj", 1);
    ASSERT_TRUE(completed.empty()) << "quest with a branch choice should not auto-complete.";

    // Explicit completion with the branch choice publishes QuestCompleted.
    ASSERT_TRUE(QuestSystem::CompleteQuest(&GetScene(), m_Player, "QB", "spare"));
    ASSERT_EQ(completed.size(), 1u);
    EXPECT_EQ(completed[0].QuestID, "QB");
    EXPECT_EQ(completed[0].BranchChoice, "spare");
}
