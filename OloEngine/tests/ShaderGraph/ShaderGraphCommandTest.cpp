#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderGraph/ShaderGraphCommand.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraph.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"

using namespace OloEngine;

class ShaderGraphCommandTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Graph = std::make_unique<ShaderGraph>();
        m_History = ShaderGraphCommandHistory();
    }

    std::unique_ptr<ShaderGraph> m_Graph;
    ShaderGraphCommandHistory m_History;
};

// ─── AddNodeCommand ──────────────────────────────────────────

TEST_F(ShaderGraphCommandTest, AddNodeExecuteAddsNode)
{
    auto cmd = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::FloatParameter, glm::vec2(10.0f, 20.0f));
    m_History.Execute(std::move(cmd), *m_Graph);

    EXPECT_EQ(m_Graph->GetNodes().Num(), 1);
    EXPECT_EQ(m_Graph->GetNodes().GetHead()->GetValue()->TypeName, ShaderGraphNodeTypes::FloatParameter);
    EXPECT_EQ(m_Graph->GetNodes().GetHead()->GetValue()->EditorPosition, glm::vec2(10.0f, 20.0f));
}

TEST_F(ShaderGraphCommandTest, AddNodeUndoRemovesNode)
{
    auto cmd = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::FloatParameter, glm::vec2(0.0f));
    m_History.Execute(std::move(cmd), *m_Graph);
    ASSERT_EQ(m_Graph->GetNodes().Num(), 1);

    m_History.Undo(*m_Graph);
    EXPECT_EQ(m_Graph->GetNodes().Num(), 0);
}

TEST_F(ShaderGraphCommandTest, AddNodeRedoRestoresNode)
{
    auto cmd = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::FloatParameter, glm::vec2(5.0f, 5.0f));
    m_History.Execute(std::move(cmd), *m_Graph);
    m_History.Undo(*m_Graph);
    ASSERT_EQ(m_Graph->GetNodes().Num(), 0);

    m_History.Redo(*m_Graph);
    EXPECT_EQ(m_Graph->GetNodes().Num(), 1);
    EXPECT_EQ(m_Graph->GetNodes().GetHead()->GetValue()->EditorPosition, glm::vec2(5.0f, 5.0f));
}

// ─── RemoveNodeCommand ──────────────────────────────────────

TEST_F(ShaderGraphCommandTest, RemoveNodeUndoRestoresNodeAndLinks)
{
    // Build a small graph: Float -> PBROutput
    auto floatNode = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto pbrNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
    UUID floatID = floatNode->ID;
    UUID outPinID = floatNode->Outputs[0].ID;
    UUID metallicPinID = pbrNode->FindPinByName("Metallic", ShaderGraphPinDirection::Input)->ID;

    m_Graph->AddNode(std::move(floatNode));
    m_Graph->AddNode(std::move(pbrNode));
    m_Graph->AddLink(outPinID, metallicPinID);

    ASSERT_EQ(m_Graph->GetLinks().Num(), 1);

    // Remove the float node via command
    auto cmd = CreateScope<RemoveNodeCommand>(floatID);
    m_History.Execute(std::move(cmd), *m_Graph);

    EXPECT_EQ(m_Graph->GetNodes().Num(), 1); // Only PBR remains
    EXPECT_EQ(m_Graph->GetLinks().Num(), 0); // Link removed

    // Undo
    m_History.Undo(*m_Graph);
    EXPECT_EQ(m_Graph->GetNodes().Num(), 2);
    EXPECT_NE(m_Graph->FindNode(floatID), nullptr);
    // Link should be restored
    EXPECT_EQ(m_Graph->GetLinks().Num(), 1);
}

// ─── AddLinkCommand ─────────────────────────────────────────

TEST_F(ShaderGraphCommandTest, AddLinkUndoRemovesLink)
{
    auto floatNode = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto pbrNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
    UUID outPinID = floatNode->Outputs[0].ID;
    UUID metallicPinID = pbrNode->FindPinByName("Metallic", ShaderGraphPinDirection::Input)->ID;

    m_Graph->AddNode(std::move(floatNode));
    m_Graph->AddNode(std::move(pbrNode));

    auto cmd = CreateScope<AddLinkCommand>(outPinID, metallicPinID);
    m_History.Execute(std::move(cmd), *m_Graph);
    ASSERT_EQ(m_Graph->GetLinks().Num(), 1);

    m_History.Undo(*m_Graph);
    EXPECT_EQ(m_Graph->GetLinks().Num(), 0);
}

// ─── RemoveLinkCommand ──────────────────────────────────────

