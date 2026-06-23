#include "OloEnginePCH.h"

// =============================================================================
// DialogueAcceptsQuestAndGatesOnStateTest — Functional Test.
//
// Cross-subsystem seam under test:
//   DialogueSystem (action / condition node dispatch) ×
//   QuestDialogueBridge (the glue registered in Scene::InitDialogueSystem) ×
//   QuestSystem / QuestJournal (entity-aware quest state) ×
//   QuestGiverComponent (NPC's offered quests) × GameplayEventBus.
//
// This pins the dialogue->quest integration: an NPC conversation can accept a
// quest onto the player's journal, and a dialogue condition node can branch on
// whether that quest is active. Both are driven through the *real* registration
// path — EnableDialogue() -> Scene::InitDialogueSystem(), the same call the
// runtime makes at OnRuntimeStart — so the test exercises production wiring,
// not a test-only handler set.
//
// Scenarios:
//   1. An `accept_quest` action node with an explicit quest id moves the quest
//      Unavailable -> Active and publishes QuestStarted stamped with the
//      player's UUID; the dialogue then advances to its terminal line.
//   2. A bare `accept_quest` node (no args) falls back to the speaking NPC's
//      QuestGiverComponent.OfferedQuestIDs — wiring the giver component in.
//   3. A `quest_active` condition node takes the false branch before the quest
//      is accepted and the true branch after, proving conditions read live
//      journal state.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Gameplay/GameplayEventBus.h"
#include "OloEngine/Gameplay/Quest/QuestComponents.h"
#include "OloEngine/Gameplay/Quest/QuestSystem.h"
#include "OloEngine/Gameplay/Quest/QuestDatabase.h"
#include "OloEngine/Gameplay/Quest/QuestEvents.h"
#include "OloEngine/Gameplay/Quest/Quest.h"
#include "OloEngine/Gameplay/Quest/QuestObjective.h"

#include <string>
#include <string_view>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::Functional;

namespace
{
    constexpr const char* kQuestId = "dlg.test.fetch";

    // A minimal single-stage, single-objective quest so AcceptQuest's
    // "non-empty stages / requirements met" guards pass; the objective itself
    // is irrelevant to acceptance and state-gating.
    QuestDefinition MakeSimpleQuest(std::string_view id)
    {
        QuestDefinition def;
        def.QuestID = std::string(id);
        def.Title = std::string(id);

        QuestStage stage;
        stage.StageID = "stage0";
        QuestObjective obj;
        obj.ObjectiveID = "obj0";
        obj.ObjectiveType = QuestObjective::Type::Interact;
        obj.TargetID = "altar";
        obj.RequiredCount = 1;
        stage.Objectives.push_back(std::move(obj));
        def.Stages.push_back(std::move(stage));
        return def;
    }
} // namespace

class DialogueAcceptsQuestAndGatesOnStateTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        EnableAssetManager({});

        // QuestDatabase is a process-global singleton; register the test quest
        // for the accept handler's id->definition lookup, and clear in
        // TearDown so we don't leak state into other suites.
        QuestDatabase::Clear();
        QuestDatabase::Register(MakeSimpleQuest(kQuestId));

        m_Player = GetScene().CreateEntity("Player");
        m_Player.AddComponent<QuestJournalComponent>();
        m_PlayerUUID = m_Player.GetUUID();

        m_Npc = GetScene().CreateEntity("NPC");
        auto& giver = m_Npc.AddComponent<QuestGiverComponent>();
        giver.OfferedQuestIDs.emplace_back(kQuestId);
        // TriggerOnce off so the condition test can re-run the same dialogue.
        m_Npc.AddComponent<DialogueComponent>().m_TriggerOnce = false;
        m_Npc.AddComponent<DialogueStateComponent>();

        EnableDialogue(); // -> Scene::InitDialogueSystem() registers the bridge
    }

    void TearDown() override
    {
        QuestDatabase::Clear();
        FunctionalTest::TearDown();
    }

    // Attaches a built tree as a memory-only asset and points the NPC at it.
    // Takes the Ref by value: Ref<T> is const-propagating, so a const& would
    // only expose the const RebuildNodeIndex-less view of the asset.
    void AttachTree(Ref<DialogueTreeAsset> tree)
    {
        tree->RebuildNodeIndex();
        const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<DialogueTreeAsset>(tree);
        ASSERT_NE(static_cast<u64>(handle), 0ULL);
        m_Npc.GetComponent<DialogueComponent>().m_DialogueTree = handle;
    }

    QuestJournal& PlayerJournal()
    {
        return m_Player.GetComponent<QuestJournalComponent>().Journal;
    }

    Entity m_Player;
    Entity m_Npc;
    UUID m_PlayerUUID;
};

