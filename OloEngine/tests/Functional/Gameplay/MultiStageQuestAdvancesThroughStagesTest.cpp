#include "OloEnginePCH.h"

// =============================================================================
// MultiStageQuestAdvancesThroughStagesTest — Functional Test.
//
// Cross-subsystem seam under test:
//   QuestJournal::AcceptQuest × IncrementObjective × TryAdvanceStage ×
//   per-stage objective adoption (ObjectiveStates rebuilt from
//   Definition.Stages[CurrentStageIndex].Objectives). The earlier
//   QuestObjectiveCompletionAdvancesStageTest covered single-stage
//   auto-complete. This one pins the BETWEEN-stages path:
//     - Stage 0 completes → CurrentStageIndex advances to 1.
//     - ObjectiveStates is updated to the stage-1 objectives (NOT the
//       stage-0 ones with their progress preserved).
//     - Stage 1's objectives start at CurrentCount=0, IsCompleted=false.
//   A regression that forgets to refresh ObjectiveStates leaves the
//   journal querying stale stage-0 objective IDs on stage 1, which then
//   silently never complete because IncrementObjective doesn't find them.
//
// Scenario: 2-stage quest:
//   - Stage 0: kill 1 wolf (id "kill_wolf")
//   - Stage 1: reach the village (id "reach_village")
// Increment kill_wolf → stage 0 completes → stage 1 active. Confirm:
//   - quest still Active (not auto-completed; stage 1 is required first)
//   - CurrentStageIndex == 1
//   - The active objective is now "reach_village" with CurrentCount=0.
// Then notify reach_village → quest auto-completes.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class MultiStageQuestAdvancesThroughStagesTest : public FunctionalTest
{
  protected:
    static constexpr const char* kQuestID = "Q_TwoStage";
    static constexpr const char* kObj0   = "kill_wolf";
    static constexpr const char* kObj1   = "reach_village";

    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Player");
        auto& jc = m_Player.AddComponent<QuestJournalComponent>();

        QuestDefinition def;
        def.QuestID = kQuestID;
        def.Title = "Two-stage quest";

        QuestStage s0;
        s0.StageID = "stage0";
        s0.RequireAllObjectives = true;
        QuestObjective killObj;
        killObj.ObjectiveID = kObj0;
        killObj.ObjectiveType = QuestObjective::Type::Kill;
        killObj.TargetID = "wolf";
        killObj.RequiredCount = 1;
        s0.Objectives.push_back(std::move(killObj));
        def.Stages.push_back(std::move(s0));

        QuestStage s1;
        s1.StageID = "stage1";
        s1.RequireAllObjectives = true;
        QuestObjective reachObj;
        reachObj.ObjectiveID = kObj1;
        reachObj.ObjectiveType = QuestObjective::Type::Reach;
        reachObj.TargetID = "village";
        reachObj.RequiredCount = 1;
        s1.Objectives.push_back(std::move(reachObj));
        def.Stages.push_back(std::move(s1));

        ASSERT_TRUE(jc.Journal.AcceptQuest(kQuestID, def));
    }

    Entity m_Player;
};

TEST_F(MultiStageQuestAdvancesThroughStagesTest, FinishingStageZeroSwitchesToStageOneObjectives)
{
    auto& jc = m_Player.GetComponent<QuestJournalComponent>();

    // Stage-0 setup checks.
    EXPECT_EQ(jc.Journal.GetCurrentStageIndex(kQuestID), 0);
    {
        const auto* obj = jc.Journal.GetObjective(kQuestID, kObj0);
        ASSERT_NE(obj, nullptr) << "stage-0 objective not present at quest start.";
    }
    {
        const auto* obj1 = jc.Journal.GetObjective(kQuestID, kObj1);
        EXPECT_EQ(obj1, nullptr)
            << "stage-1 objective is already visible on stage 0 — "
               "ObjectiveStates was populated with ALL stages' objectives, "
               "not just the current stage's.";
    }

    // Complete stage 0's objective.
    jc.Journal.IncrementObjective(kQuestID, kObj0, 1);
    EXPECT_EQ(jc.Journal.GetCurrentStageIndex(kQuestID), 1)
        << "CurrentStageIndex didn't advance after stage 0's only objective hit required count.";
    EXPECT_EQ(jc.Journal.GetQuestStatus(kQuestID), QuestStatus::Active)
        << "quest auto-completed despite stage 1 not being attempted yet.";

    // Stage-1 objective should now be active; stage-0 objective should be gone.
    {
        const auto* obj1 = jc.Journal.GetObjective(kQuestID, kObj1);
        ASSERT_NE(obj1, nullptr)
            << "stage-1 objective not present after stage advance — "
               "TryAdvanceStage didn't repopulate ObjectiveStates from the new stage's definitions.";
        EXPECT_EQ(obj1->CurrentCount, 0)
            << "stage-1 objective starts with non-zero progress (state leaked from stage 0?).";
        EXPECT_FALSE(obj1->IsCompleted);
    }

    // Complete stage 1 → quest auto-completes.
    jc.Journal.IncrementObjective(kQuestID, kObj1, 1);
    EXPECT_FALSE(jc.Journal.IsQuestActive(kQuestID))
        << "stage 1 done but quest still listed as active.";
    EXPECT_TRUE(jc.Journal.HasCompletedQuest(kQuestID))
        << "stage 1 done but quest not in CompletedQuestIDs.";
}
