#include <gtest/gtest.h>
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"
#include "OloEngine/Gameplay/Quest/QuestRequirement.h"
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
    EXPECT_EQ(journal.GetQuestStatus("timed_quest"), QuestStatus::Failed);
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
    EXPECT_EQ(journal.GetQuestStatus("fail_tag_quest"), QuestStatus::Failed);
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

    // Empty branch should be rejected when choices exist
    EXPECT_FALSE(journal.CompleteQuest("branch_quest", "").has_value());
    EXPECT_TRUE(journal.IsQuestActive("branch_quest"));
    EXPECT_FALSE(journal.HasCompletedQuest("branch_quest"));

    // Invalid branch should be rejected
    EXPECT_FALSE(journal.CompleteQuest("branch_quest", "invalid_branch").has_value());
    EXPECT_TRUE(journal.IsQuestActive("branch_quest"));
    EXPECT_FALSE(journal.HasCompletedQuest("branch_quest"));

    // Manually complete with a valid chosen branch
    auto rewards = journal.CompleteQuest("branch_quest", "save_village");
    EXPECT_TRUE(rewards.has_value());
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

// Test prerequisite quest requirements (via Requirements)
TEST_F(QuestSystemTest, AcceptQuest_RequiresPrerequisiteQuests)
{
    QuestJournal journal;
    auto prereq = MakeSimpleQuest("prereq_quest");
    auto def = MakeSimpleQuest("main_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::QuestCompleted;
    req.Target = "prereq_quest";
    def.Requirements.push_back(req);

    // Should fail without prerequisite
    EXPECT_FALSE(journal.AcceptQuest("main_quest", def));

    // Complete the prerequisite
    journal.AcceptQuest("prereq_quest", prereq);
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }
    EXPECT_TRUE(journal.HasCompletedQuest("prereq_quest"));

    // Now should succeed
    EXPECT_TRUE(journal.AcceptQuest("main_quest", def));
}

// Test required tags for quest acceptance (via Requirements)
TEST_F(QuestSystemTest, AcceptQuest_RequiresTags)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("tagged_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::HasTag;
    req.Target = "hero_path";
    def.Requirements.push_back(req);

    // Should fail without tag
    EXPECT_FALSE(journal.AcceptQuest("tagged_quest", def));

    journal.AddTag("hero_path");

    // Now should succeed
    EXPECT_TRUE(journal.AcceptQuest("tagged_quest", def));
}

// Test questId/definition.QuestID mismatch is rejected
TEST_F(QuestSystemTest, AcceptQuest_MismatchedQuestID)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("real_id");

    EXPECT_FALSE(journal.AcceptQuest("wrong_id", def));
}

// Test completing a quest that isn't ready (stages not done) is rejected
TEST_F(QuestSystemTest, CompleteQuest_NotReady)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("incomplete_quest");

    journal.AcceptQuest("incomplete_quest", def);
    // Don't complete any objectives

    auto result = journal.CompleteQuest("incomplete_quest");
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(journal.IsQuestActive("incomplete_quest"));
}

///////////////////////////////////////////////////////////////////////////////
// Flexible Requirement System Tests
///////////////////////////////////////////////////////////////////////////////

// Test Level requirement
TEST_F(QuestSystemTest, Requirement_Level)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("level_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::Level;
    req.Value = 5;
    req.Comparison = ComparisonOp::GreaterThanOrEqual;
    def.Requirements.push_back(req);

    // Level 0 → reject
    EXPECT_FALSE(journal.AcceptQuest("level_quest", def));

    // Level 4 → reject
    journal.SetPlayerLevel(4);
    EXPECT_FALSE(journal.AcceptQuest("level_quest", def));

    // Level 5 → accept
    journal.SetPlayerLevel(5);
    EXPECT_TRUE(journal.AcceptQuest("level_quest", def));
}

// Test Reputation requirement
TEST_F(QuestSystemTest, Requirement_Reputation)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("rep_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::Reputation;
    req.Target = "village_elder";
    req.Value = 100;
    req.Comparison = ComparisonOp::GreaterThanOrEqual;
    def.Requirements.push_back(req);

    // No reputation → reject
    EXPECT_FALSE(journal.AcceptQuest("rep_quest", def));

    // Reputation 50 → reject
    journal.SetReputation("village_elder", 50);
    EXPECT_FALSE(journal.AcceptQuest("rep_quest", def));

    // Reputation 100 → accept
    journal.SetReputation("village_elder", 100);
    EXPECT_TRUE(journal.AcceptQuest("rep_quest", def));
}