TEST_F(DialogueAcceptsQuestAndGatesOnStateTest, AcceptActionMovesQuestToActiveAndPublishesStarted)
{
    // tree: action(accept_quest, kQuestId) -> dialogue("Accepted!")
    auto tree = Ref<DialogueTreeAsset>::Create();

    DialogueNodeData action;
    action.ID = UUID{ 0xA1ULL };
    action.Type = "action";
    action.Name = "Accept";
    action.Properties.try_emplace("actionName", DialoguePropertyValue{ std::string("accept_quest") });
    action.Properties.try_emplace("actionArgs", DialoguePropertyValue{ std::string(kQuestId) });

    DialogueNodeData say;
    say.ID = UUID{ 0xB1ULL };
    say.Type = "dialogue";
    say.Name = "Done";
    say.Properties.try_emplace("text", DialoguePropertyValue{ std::string("Accepted!") });

    tree->GetNodesWritable().push_back(std::move(action));
    tree->GetNodesWritable().push_back(std::move(say));

    DialogueConnection conn;
    conn.SourceNodeID = UUID{ 0xA1ULL };
    conn.TargetNodeID = UUID{ 0xB1ULL };
    tree->GetConnectionsWritable().push_back(conn);
    tree->SetRootNodeID(UUID{ 0xA1ULL });
    AttachTree(tree);

    ASSERT_FALSE(PlayerJournal().IsQuestActive(kQuestId));
    EXPECT_EQ(PlayerJournal().GetQuestStatus(kQuestId), QuestStatus::Unavailable);

    std::vector<QuestStartedEvent> started;
    GetScene().GetGameplayEvents().Subscribe<QuestStartedEvent>([&started](const QuestStartedEvent& e)
                                                                { started.push_back(e); });

    auto* dialogueSystem = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogueSystem, nullptr);
    dialogueSystem->StartDialogue(m_Npc);

    // The action node ran the accept handler, which routed through QuestSystem
    // onto the player's journal: Unavailable -> Active.
    EXPECT_TRUE(PlayerJournal().IsQuestActive(kQuestId))
        << "accept_quest action did not move the quest to Active on the player journal.";
    EXPECT_EQ(PlayerJournal().GetQuestStatus(kQuestId), QuestStatus::Active);

    // QuestSystem (not the raw journal) published the entity-stamped event.
    ASSERT_EQ(started.size(), 1u) << "accept did not publish exactly one QuestStarted via the bridge.";
    EXPECT_EQ(started[0].QuestID, kQuestId);
    EXPECT_EQ(static_cast<u64>(started[0].EntityID), static_cast<u64>(m_PlayerUUID))
        << "QuestStarted carried the wrong owner — bridge resolved the wrong journal entity.";

    // Dialogue continued past the action node to the terminal line.
    const auto& state = m_Npc.GetComponent<DialogueStateComponent>();
    EXPECT_EQ(static_cast<u64>(state.m_CurrentNodeID), 0xB1ULL);
    EXPECT_EQ(state.m_CurrentText, std::string("Accepted!"));
}

