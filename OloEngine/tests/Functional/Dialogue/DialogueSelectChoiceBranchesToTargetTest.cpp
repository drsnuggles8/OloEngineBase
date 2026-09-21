#include "OloEnginePCH.h"

// =============================================================================
// DialogueSelectChoiceBranchesToTargetTest — Functional Test.
//
// Cross-subsystem seam under test:
//   DialogueSystem::ProcessNode ("choice" branch) × m_AvailableChoices
//   population from outgoing connections × DialogueSystem::SelectChoice
//   (target traversal) × DialogueState transitions Displaying →
//   WaitingForChoice → Processing → Displaying. The dialogue UI shows a
//   choice prompt; when the player picks one, the state machine must
//   walk the corresponding edge and land on the right target node.
//   A regression that maps SelectChoice's index to the wrong connection
//   (e.g. ordering bug, off-by-one) silently sends every player down
//   the wrong narrative branch.
//
// Scenario: dialogue tree:
//
//          [Start]
//             ↓
//          [Choice]
//          /     \
//      [Left]   [Right]
//
// Start the dialogue, walk to the choice node, verify two choices are
// available, SelectChoice(1) → land on the Right node (not Left).
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Dialogue/DialogueSystem.h"
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"

using namespace OloEngine;
using namespace OloEngine::Functional;

class DialogueSelectChoiceBranchesToTargetTest : public FunctionalTest
{
  protected:
    static constexpr u64 kStart_ID = 0x1ULL;
    static constexpr u64 kChoice_ID = 0x2ULL;
    static constexpr u64 kLeft_ID = 0x3ULL;
    static constexpr u64 kRight_ID = 0x4ULL;
    static constexpr const char* kRightText = "took the right path";

    void BuildScene() override
    {
        EnableAssetManager({});

        m_TreeAsset = Ref<DialogueTreeAsset>::Create();
        auto& nodes = m_TreeAsset->GetNodesWritable();
        auto& edges = m_TreeAsset->GetConnectionsWritable();

        // Start node — dialogue type, no text required (we'll advance past it).
        DialogueNodeData start;
        start.ID = OloEngine::UUID{ kStart_ID };
        start.Type = "dialogue";
        start.Name = "Start";
        start.Properties.emplace("text", DialoguePropertyValue{ std::string("Pick.") });
        nodes.AddTail(std::move(start));

        // Choice node — branches to Left/Right.
        DialogueNodeData choiceNode;
        choiceNode.ID = OloEngine::UUID{ kChoice_ID };
        choiceNode.Type = "choice";
        choiceNode.Name = "Choice";
        nodes.AddTail(std::move(choiceNode));

        // Left target.
        DialogueNodeData left;
        left.ID = OloEngine::UUID{ kLeft_ID };
        left.Type = "dialogue";
        left.Name = "Left";
        left.Properties.emplace("text", DialoguePropertyValue{ std::string("took the left path") });
        nodes.AddTail(std::move(left));

        // Right target.
        DialogueNodeData right;
        right.ID = OloEngine::UUID{ kRight_ID };
        right.Type = "dialogue";
        right.Name = "Right";
        right.Properties.emplace("text", DialoguePropertyValue{ std::string(kRightText) });
        nodes.AddTail(std::move(right));

        // Edges: Start → Choice, Choice → Left, Choice → Right.
        edges.Add({ OloEngine::UUID{ kStart_ID }, OloEngine::UUID{ kChoice_ID }, "", "" });
        edges.Add({ OloEngine::UUID{ kChoice_ID }, OloEngine::UUID{ kLeft_ID }, "Left", "" });
        edges.Add({ OloEngine::UUID{ kChoice_ID }, OloEngine::UUID{ kRight_ID }, "Right", "" });

        m_TreeAsset->SetRootNodeID(OloEngine::UUID{ kStart_ID });
        m_TreeAsset->RebuildNodeIndex();

        m_TreeHandle = AssetManager::AddMemoryOnlyAsset<DialogueTreeAsset>(m_TreeAsset);
        ASSERT_NE(static_cast<u64>(m_TreeHandle), 0ULL);

        m_Speaker = GetScene().CreateEntity("Speaker");
        auto& dc = m_Speaker.AddComponent<DialogueComponent>();
        dc.m_DialogueTree = m_TreeHandle;
        m_Speaker.AddComponent<DialogueStateComponent>();

        EnableDialogue();
    }

