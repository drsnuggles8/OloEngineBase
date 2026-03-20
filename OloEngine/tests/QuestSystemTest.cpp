#include <gtest/gtest.h>
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"
#include "OloEngine/Gameplay/Quest/QuestJournal.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"

using namespace OloEngine;

// Helper to build a simple quest definition
static QuestDefinition MakeSimpleQuest(const std::string& id, const std::string& title = "Test Quest")
{
    QuestDefinition def;
    def.QuestID = id;
    def.Title = title;
    def.Description = "A test quest";
    def.Category = "Side";

    QuestStage stage;
    stage.StageID = "stage_1";
    stage.Description = "First stage";
    stage.RequireAllObjectives = true;

    QuestObjective obj;
    obj.ObjectiveID = "kill_wolves";
    obj.Description = "Kill 5 Wolves";
    obj.ObjectiveType = QuestObjective::Type::Kill;
    obj.TargetID = "wolf";
    obj.RequiredCount = 5;
    stage.Objectives.push_back(obj);

    def.Stages.push_back(stage);
    return def;
}

static QuestDefinition MakeMultiStageQuest()
{
    QuestDefinition def;
    def.QuestID = "multi_stage";
    def.Title = "Multi-Stage Quest";
    def.Category = "Main";

    // Stage 1: Kill wolves
    QuestStage stage1;
    stage1.StageID = "stage_1";
    stage1.RequireAllObjectives = true;
    QuestObjective obj1;
    obj1.ObjectiveID = "kill_wolves";
    obj1.ObjectiveType = QuestObjective::Type::Kill;
    obj1.TargetID = "wolf";
    obj1.RequiredCount = 3;
    stage1.Objectives.push_back(obj1);
    def.Stages.push_back(stage1);

    // Stage 2: Collect herbs
    QuestStage stage2;
    stage2.StageID = "stage_2";
    stage2.RequireAllObjectives = true;
    QuestObjective obj2;
    obj2.ObjectiveID = "collect_herbs";
    obj2.ObjectiveType = QuestObjective::Type::Collect;
    obj2.TargetID = "herb";
    obj2.RequiredCount = 5;
    stage2.Objectives.push_back(obj2);
    def.Stages.push_back(stage2);

    return def;
}

class QuestSystemTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        QuestDatabase::Clear();
    }

    void TearDown() override
    {
        QuestDatabase::Clear();
    }
};

// Test quest lifecycle: accept → complete
TEST_F(QuestSystemTest, QuestLifecycle_AcceptToComplete)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("quest_01");

    ASSERT_TRUE(journal.AcceptQuest("quest_01", def));
    EXPECT_TRUE(journal.IsQuestActive("quest_01"));
    EXPECT_EQ(journal.GetQuestStatus("quest_01"), QuestStatus::Active);

    // Kill 5 wolves
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }

    // Quest should be auto-completed (no choices)
    EXPECT_TRUE(journal.HasCompletedQuest("quest_01"));
    EXPECT_FALSE(journal.IsQuestActive("quest_01"));
    EXPECT_EQ(journal.GetQuestStatus("quest_01"), QuestStatus::Completed);
}

// Test that unavailable shows when quest not accepted
TEST_F(QuestSystemTest, QuestStatus_Unavailable)
{
    QuestJournal journal;
    EXPECT_EQ(journal.GetQuestStatus("nonexistent"), QuestStatus::Unavailable);
}

// Test duplicate accept
TEST_F(QuestSystemTest, AcceptQuest_AlreadyActive)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("quest_01");

    ASSERT_TRUE(journal.AcceptQuest("quest_01", def));
    EXPECT_FALSE(journal.AcceptQuest("quest_01", def));
}

// Test abandon quest
TEST_F(QuestSystemTest, AbandonQuest)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("quest_01");

    journal.AcceptQuest("quest_01", def);
    ASSERT_TRUE(journal.AbandonQuest("quest_01"));
    EXPECT_FALSE(journal.IsQuestActive("quest_01"));
    EXPECT_EQ(journal.GetQuestStatus("quest_01"), QuestStatus::Unavailable);
}