TEST_F(DialogueAcceptsQuestAndGatesOnStateTest, BareAcceptActionUsesQuestGiverOfferedQuest)
{
    // tree: action(accept_quest, <no args>) -> dialogue. With empty args the
    // handler must fall back to the speaking NPC's QuestGiverComponent.
    auto tree = Ref<DialogueTreeAsset>::Create();

    DialogueNodeData action;
    action.ID = UUID{ 0xA2ULL };
    action.Type = "action";
    action.Name = "Offer";
    action.Properties.try_emplace("actionName", DialoguePropertyValue{ std::string("accept_quest") });
    // intentionally no "actionArgs" property

    DialogueNodeData say;
    say.ID = UUID{ 0xB2ULL };
    say.Type = "dialogue";
    say.Name = "Done";
    say.Properties.try_emplace("text", DialoguePropertyValue{ std::string("Take this.") });

    tree->GetNodesWritable().push_back(std::move(action));
    tree->GetNodesWritable().push_back(std::move(say));

    DialogueConnection conn;
    conn.SourceNodeID = UUID{ 0xA2ULL };
    conn.TargetNodeID = UUID{ 0xB2ULL };
    tree->GetConnectionsWritable().push_back(conn);
    tree->SetRootNodeID(UUID{ 0xA2ULL });
    AttachTree(tree);

    ASSERT_FALSE(PlayerJournal().IsQuestActive(kQuestId));

    auto* dialogueSystem = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogueSystem, nullptr);
    dialogueSystem->StartDialogue(m_Npc);

    EXPECT_TRUE(PlayerJournal().IsQuestActive(kQuestId))
        << "bare accept_quest did not pick up the NPC's QuestGiverComponent.OfferedQuestIDs[0].";
}

TEST_F(DialogueAcceptsQuestAndGatesOnStateTest, ConditionNodeBranchesOnQuestActiveState)
{
    // tree: condition(quest_active, kQuestId) --true--> "HaveIt"
    //                                          --false--> "GoGetIt"
    auto tree = Ref<DialogueTreeAsset>::Create();

    DialogueNodeData cond;
    cond.ID = UUID{ 0xC1ULL };
    cond.Type = "condition";
    cond.Name = "CheckQuest";
    cond.Properties.try_emplace("conditionExpression", DialoguePropertyValue{ std::string("quest_active") });
    cond.Properties.try_emplace("conditionArgs", DialoguePropertyValue{ std::string(kQuestId) });

    DialogueNodeData yes;
    yes.ID = UUID{ 0xD1ULL };
    yes.Type = "dialogue";
    yes.Name = "Yes";
    yes.Properties.try_emplace("text", DialoguePropertyValue{ std::string("HaveIt") });

    DialogueNodeData no;
    no.ID = UUID{ 0xE1ULL };
    no.Type = "dialogue";
    no.Name = "No";
    no.Properties.try_emplace("text", DialoguePropertyValue{ std::string("GoGetIt") });

    tree->GetNodesWritable().push_back(std::move(cond));
    tree->GetNodesWritable().push_back(std::move(yes));
    tree->GetNodesWritable().push_back(std::move(no));

    DialogueConnection trueConn;
    trueConn.SourceNodeID = UUID{ 0xC1ULL };
    trueConn.TargetNodeID = UUID{ 0xD1ULL };
    trueConn.SourcePort = "true";
    tree->GetConnectionsWritable().push_back(trueConn);

    DialogueConnection falseConn;
    falseConn.SourceNodeID = UUID{ 0xC1ULL };
    falseConn.TargetNodeID = UUID{ 0xE1ULL };
    falseConn.SourcePort = "false";
    tree->GetConnectionsWritable().push_back(falseConn);

    tree->SetRootNodeID(UUID{ 0xC1ULL });
    AttachTree(tree);

    auto* dialogueSystem = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogueSystem, nullptr);

    // Quest not yet accepted -> condition false -> false branch.
    dialogueSystem->StartDialogue(m_Npc);
    {
        const auto& state = m_Npc.GetComponent<DialogueStateComponent>();
        EXPECT_EQ(static_cast<u64>(state.m_CurrentNodeID), 0xE1ULL)
            << "quest_active should be false before accept — expected the false branch.";
        EXPECT_EQ(state.m_CurrentText, std::string("GoGetIt"));
    }

    // Accept the quest (directly via the service layer; the accept path itself
    // is covered by the action-node test above), then re-run the dialogue.
    ASSERT_TRUE(QuestSystem::AcceptQuest(&GetScene(), m_Player, kQuestId));

    dialogueSystem->StartDialogue(m_Npc);
    {
        const auto& state = m_Npc.GetComponent<DialogueStateComponent>();
        EXPECT_EQ(static_cast<u64>(state.m_CurrentNodeID), 0xD1ULL)
            << "quest_active should be true after accept — expected the true branch.";
        EXPECT_EQ(state.m_CurrentText, std::string("HaveIt"));
    }
}
