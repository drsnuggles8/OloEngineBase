#include <gtest/gtest.h>
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTypes.h"
#include "OloEngine/Scene/Components.h"

using namespace OloEngine;

// ============================================================================
// DialogueTreeAsset query tests
// ============================================================================

class DialogueTreeAssetTest : public ::testing::Test
{
  protected:
    Ref<DialogueTreeAsset> CreateSampleTree()
    {
        auto asset = Ref<DialogueTreeAsset>::Create();

        DialogueNodeData dialogueNode;
        dialogueNode.ID = UUID(100);
        dialogueNode.Type = "dialogue";
        dialogueNode.Name = "Greeting";
        dialogueNode.Properties["speaker"] = std::string("Guard");
        dialogueNode.Properties["text"] = std::string("Halt!");

        DialogueNodeData choiceNode;
        choiceNode.ID = UUID(200);
        choiceNode.Type = "choice";
        choiceNode.Name = "PlayerChoice";

        DialogueNodeData conditionNode;
        conditionNode.ID = UUID(300);
        conditionNode.Type = "condition";
        conditionNode.Name = "HasKey";
        conditionNode.Properties["conditionExpression"] = std::string("hasKey");

        DialogueNodeData responseTrue;
        responseTrue.ID = UUID(400);
        responseTrue.Type = "dialogue";
        responseTrue.Name = "WelcomeBack";
        responseTrue.Properties["text"] = std::string("Welcome back!");

        DialogueNodeData responseFalse;
        responseFalse.ID = UUID(500);
        responseFalse.Type = "dialogue";
        responseFalse.Name = "GoAway";
        responseFalse.Properties["text"] = std::string("Move along.");

        asset->GetNodesWritable().push_back(std::move(dialogueNode));
        asset->GetNodesWritable().push_back(std::move(choiceNode));
        asset->GetNodesWritable().push_back(std::move(conditionNode));
        asset->GetNodesWritable().push_back(std::move(responseTrue));
        asset->GetNodesWritable().push_back(std::move(responseFalse));

        // Connections: dialogue -> choice -> condition -> true/false branches
        DialogueConnection conn1;
        conn1.SourceNodeID = UUID(100);
        conn1.TargetNodeID = UUID(200);
        conn1.SourcePort = "output";
        conn1.TargetPort = "input";

        DialogueConnection conn2;
        conn2.SourceNodeID = UUID(200);
        conn2.TargetNodeID = UUID(300);
        conn2.SourcePort = "Check pass";
        conn2.TargetPort = "input";

        DialogueConnection conn3;
        conn3.SourceNodeID = UUID(300);
        conn3.TargetNodeID = UUID(400);
        conn3.SourcePort = "true";
        conn3.TargetPort = "input";

        DialogueConnection conn4;
        conn4.SourceNodeID = UUID(300);
        conn4.TargetNodeID = UUID(500);
        conn4.SourcePort = "false";
        conn4.TargetPort = "input";

        asset->GetConnectionsWritable().push_back(std::move(conn1));
        asset->GetConnectionsWritable().push_back(std::move(conn2));
        asset->GetConnectionsWritable().push_back(std::move(conn3));
        asset->GetConnectionsWritable().push_back(std::move(conn4));

        asset->SetRootNodeID(UUID(100));
        asset->RebuildNodeIndex();
        return asset;
    }
};

TEST_F(DialogueTreeAssetTest, FindNodeReturnsCorrectNode)
{
    auto tree = CreateSampleTree();

    auto* node = tree->FindNode(UUID(100));
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->Name, "Greeting");
    EXPECT_EQ(node->Type, "dialogue");

    auto* choice = tree->FindNode(UUID(200));
    ASSERT_NE(choice, nullptr);
    EXPECT_EQ(choice->Name, "PlayerChoice");
    EXPECT_EQ(choice->Type, "choice");
}

TEST_F(DialogueTreeAssetTest, FindNodeReturnsNullForMissing)
{
    auto tree = CreateSampleTree();
    EXPECT_EQ(tree->FindNode(UUID(9999)), nullptr);
}

TEST_F(DialogueTreeAssetTest, GetConnectionsFromReturnsAll)
{
    auto tree = CreateSampleTree();

    // Condition node (300) has two outgoing connections (true/false)
    auto connections = tree->GetConnectionsFrom(UUID(300));
    EXPECT_EQ(connections.size(), 2u);
}

TEST_F(DialogueTreeAssetTest, GetConnectionsFromWithPort)
{
    auto tree = CreateSampleTree();

    auto trueConns = tree->GetConnectionsFrom(UUID(300), "true");
    ASSERT_EQ(trueConns.size(), 1u);
    EXPECT_EQ(static_cast<u64>(trueConns[0].TargetNodeID), 400u);

    auto falseConns = tree->GetConnectionsFrom(UUID(300), "false");
    ASSERT_EQ(falseConns.size(), 1u);
    EXPECT_EQ(static_cast<u64>(falseConns[0].TargetNodeID), 500u);
}