// Test objective increment tracking
TEST_F(QuestSystemTest, ObjectiveTracking_Increment)
{
    QuestJournal journal;
    // Use multi-stage quest so completing stage 1 doesn't auto-complete the quest
    auto def = MakeMultiStageQuest();
    journal.AcceptQuest("multi_stage", def);

    journal.IncrementObjective("multi_stage", "kill_wolves", 1);
    auto const* obj = journal.GetObjective("multi_stage", "kill_wolves");
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->CurrentCount, 1);
    EXPECT_FALSE(obj->IsCompleted);

    journal.IncrementObjective("multi_stage", "kill_wolves", 2);
    obj = journal.GetObjective("multi_stage", "kill_wolves");
    // Stage advances after 3 kills; now on stage 2 objectives
    // Verify stage advanced
    EXPECT_EQ(journal.GetCurrentStageIndex("multi_stage"), 1);
}

// Test overshoot (more kills than required)
TEST_F(QuestSystemTest, ObjectiveTracking_Overshoot)
{
    QuestJournal journal;
    // Use multi-stage quest so completing stage 1 doesn't auto-complete
    auto def = MakeMultiStageQuest();
    journal.AcceptQuest("multi_stage", def);

    journal.IncrementObjective("multi_stage", "kill_wolves", 100);
    // Stage 1 completes, advances to stage 2
    EXPECT_EQ(journal.GetCurrentStageIndex("multi_stage"), 1);
    EXPECT_TRUE(journal.IsQuestActive("multi_stage"));
}

// Test multi-stage advancement
TEST_F(QuestSystemTest, MultiStage_Advancement)
{
    QuestJournal journal;
    auto def = MakeMultiStageQuest();
    journal.AcceptQuest("multi_stage", def);

    EXPECT_EQ(journal.GetCurrentStageIndex("multi_stage"), 0);

    // Complete stage 1
    for (int i = 0; i < 3; ++i)
    {
        journal.NotifyKill("wolf");
    }

    EXPECT_EQ(journal.GetCurrentStageIndex("multi_stage"), 1);

    // Complete stage 2
    journal.NotifyCollect("herb", 5);

    EXPECT_TRUE(journal.HasCompletedQuest("multi_stage"));
}

// Test notify-based progress (Kill)
TEST_F(QuestSystemTest, NotifyKill_UpdatesCorrectObjective)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("quest_01");
    journal.AcceptQuest("quest_01", def);

    journal.NotifyKill("wolf");
    auto const* obj = journal.GetObjective("quest_01", "kill_wolves");
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->CurrentCount, 1);

    // Notify for unrelated target should not affect anything
    journal.NotifyKill("bear");
    obj = journal.GetObjective("quest_01", "kill_wolves");
    EXPECT_EQ(obj->CurrentCount, 1);
}

// Test notify-based progress (Collect)
TEST_F(QuestSystemTest, NotifyCollect_UpdatesCorrectObjective)
{
    QuestJournal journal;

    QuestDefinition def;
    def.QuestID = "collect_quest";
    QuestStage stage;
    stage.StageID = "stage_1";
    QuestObjective obj;
    obj.ObjectiveID = "collect_gems";
    obj.ObjectiveType = QuestObjective::Type::Collect;
    obj.TargetID = "ruby";
    obj.RequiredCount = 10;
    stage.Objectives.push_back(obj);
    def.Stages.push_back(stage);

    journal.AcceptQuest("collect_quest", def);
    journal.NotifyCollect("ruby", 3);

    auto const* objPtr = journal.GetObjective("collect_quest", "collect_gems");
    ASSERT_NE(objPtr, nullptr);
    EXPECT_EQ(objPtr->CurrentCount, 3);
}

// Test time-limited quest failure
TEST_F(QuestSystemTest, TimedQuest_ExpiresOnTimeout)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("timed_quest");
    def.TimeLimit = 10.0f;
    def.CanFail = true;

    journal.AcceptQuest("timed_quest", def);
    EXPECT_TRUE(journal.IsQuestActive("timed_quest"));

    // 5 seconds pass
    journal.UpdateTimers(5.0f);
    EXPECT_TRUE(journal.IsQuestActive("timed_quest"));

    // 6 more seconds pass (total 11 > 10)
    journal.UpdateTimers(6.0f);
    EXPECT_FALSE(journal.IsQuestActive("timed_quest"));
}

// Test failure via tags
TEST_F(QuestSystemTest, FailOnTag)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("fail_tag_quest");
    def.CanFail = true;
    def.FailOnTags.push_back("villain_path");

    journal.AcceptQuest("fail_tag_quest", def);
    EXPECT_TRUE(journal.IsQuestActive("fail_tag_quest"));

    journal.AddTag("villain_path");
    journal.UpdateTimers(0.0f); // Trigger check

    EXPECT_FALSE(journal.IsQuestActive("fail_tag_quest"));
}