// Test Reputation with negative threshold (must have LOW reputation)
TEST_F(QuestSystemTest, Requirement_ReputationLessThan)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("villain_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::Reputation;
    req.Target = "village_elder";
    req.Value = -50;
    req.Comparison = ComparisonOp::LessThanOrEqual;
    def.Requirements.push_back(req);

    // Reputation 0 → reject (0 > -50)
    EXPECT_FALSE(journal.AcceptQuest("villain_quest", def));

    // Reputation -100 → accept
    journal.SetReputation("village_elder", -100);
    EXPECT_TRUE(journal.AcceptQuest("villain_quest", def));
}

// Test HasItem requirement
TEST_F(QuestSystemTest, Requirement_HasItem)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("item_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::HasItem;
    req.Target = "magic_amulet";
    req.Value = 1;
    req.Comparison = ComparisonOp::GreaterThanOrEqual;
    def.Requirements.push_back(req);

    // No items → reject
    EXPECT_FALSE(journal.AcceptQuest("item_quest", def));

    // Has amulet → accept
    journal.SetItemCount("magic_amulet", 1);
    EXPECT_TRUE(journal.AcceptQuest("item_quest", def));
}

// Test Stat requirement
TEST_F(QuestSystemTest, Requirement_Stat)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("stat_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::Stat;
    req.Target = "strength";
    req.Value = 10;
    req.Comparison = ComparisonOp::GreaterThanOrEqual;
    def.Requirements.push_back(req);

    EXPECT_FALSE(journal.AcceptQuest("stat_quest", def));

    journal.SetStat("strength", 10);
    EXPECT_TRUE(journal.AcceptQuest("stat_quest", def));
}

// Test IsClass requirement
TEST_F(QuestSystemTest, Requirement_IsClass)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("class_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::IsClass;
    req.Target = "Warrior";
    def.Requirements.push_back(req);

    // No class set → reject
    EXPECT_FALSE(journal.AcceptQuest("class_quest", def));

    // Wrong class → reject
    journal.SetPlayerClass("Mage");
    EXPECT_FALSE(journal.AcceptQuest("class_quest", def));

    // Correct class → accept
    journal.SetPlayerClass("Warrior");
    EXPECT_TRUE(journal.AcceptQuest("class_quest", def));
}

// Test IsFaction requirement
TEST_F(QuestSystemTest, Requirement_IsFaction)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("faction_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::IsFaction;
    req.Target = "Alliance";
    def.Requirements.push_back(req);

    EXPECT_FALSE(journal.AcceptQuest("faction_quest", def));

    journal.SetPlayerFaction("Horde");
    EXPECT_FALSE(journal.AcceptQuest("faction_quest", def));

    journal.SetPlayerFaction("Alliance");
    EXPECT_TRUE(journal.AcceptQuest("faction_quest", def));
}

// Test QuestCompleted requirement (via Requirements vector, not legacy field)
TEST_F(QuestSystemTest, Requirement_QuestCompleted)
{
    QuestJournal journal;
    auto prereq = MakeSimpleQuest("prereq");
    auto def = MakeSimpleQuest("main_q");

    QuestRequirement req;
    req.Type = QuestRequirementType::QuestCompleted;
    req.Target = "prereq";
    def.Requirements.push_back(req);

    EXPECT_FALSE(journal.AcceptQuest("main_q", def));

    // Complete prerequisite
    journal.AcceptQuest("prereq", prereq);
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }
    EXPECT_TRUE(journal.HasCompletedQuest("prereq"));

    EXPECT_TRUE(journal.AcceptQuest("main_q", def));
}

// Test QuestActive requirement
TEST_F(QuestSystemTest, Requirement_QuestActive)
{
    QuestJournal journal;
    auto companion = MakeSimpleQuest("companion_q");
    auto def = MakeSimpleQuest("parallel_q");

    QuestRequirement req;
    req.Type = QuestRequirementType::QuestActive;
    req.Target = "companion_q";
    def.Requirements.push_back(req);

    // Companion quest not active → reject
    EXPECT_FALSE(journal.AcceptQuest("parallel_q", def));

    // Accept companion quest → now accept parallel quest
    journal.AcceptQuest("companion_q", companion);
    EXPECT_TRUE(journal.AcceptQuest("parallel_q", def));
}

