#include <gtest/gtest.h>
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSerializer.h"
#include "OloEngine/Core/Log.h"
#include <fstream>
#include <filesystem>

using namespace OloEngine;
using namespace OloEngine::Audio::SoundGraph;
using SoundGraphSerializerType = OloEngine::Audio::SoundGraph::SoundGraphSerializer;

// Test SoundGraphAsset and SoundGraphSerializer functionality
class SoundGraphSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize logging if needed
        if (!Log::GetCoreLogger())
        {
            Log::Init();
        }
        
        // Create test directory
        testDir = std::filesystem::temp_directory_path() / "OloEngine_SoundGraph_Tests";
        std::filesystem::create_directories(testDir);
    }
    
    void TearDown() override
    {
        // Clean up test files
        if (std::filesystem::exists(testDir))
        {
            std::filesystem::remove_all(testDir);
        }
    }
    
    std::filesystem::path testDir;
};

TEST_F(SoundGraphSerializationTest, SoundGraphAssetCreation)
{
    // Test creating a SoundGraphAsset
    SoundGraphAsset asset;
    asset.Name = "Test Sound Graph";
    asset.Description = "A test sound graph";
    // Note: asset ID/handle is managed by AssetManager, cannot be set directly
    
    EXPECT_FALSE(asset.Name.empty());
    EXPECT_EQ(asset.Name, "Test Sound Graph");
    EXPECT_FALSE(asset.IsValid()); // Should be invalid with no nodes
}

TEST_F(SoundGraphSerializationTest, SoundGraphAssetWithNodes)
{
    // Test creating a SoundGraphAsset with nodes
    SoundGraphAsset asset;
    asset.Name = "Test Graph with Nodes";
    // Note: asset ID/handle is managed by AssetManager
    
    // Add a test node
    SoundGraphNodeData node;
    node.ID = UUID();
    node.Name = "Wave Player 1";
    node.Type = "WavePlayer";
    node.Properties["Volume"] = "0.8";
    node.Properties["Pitch"] = "1.0";
    node.PosX = 100.0f;
    node.PosY = 200.0f;
    
    asset.AddNode(node);
    
    EXPECT_EQ(asset.Nodes.size(), 1);
    EXPECT_TRUE(asset.HasNode(node.ID));
    EXPECT_TRUE(asset.IsValid()); // Should be valid with nodes
    
    // Test node retrieval
    const SoundGraphNodeData* retrievedNode = asset.GetNode(node.ID);
    ASSERT_NE(retrievedNode, nullptr);
    EXPECT_EQ(retrievedNode->Name, "Wave Player 1");
    EXPECT_EQ(retrievedNode->Type, "WavePlayer");
}

TEST_F(SoundGraphSerializationTest, BasicSerialization)
{
    // Test basic serialization functionality
    SoundGraphAsset originalAsset;
    originalAsset.Name = "Serialization Test Graph";
    originalAsset.Description = "Test graph for serialization";
    // Note: asset ID/handle is managed by AssetManager
    
    // Add a node
    SoundGraphNodeData node;
    node.ID = UUID();
    node.Name = "Test Node";
    node.Type = "WavePlayer";
    node.Properties["Volume"] = "0.5";
    node.Properties["TestProperty"] = "TestValue";
    
    originalAsset.AddNode(node);
    
    // Serialize to string
    std::string yamlString = SoundGraphSerializerType::SerializeToString(originalAsset);
    EXPECT_FALSE(yamlString.empty());
    EXPECT_NE(yamlString.find("SoundGraph"), std::string::npos);
    EXPECT_NE(yamlString.find("Serialization Test Graph"), std::string::npos);
    EXPECT_NE(yamlString.find("WavePlayer"), std::string::npos);
}

TEST_F(SoundGraphSerializationTest, SerializationRoundTrip)
{
    // Test complete serialization round trip
    SoundGraphAsset originalAsset;
    originalAsset.Name = "Round Trip Test";
    originalAsset.Description = "Testing serialization round trip";
    // Note: asset ID/handle is managed by AssetManager
    
    // Add nodes
    SoundGraphNodeData node1;
    node1.ID = UUID();
    node1.Name = "Wave Player 1";
    node1.Type = "WavePlayer";
    node1.Properties["Volume"] = "0.7";
    node1.Properties["AudioFile"] = "test.wav";
    node1.PosX = 50.0f;
    node1.PosY = 100.0f;
    
    SoundGraphNodeData node2;
    node2.ID = UUID();
    node2.Name = "Mixer";
    node2.Type = "Mixer";
    node2.Properties["Channels"] = "2";
    node2.PosX = 200.0f;
    node2.PosY = 150.0f;
    
    originalAsset.AddNode(node1);
    originalAsset.AddNode(node2);
    
    // Add connection
    SoundGraphConnection connection;
    connection.SourceNodeID = node1.ID;
    connection.SourceEndpoint = "Output";
    connection.TargetNodeID = node2.ID;
    connection.TargetEndpoint = "Input1";
    connection.IsEvent = false;
    
    originalAsset.AddConnection(connection);
    
    // Test file round trip
    auto testFile = testDir / "roundtrip_test.yaml";
    bool serializeSuccess = SoundGraphSerializerType::Serialize(originalAsset, testFile);
    EXPECT_TRUE(serializeSuccess);
    EXPECT_TRUE(std::filesystem::exists(testFile));
    
    // Deserialize back
    SoundGraphAsset deserializedAsset;
    bool deserializeSuccess = SoundGraphSerializerType::Deserialize(deserializedAsset, testFile);
    EXPECT_TRUE(deserializeSuccess);
    
    // Verify deserialized data
    EXPECT_EQ(deserializedAsset.Name, originalAsset.Name);
    EXPECT_EQ(deserializedAsset.Description, originalAsset.Description);
    EXPECT_EQ(deserializedAsset.Nodes.size(), originalAsset.Nodes.size());
    EXPECT_EQ(deserializedAsset.Connections.size(), originalAsset.Connections.size());
    
    // Verify node properties
    if (!deserializedAsset.Nodes.empty())
    {
        const auto& deserializedNode = deserializedAsset.Nodes[0];
        EXPECT_EQ(deserializedNode.Name, "Wave Player 1");
        EXPECT_EQ(deserializedNode.Type, "WavePlayer");
        
        auto volumeIt = deserializedNode.Properties.find("Volume");
        ASSERT_NE(volumeIt, deserializedNode.Properties.end());
        EXPECT_EQ(volumeIt->second, "0.7");
    }
    
    // Verify connection
    if (!deserializedAsset.Connections.empty())
    {
        const auto& deserializedConnection = deserializedAsset.Connections[0];
        EXPECT_EQ(deserializedConnection.SourceEndpoint, "Output");
        EXPECT_EQ(deserializedConnection.TargetEndpoint, "Input1");
        EXPECT_FALSE(deserializedConnection.IsEvent);
    }
}