#include <gtest/gtest.h>
#include "OloEngine/Scene/Scene.h" // Required: AssetSerializer.h uses Ref<Scene> inline
#include "OloEngine/Dialogue/DialogueTreeAsset.h"
#include "OloEngine/Dialogue/DialogueTreeSerializer.h"

#include <algorithm>

using namespace OloEngine;

class DialogueTreeSerializationTest : public ::testing::Test
{
  protected:
    Ref<DialogueTreeAsset> CreateSampleTree()
    {
        auto asset = Ref<DialogueTreeAsset>::Create();

        DialogueNodeData rootNode;
        rootNode.ID = UUID(1001);
        rootNode.Type = "dialogue";
        rootNode.Name = "Greeting";
        rootNode.Properties["speaker"] = std::string("Guard");
        rootNode.Properties["text"] = std::string("Halt! Who goes there?");
        rootNode.EditorPosition = { 100.0f, 200.0f };

        DialogueNodeData choiceNode;
        choiceNode.ID = UUID(1002);
        choiceNode.Type = "choice";
        choiceNode.Name = "PlayerResponse";
        choiceNode.EditorPosition = { 300.0f, 200.0f };

        DialogueNodeData responseNode;
        responseNode.ID = UUID(1003);
        responseNode.Type = "dialogue";
        responseNode.Name = "GuardReply";
        responseNode.Properties["speaker"] = std::string("Guard");
        responseNode.Properties["text"] = std::string("Move along.");
        responseNode.EditorPosition = { 500.0f, 200.0f };

        asset->GetNodesWritable().push_back(std::move(rootNode));
        asset->GetNodesWritable().push_back(std::move(choiceNode));
        asset->GetNodesWritable().push_back(std::move(responseNode));

        DialogueConnection conn1;
        conn1.SourceNodeID = UUID(1001);
        conn1.TargetNodeID = UUID(1002);
        conn1.SourcePort = "output";
        conn1.TargetPort = "input";

        DialogueConnection conn2;
        conn2.SourceNodeID = UUID(1002);
        conn2.TargetNodeID = UUID(1003);
        conn2.SourcePort = "I'm a friend.";
        conn2.TargetPort = "input";

        asset->GetConnectionsWritable().push_back(std::move(conn1));
        asset->GetConnectionsWritable().push_back(std::move(conn2));

        asset->SetRootNodeID(UUID(1001));
        asset->RebuildNodeIndex();
        return asset;
    }

    DialogueTreeSerializer serializer;
};

TEST_F(DialogueTreeSerializationTest, SerializeAndDeserializeRoundTrip)
{
    auto original = CreateSampleTree();

    std::string yaml = serializer.TestSerializeToYAML(original);
    ASSERT_FALSE(yaml.empty());

    auto deserialized = Ref<DialogueTreeAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));

    // Verify nodes
    EXPECT_EQ(deserialized->GetNodes().size(), 3u);
    EXPECT_EQ(static_cast<u64>(deserialized->GetRootNodeID()), static_cast<u64>(original->GetRootNodeID()));

    auto* root = deserialized->FindNode(UUID(1001));
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->Type, "dialogue");
    EXPECT_EQ(root->Name, "Greeting");

    auto it = root->Properties.find("speaker");
    ASSERT_NE(it, root->Properties.end());
    EXPECT_EQ(std::get<std::string>(it->second), "Guard");
    EXPECT_FLOAT_EQ(root->EditorPosition.x, 100.0f);
    EXPECT_FLOAT_EQ(root->EditorPosition.y, 200.0f);

    // Verify response node text
    auto* response = deserialized->FindNode(UUID(1003));
    ASSERT_NE(response, nullptr);
    auto textIt = response->Properties.find("text");
    ASSERT_NE(textIt, response->Properties.end());
    EXPECT_EQ(std::get<std::string>(textIt->second), "Move along.");

    // Verify connections (order-independent)
    ASSERT_EQ(deserialized->GetConnections().size(), 2u);
    const auto& conns = deserialized->GetConnections();
    bool hasOutputConn = std::ranges::any_of(conns, [](const auto& c)
                                             { return c.SourcePort == "output" && c.TargetPort == "input"; });
    bool hasChoiceConn = std::ranges::any_of(conns, [](const auto& c)
                                             { return c.SourcePort == "I'm a friend." && c.TargetPort == "input"; });
    EXPECT_TRUE(hasOutputConn);
    EXPECT_TRUE(hasChoiceConn);
}