// Test QuestFailed requirement
TEST_F(QuestSystemTest, Requirement_QuestFailed)
{
    QuestJournal journal;
    auto failed_q = MakeSimpleQuest("fail_q");
    failed_q.CanFail = true;
    auto def = MakeSimpleQuest("redemption_q");

    QuestRequirement req;
    req.Type = QuestRequirementType::QuestFailed;
    req.Target = "fail_q";
    def.Requirements.push_back(req);

    EXPECT_FALSE(journal.AcceptQuest("redemption_q", def));

    journal.AcceptQuest("fail_q", failed_q);
    journal.FailQuest("fail_q");

    EXPECT_TRUE(journal.AcceptQuest("redemption_q", def));
}

// Test QuestNotStarted requirement
TEST_F(QuestSystemTest, Requirement_QuestNotStarted)
{
    QuestJournal journal;
    auto exclusive_q = MakeSimpleQuest("exclusive_q");
    auto def = MakeSimpleQuest("gated_q");

    QuestRequirement req;
    req.Type = QuestRequirementType::QuestNotStarted;
    req.Target = "exclusive_q";
    def.Requirements.push_back(req);

    // exclusive_q not started → accept
    EXPECT_TRUE(journal.AcceptQuest("gated_q", def));

    // Now start exclusive_q and try to accept gated_q again
    journal.AbandonQuest("gated_q");
    journal.AcceptQuest("exclusive_q", exclusive_q);

    EXPECT_FALSE(journal.AcceptQuest("gated_q", def));
}

// Test DoesNotHaveTag requirement
TEST_F(QuestSystemTest, Requirement_DoesNotHaveTag)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("good_quest");

    QuestRequirement req;
    req.Type = QuestRequirementType::DoesNotHaveTag;
    req.Target = "villain_path";
    def.Requirements.push_back(req);

    // No villain tag → accept
    EXPECT_TRUE(journal.AcceptQuest("good_quest", def));

    journal.AbandonQuest("good_quest");
    journal.AddTag("villain_path");

    // Has villain tag → reject
    EXPECT_FALSE(journal.AcceptQuest("good_quest", def));
}

// Test HasTag requirement (via Requirements vector)
TEST_F(QuestSystemTest, Requirement_HasTag)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("tagged_quest2");

    QuestRequirement req;
    req.Type = QuestRequirementType::HasTag;
    req.Target = "hero_path";
    def.Requirements.push_back(req);

    EXPECT_FALSE(journal.AcceptQuest("tagged_quest2", def));

    journal.AddTag("hero_path");
    EXPECT_TRUE(journal.AcceptQuest("tagged_quest2", def));
}

// Test composite: All (AND) combinator
TEST_F(QuestSystemTest, Requirement_All_Combinator)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("combo_quest");

    QuestRequirement allReq;
    allReq.Type = QuestRequirementType::All;

    QuestRequirement levelReq;
    levelReq.Type = QuestRequirementType::Level;
    levelReq.Value = 5;
    levelReq.Comparison = ComparisonOp::GreaterThanOrEqual;
    allReq.Children.push_back(levelReq);

    QuestRequirement tagReq;
    tagReq.Type = QuestRequirementType::HasTag;
    tagReq.Target = "warrior_training";
    allReq.Children.push_back(tagReq);

    def.Requirements.push_back(allReq);

    // Neither met → reject
    EXPECT_FALSE(journal.AcceptQuest("combo_quest", def));

    // Only level → reject
    journal.SetPlayerLevel(5);
    EXPECT_FALSE(journal.AcceptQuest("combo_quest", def));

    // Both met → accept
    journal.AddTag("warrior_training");
    EXPECT_TRUE(journal.AcceptQuest("combo_quest", def));
}

// Test composite: Any (OR) combinator
TEST_F(QuestSystemTest, Requirement_Any_Combinator)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("any_quest");

    QuestRequirement anyReq;
    anyReq.Type = QuestRequirementType::Any;

    QuestRequirement classReq;
    classReq.Type = QuestRequirementType::IsClass;
    classReq.Target = "Warrior";
    anyReq.Children.push_back(classReq);

    QuestRequirement classReq2;
    classReq2.Type = QuestRequirementType::IsClass;
    classReq2.Target = "Paladin";
    anyReq.Children.push_back(classReq2);

    def.Requirements.push_back(anyReq);

    // No class → reject
    EXPECT_FALSE(journal.AcceptQuest("any_quest", def));

    // Mage → reject (neither Warrior nor Paladin)
    journal.SetPlayerClass("Mage");
    EXPECT_FALSE(journal.AcceptQuest("any_quest", def));

    // Paladin → accept (matches one)
    journal.SetPlayerClass("Paladin");
    EXPECT_TRUE(journal.AcceptQuest("any_quest", def));
}