TEST_F(DialogueTreeAssetTest, GetConnectionsFromEmptyForLeafNode)
{
    auto tree = CreateSampleTree();

    // Node 400 (a leaf dialogue node) has no outgoing connections
    auto connections = tree->GetConnectionsFrom(UUID(400));
    EXPECT_TRUE(connections.empty());
}

TEST_F(DialogueTreeAssetTest, RootNodeIDAccessor)
{
    auto tree = CreateSampleTree();
    EXPECT_EQ(static_cast<u64>(tree->GetRootNodeID()), 100u);
}

TEST_F(DialogueTreeAssetTest, AssetTypeIsDialogueTree)
{
    auto tree = Ref<DialogueTreeAsset>::Create();
    EXPECT_EQ(tree->GetAssetType(), AssetType::DialogueTree);
    EXPECT_EQ(DialogueTreeAsset::GetStaticType(), AssetType::DialogueTree);
}

TEST_F(DialogueTreeAssetTest, NodePropertyAccess)
{
    auto tree = CreateSampleTree();
    auto* node = tree->FindNode(UUID(100));
    ASSERT_NE(node, nullptr);

    auto it = node->Properties.find("speaker");
    ASSERT_NE(it, node->Properties.end());
    EXPECT_EQ(std::get<std::string>(it->second), "Guard");

    auto textIt = node->Properties.find("text");
    ASSERT_NE(textIt, node->Properties.end());
    EXPECT_EQ(std::get<std::string>(textIt->second), "Halt!");
}

// ============================================================================
// DialogueState / DialogueStateComponent data structure tests
// ============================================================================

// Retired: DialogueStateComponentTest::DefaultValues,
// DialogueComponentTest::DefaultValues, DialogueComponentTest::TriggerOnceFlag.
// All three pinned design choices in the component headers rather than
// invariants. See docs/testing.md section 4.1. The Copy/Assignment
// tests below are kept because they pin the explicit non-trivial
// copy/assign operators that DialogueComponent declares specifically to
// exclude m_HasTriggered from being copied -- that IS a real contract.

TEST(DialogueComponentTest, CopyDoesNotCopyHasTriggered)
{
    DialogueComponent original;
    original.m_DialogueTree = 42;
    original.m_AutoTrigger = true;
    original.m_TriggerRadius = 5.0f;
    original.m_TriggerOnce = false;
    original.m_HasTriggered = true;

    DialogueComponent copy(original);
    EXPECT_EQ(static_cast<u64>(copy.m_DialogueTree), 42u);
    EXPECT_TRUE(copy.m_AutoTrigger);
    EXPECT_FLOAT_EQ(copy.m_TriggerRadius, 5.0f);
    EXPECT_FALSE(copy.m_TriggerOnce);
    EXPECT_FALSE(copy.m_HasTriggered); // runtime-only, not copied
}

TEST(DialogueComponentTest, AssignmentDoesNotCopyHasTriggered)
{
    DialogueComponent original;
    original.m_DialogueTree = 99;
    original.m_AutoTrigger = true;
    original.m_TriggerRadius = 7.0f;
    original.m_TriggerOnce = true;
    original.m_HasTriggered = true;

    DialogueComponent assigned;
    assigned.m_HasTriggered = true; // set before assignment
    assigned = original;
    EXPECT_EQ(static_cast<u64>(assigned.m_DialogueTree), 99u);
    EXPECT_TRUE(assigned.m_AutoTrigger);
    EXPECT_FLOAT_EQ(assigned.m_TriggerRadius, 7.0f);
    EXPECT_TRUE(assigned.m_TriggerOnce);
    EXPECT_FALSE(assigned.m_HasTriggered); // runtime-only, not copied via assignment
}

// DialogueChoiceTest::ChoiceDataStructure and
// DialogueConnectionTest::ConnectionDataStructure retired -- both were
// pure POD field-assignment smoke (a = "x"; EXPECT_EQ(a, "x")). See
// docs/testing.md section 4.1.

// ============================================================================
// DialogueSystem integration tests
// NOTE: These test cases require Scene + AssetManager infrastructure.
//       They are modeled after the plan's specifications but guarded behind
//       a compile-time flag until the test harness supports full engine init.
//       See the commented-out Rendering/CommandDispatchTest.cpp for precedent.
// ============================================================================
// Future test cases when integration test harness is available:
// - StartDialogueSetsDisplayingState
// - AdvanceDialogueMovesToNextNode
// - SelectChoiceFollowsBranch
// - ConditionNodeBranchesOnTrue
// - ConditionNodeBranchesOnFalse
// - ActionNodeInvokesCallback
// - EndDialogueRemovesState
// - TypewriterProgressAdvances
// - TriggerOncePreventsDuplicateStart