TEST_F(ShaderGraphCommandTest, RemoveLinkUndoRestoresLink)
{
    auto floatNode = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto pbrNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
    UUID outPinID = floatNode->Outputs[0].ID;
    UUID metallicPinID = pbrNode->FindPinByName("Metallic", ShaderGraphPinDirection::Input)->ID;

    m_Graph->AddNode(std::move(floatNode));
    m_Graph->AddNode(std::move(pbrNode));
    auto* link = m_Graph->AddLink(outPinID, metallicPinID);
    ASSERT_NE(link, nullptr);
    UUID linkID = link->ID;

    auto cmd = CreateScope<RemoveLinkCommand>(linkID);
    m_History.Execute(std::move(cmd), *m_Graph);
    EXPECT_EQ(m_Graph->GetLinks().Num(), 0);

    m_History.Undo(*m_Graph);
    EXPECT_EQ(m_Graph->GetLinks().Num(), 1);
}

// ─── MoveNodeCommand ────────────────────────────────────────

TEST_F(ShaderGraphCommandTest, MoveNodeUndoRestoresPosition)
{
    auto node = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    UUID nodeID = node->ID;
    m_Graph->AddNode(std::move(node));

    glm::vec2 oldPos(0.0f, 0.0f);
    glm::vec2 newPos(100.0f, 200.0f);

    auto cmd = CreateScope<MoveNodeCommand>(nodeID, oldPos, newPos);
    m_History.Execute(std::move(cmd), *m_Graph);
    EXPECT_EQ(m_Graph->FindNode(nodeID)->EditorPosition, newPos);

    m_History.Undo(*m_Graph);
    EXPECT_EQ(m_Graph->FindNode(nodeID)->EditorPosition, oldPos);
}

// ─── CommandHistory ─────────────────────────────────────────

TEST_F(ShaderGraphCommandTest, HistoryCanUndoCanRedo)
{
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.CanRedo());

    auto cmd = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::FloatParameter, glm::vec2(0.0f));
    m_History.Execute(std::move(cmd), *m_Graph);

    EXPECT_TRUE(m_History.CanUndo());
    EXPECT_FALSE(m_History.CanRedo());

    m_History.Undo(*m_Graph);
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_TRUE(m_History.CanRedo());
}

TEST_F(ShaderGraphCommandTest, NewCommandClearsRedoStack)
{
    auto cmd1 = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::FloatParameter, glm::vec2(0.0f));
    m_History.Execute(std::move(cmd1), *m_Graph);
    m_History.Undo(*m_Graph);
    ASSERT_TRUE(m_History.CanRedo());

    // Execute a new command — redo should be cleared
    auto cmd2 = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::PBROutput, glm::vec2(100.0f));
    m_History.Execute(std::move(cmd2), *m_Graph);
    EXPECT_FALSE(m_History.CanRedo());
}

TEST_F(ShaderGraphCommandTest, HistoryClearResetsStacks)
{
    auto cmd = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::FloatParameter, glm::vec2(0.0f));
    m_History.Execute(std::move(cmd), *m_Graph);
    ASSERT_TRUE(m_History.CanUndo());

    m_History.Clear();
    EXPECT_FALSE(m_History.CanUndo());
    EXPECT_FALSE(m_History.CanRedo());
}

TEST_F(ShaderGraphCommandTest, MultipleUndoRedoCycles)
{
    // Add 3 nodes
    for (int i = 0; i < 3; ++i)
    {
        auto cmd = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::FloatParameter,
                                               glm::vec2(static_cast<f32>(i) * 100.0f, 0.0f));
        m_History.Execute(std::move(cmd), *m_Graph);
    }
    EXPECT_EQ(m_Graph->GetNodes().Num(), 3);

    // Undo all
    m_History.Undo(*m_Graph);
    m_History.Undo(*m_Graph);
    m_History.Undo(*m_Graph);
    EXPECT_EQ(m_Graph->GetNodes().Num(), 0);

    // Redo all
    m_History.Redo(*m_Graph);
    m_History.Redo(*m_Graph);
    m_History.Redo(*m_Graph);
    EXPECT_EQ(m_Graph->GetNodes().Num(), 3);
}

TEST_F(ShaderGraphCommandTest, OwnedHistoryPreservesOrderAcrossManyUndoRedoOperations)
{
    TArray<UUID> ids;
    for (i32 i = 0; i < 130; ++i)
    {
        auto command = CreateScope<AddNodeCommand>(ShaderGraphNodeTypes::FloatParameter, glm::vec2(static_cast<f32>(i), 0.0f));
        auto* const commandPtr = command.get();
        m_History.Execute(std::move(command), *m_Graph);
        ids.Add(commandPtr->GetNodeID());
    }
    for (i32 i = ids.Num() - 1; i >= 0; --i)
    {
        m_History.Undo(*m_Graph);
        EXPECT_EQ(m_Graph->FindNode(ids[i]), nullptr);
        EXPECT_EQ(m_Graph->GetNodes().Num(), i);
    }
    for (i32 i = 0; i < ids.Num(); ++i)
    {
        m_History.Redo(*m_Graph);
        const auto* node = m_Graph->FindNode(ids[i]);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->EditorPosition.x, static_cast<f32>(i));
    }
    EXPECT_FALSE(m_History.CanRedo());
    EXPECT_EQ(m_Graph->GetNodes().Num(), ids.Num());
}