// Test composite: Not combinator
TEST_F(QuestSystemTest, Requirement_Not_Combinator)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("not_quest");

    QuestRequirement notReq;
    notReq.Type = QuestRequirementType::Not;

    QuestRequirement tagReq;
    tagReq.Type = QuestRequirementType::HasTag;
    tagReq.Target = "banned";
    notReq.Children.push_back(tagReq);

    def.Requirements.push_back(notReq);

    // No "banned" tag → accept (NOT passes)
    EXPECT_TRUE(journal.AcceptQuest("not_quest", def));

    journal.AbandonQuest("not_quest");
    journal.AddTag("banned");

    // Has "banned" tag → reject (NOT fails)
    EXPECT_FALSE(journal.AcceptQuest("not_quest", def));
}

// Test nested combinators: All(Level >= 10, Any(IsClass Warrior, IsClass Paladin))
TEST_F(QuestSystemTest, Requirement_NestedCombinators)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("nested_quest");

    QuestRequirement allReq;
    allReq.Type = QuestRequirementType::All;

    QuestRequirement levelReq;
    levelReq.Type = QuestRequirementType::Level;
    levelReq.Value = 10;
    levelReq.Comparison = ComparisonOp::GreaterThanOrEqual;
    allReq.Children.push_back(levelReq);

    QuestRequirement anyReq;
    anyReq.Type = QuestRequirementType::Any;
    QuestRequirement c1;
    c1.Type = QuestRequirementType::IsClass;
    c1.Target = "Warrior";
    anyReq.Children.push_back(c1);
    QuestRequirement c2;
    c2.Type = QuestRequirementType::IsClass;
    c2.Target = "Paladin";
    anyReq.Children.push_back(c2);
    allReq.Children.push_back(anyReq);

    def.Requirements.push_back(allReq);

    // Nothing set → reject
    EXPECT_FALSE(journal.AcceptQuest("nested_quest", def));

    // Level 10, no class → reject
    journal.SetPlayerLevel(10);
    EXPECT_FALSE(journal.AcceptQuest("nested_quest", def));

    // Level 10, Mage → reject
    journal.SetPlayerClass("Mage");
    EXPECT_FALSE(journal.AcceptQuest("nested_quest", def));

    // Level 10, Warrior → accept
    journal.SetPlayerClass("Warrior");
    EXPECT_TRUE(journal.AcceptQuest("nested_quest", def));
}

// Test multiple top-level requirements (implicit AND)
TEST_F(QuestSystemTest, Requirement_MultipleTopLevel)
{
    QuestJournal journal;
    auto def = MakeSimpleQuest("multi_req_quest");

    QuestRequirement levelReq;
    levelReq.Type = QuestRequirementType::Level;
    levelReq.Value = 3;
    levelReq.Comparison = ComparisonOp::GreaterThanOrEqual;
    def.Requirements.push_back(levelReq);

    QuestRequirement repReq;
    repReq.Type = QuestRequirementType::Reputation;
    repReq.Target = "thieves_guild";
    repReq.Value = 50;
    repReq.Comparison = ComparisonOp::GreaterThanOrEqual;
    def.Requirements.push_back(repReq);

    QuestRequirement tagReq;
    tagReq.Type = QuestRequirementType::HasTag;
    tagReq.Target = "guild_member";
    def.Requirements.push_back(tagReq);

    // None met → reject
    EXPECT_FALSE(journal.AcceptQuest("multi_req_quest", def));

    // Level only → reject
    journal.SetPlayerLevel(5);
    EXPECT_FALSE(journal.AcceptQuest("multi_req_quest", def));

    // Level + reputation → reject
    journal.SetReputation("thieves_guild", 75);
    EXPECT_FALSE(journal.AcceptQuest("multi_req_quest", def));

    // All met → accept
    journal.AddTag("guild_member");
    EXPECT_TRUE(journal.AcceptQuest("multi_req_quest", def));
}