// Test branching completion choices
TEST_F(QuestSystemTest, BranchingCompletion)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("branch_quest");

    QuestBranchChoice choiceA;
    choiceA.ChoiceID = "save_village";
    choiceA.GrantedTags.push_back("hero_path");
    def.CompletionChoices.push_back(choiceA);

    QuestBranchChoice choiceB;
    choiceB.ChoiceID = "burn_village";
    choiceB.GrantedTags.push_back("villain_path");
    def.CompletionChoices.push_back(choiceB);

    journal.AcceptQuest("branch_quest", def);

    // Complete all objectives
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }

    // Quest should NOT auto-complete because there are choices
    EXPECT_TRUE(journal.IsQuestActive("branch_quest"));

    // Manually complete with a chosen branch
    journal.CompleteQuest("branch_quest", "save_village");
    EXPECT_TRUE(journal.HasCompletedQuest("branch_quest"));
    EXPECT_TRUE(journal.HasTag("hero_path"));
    EXPECT_FALSE(journal.HasTag("villain_path"));
}

// Test reward tags
TEST_F(QuestSystemTest, CompletionRewards_GrantsTags)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("reward_quest");
    def.CompletionRewards.GrantedTags.push_back("quest_01_complete");

    journal.AcceptQuest("reward_quest", def);
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }

    EXPECT_TRUE(journal.HasTag("quest_01_complete"));
}

// Test non-repeatable quest can't be re-accepted
TEST_F(QuestSystemTest, NonRepeatable_CantReaccept)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("once_only");

    journal.AcceptQuest("once_only", def);
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }

    EXPECT_TRUE(journal.HasCompletedQuest("once_only"));
    EXPECT_FALSE(journal.AcceptQuest("once_only", def));
}

// Test repeatable quest can be re-accepted
TEST_F(QuestSystemTest, Repeatable_CanReaccept)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("daily_hunt");
    def.IsRepeatable = true;

    journal.AcceptQuest("daily_hunt", def);
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }

    EXPECT_TRUE(journal.HasCompletedQuest("daily_hunt"));
    EXPECT_TRUE(journal.AcceptQuest("daily_hunt", def));
}

// Test optional objectives don't block stage completion
TEST_F(QuestSystemTest, OptionalObjectives_DontBlockStage)
{
    QuestJournal journal;
    QuestDefinition def;
    def.QuestID = "optional_quest";

    QuestStage stage;
    stage.StageID = "s1";
    stage.RequireAllObjectives = true;

    QuestObjective required;
    required.ObjectiveID = "required_obj";
    required.ObjectiveType = QuestObjective::Type::Kill;
    required.TargetID = "goblin";
    required.RequiredCount = 1;

    QuestObjective optional;
    optional.ObjectiveID = "optional_obj";
    optional.ObjectiveType = QuestObjective::Type::Collect;
    optional.TargetID = "gold";
    optional.RequiredCount = 10;
    optional.IsOptional = true;

    stage.Objectives.push_back(required);
    stage.Objectives.push_back(optional);
    def.Stages.push_back(stage);

    journal.AcceptQuest("optional_quest", def);
    journal.NotifyKill("goblin");

    // Should complete even though optional objective isn't done
    EXPECT_TRUE(journal.HasCompletedQuest("optional_quest"));
}

// Test RequireAllObjectives = false (any objective completes stage)
TEST_F(QuestSystemTest, AnyObjective_CompletesStage)
{
    QuestJournal journal;
    QuestDefinition def;
    def.QuestID = "any_quest";

    QuestStage stage;
    stage.StageID = "s1";
    stage.RequireAllObjectives = false;

    QuestObjective obj1;
    obj1.ObjectiveID = "path_a";
    obj1.ObjectiveType = QuestObjective::Type::Interact;
    obj1.TargetID = "door_a";
    obj1.RequiredCount = 1;

    QuestObjective obj2;
    obj2.ObjectiveID = "path_b";
    obj2.ObjectiveType = QuestObjective::Type::Interact;
    obj2.TargetID = "door_b";
    obj2.RequiredCount = 1;

    stage.Objectives.push_back(obj1);
    stage.Objectives.push_back(obj2);
    def.Stages.push_back(stage);

    journal.AcceptQuest("any_quest", def);
    journal.NotifyInteract("door_a");

    EXPECT_TRUE(journal.HasCompletedQuest("any_quest"));
}

