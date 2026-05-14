#include "OloEnginePCH.h"

// =============================================================================
// QuestJournalSceneYAMLRoundTripTest — Functional Test.
//
// Cross-subsystem seam under test:
//   QuestJournalComponent × SceneSerializer::SerializeToYAML ×
//   DeserializeFromYAML × QuestJournal active-state reconstruction.
//   The Scene YAML path writes:
//     - Active quest IDs + their current stage index + ElapsedTime
//     - Each ObjectiveState (ID, CurrentCount, IsCompleted)
//     - Completed / Failed quest sets, owned tags, player level / faction
//     - The full QuestDefinition for each active quest (so the runtime
//       can rehydrate without needing the original database).
//   This component is NOT on SaveGameSerializer's curated whitelist —
//   the round-trip lives entirely in Scene YAML, which is the format
//   .olo scene files use. A regression there silently drops every
//   in-progress quest at scene load.
//
// Scenario: a player entity with one Active quest (3-stage kill quest),
// with the first stage's objective at CurrentCount=2 (not yet complete).
// Marked one quest as completed previously. Player has two granted tags
// and a non-default faction. Serialize → deserialize. Verify the
// rehydrated journal restores every piece.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class QuestJournalSceneYAMLRoundTripTest : public FunctionalTest
{
  protected:
    static constexpr const char* kQuestID = "Q_Wolves";
    static constexpr const char* kObjID = "kill_wolves";
    static constexpr const char* kCompletedID = "Q_Tutorial";

    void BuildScene() override
    {
        m_Player = GetScene().CreateEntity("Hero");
        auto& jc = m_Player.AddComponent<QuestJournalComponent>();

        QuestDefinition def;
        def.QuestID = kQuestID;
        def.Title = "Wolves";
        def.Category = "Side";

        QuestStage stage;
        stage.StageID = "stage0";
        stage.RequireAllObjectives = true;
        QuestObjective obj;
        obj.ObjectiveID = kObjID;
        obj.ObjectiveType = QuestObjective::Type::Kill;
        obj.TargetID = "wolf";
        obj.RequiredCount = 3;
        stage.Objectives.push_back(std::move(obj));
        def.Stages.push_back(std::move(stage));

        ASSERT_TRUE(jc.Journal.AcceptQuest(kQuestID, def));
        jc.Journal.IncrementObjective(kQuestID, kObjID, 2); // 2/3, not complete

        jc.Journal.AddCompletedQuestID(kCompletedID);
        jc.Journal.AddTag("HeroOfTheVillage");
        jc.Journal.AddTag("Friend_Of_Wolves");
        jc.Journal.SetPlayerLevel(7);
        jc.Journal.SetPlayerFaction("Rangers");
    }

    Entity m_Player;
};

TEST_F(QuestJournalSceneYAMLRoundTripTest, ActiveAndCompletedQuestsAndPlayerStateSurviveSceneYAMLRoundTrip)
{
    SceneSerializer serializer(GetSceneRef());
    const std::string yaml = serializer.SerializeToYAML();
    ASSERT_FALSE(yaml.empty());

    Ref<Scene> restored = Scene::Create();
    restored->SetRenderingEnabled(false);
    SceneSerializer restoreSerializer(restored);
    ASSERT_TRUE(restoreSerializer.DeserializeFromYAML(yaml))
        << "Scene YAML round-trip failed at deserialize step.";

    Entity restoredPlayer = restored->FindEntityByName("Hero");
    ASSERT_TRUE(restoredPlayer);
    ASSERT_TRUE(restoredPlayer.HasComponent<QuestJournalComponent>())
        << "QuestJournalComponent missing from restored player — Scene serializer "
           "is failing to claim the YAML node, or the writer skipped emitting it.";

    const auto& journal = restoredPlayer.GetComponent<QuestJournalComponent>().Journal;

    // Active quest preserved with correct progress.
    EXPECT_EQ(journal.GetQuestStatus(kQuestID), QuestStatus::Active);
    const auto* obj = journal.GetObjective(kQuestID, kObjID);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->CurrentCount, 2)
        << "objective progress lost in round-trip — CurrentCount didn't survive.";
    EXPECT_FALSE(obj->IsCompleted);
    EXPECT_EQ(obj->RequiredCount, 3)
        << "objective definition lost — RequiredCount didn't survive (the embedded "
           "QuestDefinition isn't being written).";

    // Completed-quest ID preserved.
    EXPECT_TRUE(journal.HasCompletedQuest(kCompletedID))
        << "previously-completed quest dropped from m_CompletedQuestIDs.";

    // Granted tags preserved.
    EXPECT_TRUE(journal.HasTag("HeroOfTheVillage"));
    EXPECT_TRUE(journal.HasTag("Friend_Of_Wolves"));

    // Player external state preserved.
    EXPECT_EQ(journal.GetPlayerLevel(), 7);
    EXPECT_EQ(journal.GetPlayerFaction(), std::string("Rangers"));
}