// Test GetUnmetRequirements
TEST_F(QuestSystemTest, GetUnmetRequirements)
{
    QuestJournal journal;

    QuestRequirement levelReq;
    levelReq.Type = QuestRequirementType::Level;
    levelReq.Value = 10;
    levelReq.Comparison = ComparisonOp::GreaterThanOrEqual;
    levelReq.Description = "Level 10";

    QuestRequirement tagReq;
    tagReq.Type = QuestRequirementType::HasTag;
    tagReq.Target = "hero_path";
    tagReq.Description = "Hero path";

    std::vector<QuestRequirement> reqs = { levelReq, tagReq };

    // Both unmet
    auto unmet = journal.GetUnmetRequirements(reqs);
    EXPECT_EQ(unmet.size(), 2u);

    // Meet level, tag still unmet
    journal.SetPlayerLevel(10);
    unmet = journal.GetUnmetRequirements(reqs);
    EXPECT_EQ(unmet.size(), 1u);
    EXPECT_EQ(unmet[0]->Description, "Hero path");

    // Meet both
    journal.AddTag("hero_path");
    unmet = journal.GetUnmetRequirements(reqs);
    EXPECT_TRUE(unmet.empty());
}

// Test CheckRequirement directly
TEST_F(QuestSystemTest, CheckRequirement_Direct)
{
    QuestJournal journal;

    QuestRequirement req;
    req.Type = QuestRequirementType::Level;
    req.Value = 5;
    req.Comparison = ComparisonOp::GreaterThanOrEqual;

    EXPECT_FALSE(journal.CheckRequirement(req));
    journal.SetPlayerLevel(5);
    EXPECT_TRUE(journal.CheckRequirement(req));
}

// Test comparison operators
TEST_F(QuestSystemTest, ComparisonOperators)
{
    EXPECT_TRUE(EvaluateComparison(5, ComparisonOp::Equal, 5));
    EXPECT_FALSE(EvaluateComparison(5, ComparisonOp::Equal, 6));

    EXPECT_TRUE(EvaluateComparison(5, ComparisonOp::NotEqual, 6));
    EXPECT_FALSE(EvaluateComparison(5, ComparisonOp::NotEqual, 5));

    EXPECT_TRUE(EvaluateComparison(5, ComparisonOp::GreaterThan, 4));
    EXPECT_FALSE(EvaluateComparison(5, ComparisonOp::GreaterThan, 5));

    EXPECT_TRUE(EvaluateComparison(5, ComparisonOp::GreaterThanOrEqual, 5));
    EXPECT_FALSE(EvaluateComparison(5, ComparisonOp::GreaterThanOrEqual, 6));

    EXPECT_TRUE(EvaluateComparison(5, ComparisonOp::LessThan, 6));
    EXPECT_FALSE(EvaluateComparison(5, ComparisonOp::LessThan, 5));

    EXPECT_TRUE(EvaluateComparison(5, ComparisonOp::LessThanOrEqual, 5));
    EXPECT_FALSE(EvaluateComparison(5, ComparisonOp::LessThanOrEqual, 4));
}

// Test RequirementType string conversion
TEST_F(QuestSystemTest, RequirementTypeStringConversion)
{
    EXPECT_STREQ(RequirementTypeToString(QuestRequirementType::QuestCompleted), "QuestCompleted");
    EXPECT_STREQ(RequirementTypeToString(QuestRequirementType::Level), "Level");
    EXPECT_STREQ(RequirementTypeToString(QuestRequirementType::Reputation), "Reputation");
    EXPECT_STREQ(RequirementTypeToString(QuestRequirementType::HasTag), "HasTag");
    EXPECT_STREQ(RequirementTypeToString(QuestRequirementType::All), "All");
    EXPECT_STREQ(RequirementTypeToString(QuestRequirementType::Not), "Not");

    EXPECT_EQ(RequirementTypeFromString("QuestCompleted"), QuestRequirementType::QuestCompleted);
    EXPECT_EQ(RequirementTypeFromString("Level"), QuestRequirementType::Level);
    EXPECT_EQ(RequirementTypeFromString("Reputation"), QuestRequirementType::Reputation);
    EXPECT_EQ(RequirementTypeFromString("Any"), QuestRequirementType::Any);
    EXPECT_FALSE(RequirementTypeFromString("InvalidType").has_value());
    EXPECT_FALSE(RequirementTypeFromString("").has_value());
}