TEST_F(DialogueTreeSerializationTest, DeserializeRejectsMissingRootNode)
{
    std::string yaml = R"(
DialogueTree:
  Nodes:
    - ID: 1001
      Type: dialogue
      Name: Test
      EditorPosition: [0, 0]
  Connections: []
)";

    auto asset = Ref<DialogueTreeAsset>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML(yaml, asset));
}

TEST_F(DialogueTreeSerializationTest, DeserializeRejectsDanglingConnection)
{
    std::string yaml = R"(
DialogueTree:
  RootNodeID: 1001
  Nodes:
    - ID: 1001
      Type: dialogue
      Name: Test
      EditorPosition: [0, 0]
  Connections:
    - SourceNodeID: 1001
      TargetNodeID: 9999
      SourcePort: output
      TargetPort: input
)";

    auto asset = Ref<DialogueTreeAsset>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML(yaml, asset));
}

TEST_F(DialogueTreeSerializationTest, DeserializeRejectsDuplicateNodeID)
{
    std::string yaml = R"(
DialogueTree:
  RootNodeID: 1001
  Nodes:
    - ID: 1001
      Type: dialogue
      Name: First
      EditorPosition: [0, 0]
    - ID: 1001
      Type: dialogue
      Name: Duplicate
      EditorPosition: [100, 0]
  Connections: []
)";

    auto asset = Ref<DialogueTreeAsset>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML(yaml, asset));
}

TEST_F(DialogueTreeSerializationTest, SingleNodeNoConnections)
{
    auto asset = Ref<DialogueTreeAsset>::Create();

    DialogueNodeData rootNode;
    rootNode.ID = UUID(5000);
    rootNode.Type = "dialogue";
    rootNode.Name = "LoneNode";
    rootNode.Properties["text"] = std::string("Hello.");
    rootNode.EditorPosition = { 0.0f, 0.0f };

    asset->GetNodesWritable().push_back(std::move(rootNode));
    asset->SetRootNodeID(UUID(5000));
    asset->RebuildNodeIndex();

    std::string yaml = serializer.TestSerializeToYAML(asset);
    ASSERT_FALSE(yaml.empty());

    auto deserialized = Ref<DialogueTreeAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));

    EXPECT_EQ(deserialized->GetNodes().size(), 1u);
    EXPECT_TRUE(deserialized->GetConnections().empty());
}

TEST_F(DialogueTreeSerializationTest, RoundTripPreservesAllPropertyTypes)
{
    auto asset = Ref<DialogueTreeAsset>::Create();

    DialogueNodeData node;
    node.ID = UUID(7000);
    node.Type = "action";
    node.Name = "TypeTest";
    node.Properties["strProp"] = std::string("hello");
    node.Properties["boolProp"] = true;
    node.Properties["intProp"] = i32(42);
    node.Properties["floatProp"] = f32(3.14f);
    node.EditorPosition = { 10.0f, 20.0f };

    asset->GetNodesWritable().push_back(std::move(node));
    asset->SetRootNodeID(UUID(7000));
    asset->RebuildNodeIndex();

    std::string yaml = serializer.TestSerializeToYAML(asset);
    ASSERT_FALSE(yaml.empty());

    auto deserialized = Ref<DialogueTreeAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));

    auto* n = deserialized->FindNode(UUID(7000));
    ASSERT_NE(n, nullptr);

    auto strIt = n->Properties.find("strProp");
    ASSERT_NE(strIt, n->Properties.end());
    EXPECT_EQ(std::get<std::string>(strIt->second), "hello");

    auto boolIt = n->Properties.find("boolProp");
    ASSERT_NE(boolIt, n->Properties.end());
    EXPECT_EQ(std::get<bool>(boolIt->second), true);

    auto intIt = n->Properties.find("intProp");
    ASSERT_NE(intIt, n->Properties.end());
    EXPECT_EQ(std::get<i32>(intIt->second), 42);

    auto floatIt = n->Properties.find("floatProp");
    ASSERT_NE(floatIt, n->Properties.end());
    EXPECT_FLOAT_EQ(std::get<f32>(floatIt->second), 3.14f);
}