// Test QuestDatabase register and get
TEST_F(QuestSystemTest, QuestDatabase_RegisterAndGet)
{
    auto def = MakeSimpleQuest("db_quest", "Database Quest");
    QuestDatabase::Register(def);

    auto const* result = QuestDatabase::Get("db_quest");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->Title, "Database Quest");
    EXPECT_EQ(result->QuestID, "db_quest");

    EXPECT_EQ(QuestDatabase::Get("nonexistent"), nullptr);
}

// Test QuestDatabase getByCategory
TEST_F(QuestSystemTest, QuestDatabase_GetByCategory)
{
    auto quest1 = MakeSimpleQuest("main_01");
    quest1.Category = "Main";
    auto quest2 = MakeSimpleQuest("side_01");
    quest2.Category = "Side";
    auto quest3 = MakeSimpleQuest("main_02");
    quest3.Category = "Main";

    QuestDatabase::Register(quest1);
    QuestDatabase::Register(quest2);
    QuestDatabase::Register(quest3);

    auto mainQuests = QuestDatabase::GetByCategory("Main");
    EXPECT_EQ(mainQuests.size(), 2u);

    auto sideQuests = QuestDatabase::GetByCategory("Side");
    EXPECT_EQ(sideQuests.size(), 1u);
}

// Test GetActiveQuests / GetCompletedQuests
TEST_F(QuestSystemTest, GetActiveAndCompletedQuests)
{
    QuestJournal journal;
    auto def1 = MakeSimpleQuest("quest_a");
    auto def2 = MakeSimpleQuest("quest_b");

    journal.AcceptQuest("quest_a", def1);
    journal.AcceptQuest("quest_b", def2);

    auto active = journal.GetActiveQuests();
    EXPECT_EQ(active.size(), 2u);

    // Complete quest_a
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }

    active = journal.GetActiveQuests();
    // quest_a completed, quest_b also completed (both listen for wolfkill on same target)
    auto completed = journal.GetCompletedQuests();
    EXPECT_EQ(active.size() + completed.size(), 2u);
}

// Test SetObjectiveComplete
TEST_F(QuestSystemTest, SetObjectiveComplete)
{
    QuestJournal journal;
    // Use multi-stage quest so completing stage 1 doesn't auto-complete
    auto def = MakeMultiStageQuest();
    journal.AcceptQuest("multi_stage", def);

    journal.SetObjectiveComplete("multi_stage", "kill_wolves");
    // Stage 1 complete → advances to stage 2
    EXPECT_EQ(journal.GetCurrentStageIndex("multi_stage"), 1);
    EXPECT_TRUE(journal.IsQuestActive("multi_stage"));
}

// Test tag management
TEST_F(QuestSystemTest, TagManagement)
{
    QuestJournal journal;
    EXPECT_FALSE(journal.HasTag("test_tag"));
    journal.AddTag("test_tag");
    EXPECT_TRUE(journal.HasTag("test_tag"));
}

// Test ObjectiveType string conversion
TEST_F(QuestSystemTest, ObjectiveTypeStringConversion)
{
    EXPECT_STREQ(ObjectiveTypeToString(QuestObjective::Type::Kill), "Kill");
    EXPECT_STREQ(ObjectiveTypeToString(QuestObjective::Type::Collect), "Collect");
    EXPECT_STREQ(ObjectiveTypeToString(QuestObjective::Type::Interact), "Interact");
    EXPECT_STREQ(ObjectiveTypeToString(QuestObjective::Type::Reach), "Reach");
    EXPECT_STREQ(ObjectiveTypeToString(QuestObjective::Type::Escort), "Escort");
    EXPECT_STREQ(ObjectiveTypeToString(QuestObjective::Type::Custom), "Custom");

    EXPECT_EQ(ObjectiveTypeFromString("Kill"), QuestObjective::Type::Kill);
    EXPECT_EQ(ObjectiveTypeFromString("Collect"), QuestObjective::Type::Collect);
    EXPECT_EQ(ObjectiveTypeFromString("Unknown"), QuestObjective::Type::Custom);
}

// Test QuestStatus string conversion
TEST_F(QuestSystemTest, QuestStatusStringConversion)
{
    EXPECT_STREQ(QuestStatusToString(QuestStatus::Active), "Active");
    EXPECT_STREQ(QuestStatusToString(QuestStatus::Completed), "Completed");
    EXPECT_STREQ(QuestStatusToString(QuestStatus::Failed), "Failed");

    EXPECT_EQ(QuestStatusFromString("Active"), QuestStatus::Active);
    EXPECT_EQ(QuestStatusFromString("Completed"), QuestStatus::Completed);
    EXPECT_EQ(QuestStatusFromString("BadValue"), QuestStatus::Unavailable);
}