// Test ComparisonOp string conversion (including shorthand operators)
TEST_F(QuestSystemTest, ComparisonOpStringConversion)
{
    EXPECT_EQ(ComparisonOpFromString("Equal"), ComparisonOp::Equal);
    EXPECT_EQ(ComparisonOpFromString("=="), ComparisonOp::Equal);
    EXPECT_EQ(ComparisonOpFromString("EQ"), ComparisonOp::Equal);

    EXPECT_EQ(ComparisonOpFromString("GreaterThanOrEqual"), ComparisonOp::GreaterThanOrEqual);
    EXPECT_EQ(ComparisonOpFromString(">="), ComparisonOp::GreaterThanOrEqual);
    EXPECT_EQ(ComparisonOpFromString("GTE"), ComparisonOp::GreaterThanOrEqual);

    EXPECT_EQ(ComparisonOpFromString("LessThan"), ComparisonOp::LessThan);
    EXPECT_EQ(ComparisonOpFromString("<"), ComparisonOp::LessThan);
    EXPECT_EQ(ComparisonOpFromString("LT"), ComparisonOp::LessThan);
    EXPECT_FALSE(ComparisonOpFromString("BadOp").has_value());
    EXPECT_FALSE(ComparisonOpFromString("").has_value());
}

// Test player state persistence via setters/getters
TEST_F(QuestSystemTest, PlayerState_SettersGetters)
{
    QuestJournal journal;

    // Level
    EXPECT_EQ(journal.GetPlayerLevel(), 0);
    journal.SetPlayerLevel(42);
    EXPECT_EQ(journal.GetPlayerLevel(), 42);

    // Reputation
    EXPECT_EQ(journal.GetReputation("unknown_faction"), 0);
    journal.SetReputation("thieves_guild", 150);
    EXPECT_EQ(journal.GetReputation("thieves_guild"), 150);

    // Items
    EXPECT_EQ(journal.GetItemCount("sword"), 0);
    journal.SetItemCount("sword", 3);
    EXPECT_EQ(journal.GetItemCount("sword"), 3);

    // Stats
    EXPECT_EQ(journal.GetStat("charisma"), 0);
    journal.SetStat("charisma", 18);
    EXPECT_EQ(journal.GetStat("charisma"), 18);

    // Class
    EXPECT_TRUE(journal.GetPlayerClass().empty());
    journal.SetPlayerClass("Rogue");
    EXPECT_EQ(journal.GetPlayerClass(), "Rogue");

    // Faction
    EXPECT_TRUE(journal.GetPlayerFaction().empty());
    journal.SetPlayerFaction("Horde");
    EXPECT_EQ(journal.GetPlayerFaction(), "Horde");
}

// Test empty All combinator (vacuously true)
TEST_F(QuestSystemTest, Requirement_EmptyAll)
{
    QuestJournal journal;
    QuestRequirement req;
    req.Type = QuestRequirementType::All;
    // No children
    EXPECT_TRUE(journal.CheckRequirement(req));
}

// Test empty Any combinator (no children → false)
TEST_F(QuestSystemTest, Requirement_EmptyAny)
{
    QuestJournal journal;
    QuestRequirement req;
    req.Type = QuestRequirementType::Any;
    // No children → at least one must pass, but none exist → false
    EXPECT_FALSE(journal.CheckRequirement(req));
}

// Test empty Not combinator (expects exactly one child → false)
TEST_F(QuestSystemTest, Requirement_EmptyNot)
{
    QuestJournal journal;
    QuestRequirement req;
    req.Type = QuestRequirementType::Not;
    // No children → malformed → false
    EXPECT_FALSE(journal.CheckRequirement(req));
}

// Test multiple requirements together (quest + tag via Requirements)
TEST_F(QuestSystemTest, RequirementsCombo_QuestAndTag)
{
    QuestJournal journal;
    auto prereq = MakeSimpleQuest("combo_prereq");
    auto def = MakeSimpleQuest("combo_quest2");

    QuestRequirement questReq;
    questReq.Type = QuestRequirementType::QuestCompleted;
    questReq.Target = "combo_prereq";
    def.Requirements.push_back(questReq);

    QuestRequirement tagReq;
    tagReq.Type = QuestRequirementType::HasTag;
    tagReq.Target = "combo_tag";
    def.Requirements.push_back(tagReq);

    // Both missing → reject
    EXPECT_FALSE(journal.AcceptQuest("combo_quest2", def));

    // Complete prereq, add tag → accept
    journal.AcceptQuest("combo_prereq", prereq);
    for (int i = 0; i < 5; ++i)
    {
        journal.NotifyKill("wolf");
    }
    journal.AddTag("combo_tag");

    EXPECT_TRUE(journal.AcceptQuest("combo_quest2", def));
}