    Ref<DialogueTreeAsset> m_TreeAsset;
    AssetHandle m_TreeHandle{};
    Entity m_Speaker;
};

TEST_F(DialogueSelectChoiceBranchesToTargetTest, SelectChoiceWalksTheChosenEdgeAndLandsOnTargetNode)
{
    auto* dialogueSystem = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogueSystem, nullptr);

    dialogueSystem->StartDialogue(m_Speaker);
    auto& state = m_Speaker.GetComponent<DialogueStateComponent>();
    ASSERT_EQ(static_cast<u64>(state.m_CurrentNodeID), kStart_ID);

    // Wait for the start node's reveal to finish, then advance to the choice node.
    TickFor(1.5f);
    dialogueSystem->AdvanceDialogue(m_Speaker);

    EXPECT_EQ(static_cast<u64>(state.m_CurrentNodeID), kChoice_ID)
        << "default edge from Start didn't land on the Choice node — "
           "ProcessNode's recursion for default-edges is broken.";
    ASSERT_EQ(state.m_State, DialogueState::WaitingForChoice)
        << "choice node didn't transition the dialogue into WaitingForChoice — "
           "the 'choice' branch in ProcessNode is wired wrong.";
    ASSERT_EQ(state.m_AvailableChoices.Num(), 2u)
        << "expected two choices (Left, Right); got " << state.m_AvailableChoices.Num()
        << " — m_AvailableChoices isn't being populated from outgoing connections.";
    EXPECT_EQ(state.m_AvailableChoices[0].Text.ToStdString(), "Left");
    EXPECT_EQ(state.m_AvailableChoices[1].Text.ToStdString(), "Right");

    // Pick the second option (index 1) — should branch to the Right node.
    dialogueSystem->SelectChoice(m_Speaker, /*choiceIndex=*/1);

    EXPECT_EQ(static_cast<u64>(state.m_CurrentNodeID), kRight_ID)
        << "SelectChoice(1) didn't land on the Right node — index-to-edge "
           "mapping is wrong (index 1 mapped to Left, suggesting reversed "
           "iteration order in m_AvailableChoices population).";
    EXPECT_EQ(state.m_State, DialogueState::Displaying)
        << "after SelectChoice, state didn't transition back to Displaying — "
           "ProcessNode for the target 'dialogue' node didn't fire.";
    EXPECT_EQ(state.m_CurrentText, std::string(kRightText));
    EXPECT_EQ(state.m_SelectedChoiceIndex, 1)
        << "m_SelectedChoiceIndex wasn't updated to record which option was taken.";
}

TEST_F(DialogueSelectChoiceBranchesToTargetTest, ChoiceConditionReachesStringHandlerWithoutChangingLabel)
{
    auto* dialogueSystem = GetScene().GetDialogueSystem();
    ASSERT_NE(dialogueSystem, nullptr);
    for (auto& node : m_TreeAsset->GetNodesWritable())
    {
        if (static_cast<u64>(node.ID) == kLeft_ID)
        {
            node.Properties["condition"] = std::string("locked");
        }
    }
    bool evaluated = false;
    dialogueSystem->RegisterConditionHandler("locked", [&](OloEngine::UUID entity, const std::string& name, const std::string& args)
                                             {
                                                 evaluated = true;
                                                 EXPECT_EQ(static_cast<u64>(entity), static_cast<u64>(m_Speaker.GetUUID()));
                                                 EXPECT_EQ(name, "locked");
                                                 EXPECT_TRUE(args.empty());
                                                 return false; });
    dialogueSystem->StartDialogue(m_Speaker);
    TickFor(1.5f);
    dialogueSystem->AdvanceDialogue(m_Speaker);

    const auto& state = m_Speaker.GetComponent<DialogueStateComponent>();
    ASSERT_TRUE(evaluated);
    ASSERT_EQ(state.m_AvailableChoices.Num(), 1u);
    EXPECT_EQ(state.m_AvailableChoices[0].Text.ToStdString(), "Right");
    EXPECT_EQ(static_cast<u64>(state.m_AvailableChoices[0].TargetNodeID), kRight_ID);
}
