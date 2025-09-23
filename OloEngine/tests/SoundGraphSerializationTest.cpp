#include <gtest/gtest.h>
#include "OloEngine/Audio/SoundGraph/SoundGraphSerializer.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Core/Log.h"
#include <fstream>
#include <filesystem>

using namespace OloEngine::Audio::SoundGraph;

class SoundGraphSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize logging if needed
        if (!OloEngine::Log::GetCoreLogger())
        {
            OloEngine::Log::Init();
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

TEST_F(SoundGraphSerializationTest, BasicSerialization)
{
    // Create a simple SoundGraphAsset
    SoundGraphAsset asset;
    asset.Name = "Test Sound Graph";
    asset.ID = OloEngine::UUID();
    
    // Add a simple node
    SoundGraphAsset::NodeData wavePlayerNode;
    wavePlayerNode.ID = OloEngine::UUID();
    wavePlayerNode.Name = "Wave Player 1";
    wavePlayerNode.Type = "WavePlayer";
    wavePlayerNode.Properties["Volume"] = "0.8";
    wavePlayerNode.Properties["Pitch"] = "1.0";
    wavePlayerNode.Properties["Loop"] = "false";
    
    asset.Nodes.push_back(wavePlayerNode);
    
    // Test serialization to string
    std::string yamlString = SoundGraphSerializer::SerializeToString(asset);
    EXPECT_FALSE(yamlString.empty());
    EXPECT_NE(yamlString.find("SoundGraph"), std::string::npos);
    EXPECT_NE(yamlString.find("Test Sound Graph"), std::string::npos);
    EXPECT_NE(yamlString.find("WavePlayer"), std::string::npos);
    
    // Test serialization to file
    auto testFile = testDir / "test_graph.yaml";
    SoundGraphSerializer::Serialize(asset, testFile);
    
    EXPECT_TRUE(std::filesystem::exists(testFile));
    EXPECT_GT(std::filesystem::file_size(testFile), 0);
}

TEST_F(SoundGraphSerializationTest, SerializationRoundTrip)
{
    // Create a SoundGraphAsset with multiple nodes
    SoundGraphAsset originalAsset;
    originalAsset.Name = "Complex Test Graph";
    originalAsset.ID = OloEngine::UUID();
    
    // Add multiple nodes
    SoundGraphAsset::NodeData node1;
    node1.ID = OloEngine::UUID();
    node1.Name = "Wave Player 1";
    node1.Type = "WavePlayer";
    node1.Properties["Volume"] = "0.5";
    node1.Properties["AudioFilePath"] = "test.wav";
    
    SoundGraphAsset::NodeData node2;
    node2.ID = OloEngine::UUID();
    node2.Name = "Wave Player 2";
    node2.Type = "WavePlayer";
    node2.Properties["Volume"] = "0.7";
    node2.Properties["Pitch"] = "1.5";
    
    originalAsset.Nodes.push_back(node1);
    originalAsset.Nodes.push_back(node2);
    
    // Add a connection
    Connection connection;
    connection.SourceNodeID = OloEngine::Identifier(static_cast<u64>(node1.ID));
    connection.SourceEndpoint = "Output";
    connection.TargetNodeID = OloEngine::Identifier(static_cast<u64>(node2.ID));
    connection.TargetEndpoint = "Input";
    connection.IsEvent = false;
    
    originalAsset.Connections.push_back(connection);
    
    // Serialize to string
    std::string yamlString = SoundGraphSerializer::SerializeToString(originalAsset);
    EXPECT_FALSE(yamlString.empty());
    
    // Deserialize back
    SoundGraphAsset deserializedAsset;
    bool success = SoundGraphSerializer::DeserializeFromString(deserializedAsset, yamlString);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(deserializedAsset.Name, originalAsset.Name);
    EXPECT_EQ(deserializedAsset.Nodes.size(), originalAsset.Nodes.size());
    EXPECT_EQ(deserializedAsset.Connections.size(), originalAsset.Connections.size());
    
    // Check node properties
    if (!deserializedAsset.Nodes.empty())
    {
        const auto& deserializedNode = deserializedAsset.Nodes[0];
        EXPECT_EQ(deserializedNode.Name, "Wave Player 1");
        EXPECT_EQ(deserializedNode.Type, "WavePlayer");
        
        auto volumeIt = deserializedNode.Properties.find("Volume");
        EXPECT_NE(volumeIt, deserializedNode.Properties.end());
        EXPECT_EQ(volumeIt->second, "0.5");
    }
    
    // Check connection
    if (!deserializedAsset.Connections.empty())
    {
        const auto& deserializedConnection = deserializedAsset.Connections[0];
        EXPECT_EQ(deserializedConnection.SourceEndpoint, "Output");
        EXPECT_EQ(deserializedConnection.TargetEndpoint, "Input");
        EXPECT_FALSE(deserializedConnection.IsEvent);
    }
}

TEST_F(SoundGraphSerializationTest, FileRoundTrip)
{
    // Create asset
    SoundGraphAsset originalAsset;
    originalAsset.Name = "File Test Graph";
    originalAsset.ID = OloEngine::UUID();
    
    SoundGraphAsset::NodeData node;
    node.ID = OloEngine::UUID();
    node.Name = "Test Node";
    node.Type = "WavePlayer";
    node.Properties["TestProperty"] = "TestValue";
    
    originalAsset.Nodes.push_back(node);
    
    // Write to file
    auto testFile = testDir / "roundtrip_test.yaml";
    SoundGraphSerializer::Serialize(originalAsset, testFile);
    
    // Read back from file
    SoundGraphAsset deserializedAsset;
    bool success = SoundGraphSerializer::Deserialize(deserializedAsset, testFile);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(deserializedAsset.Name, originalAsset.Name);
    EXPECT_EQ(deserializedAsset.Nodes.size(), 1);
    
    if (!deserializedAsset.Nodes.empty())
    {
        const auto& deserializedNode = deserializedAsset.Nodes[0];
        EXPECT_EQ(deserializedNode.Name, "Test Node");
        EXPECT_EQ(deserializedNode.Type, "WavePlayer");
        
        auto propIt = deserializedNode.Properties.find("TestProperty");
        EXPECT_NE(propIt, deserializedNode.Properties.end());
        EXPECT_EQ(propIt->second, "TestValue");
    }
}

TEST_F(SoundGraphSerializationTest, ErrorHandling)
{
    // Test deserialization of missing file
    SoundGraphAsset asset;
    auto nonExistentFile = testDir / "does_not_exist.yaml";
    bool success = SoundGraphSerializer::Deserialize(asset, nonExistentFile);
    EXPECT_FALSE(success);
    
    // Test deserialization of YAML with missing required fields
    std::string invalidYaml = R"(
SoundGraph:
  Name: "Test"
  Nodes:
    - Name: "Node1"
      # Missing Type and ID
      Properties:
        Volume: "0.5"
)";
    
    success = SoundGraphSerializer::DeserializeFromString(asset, invalidYaml);
    EXPECT_FALSE(success);
    
    // Test empty string
    success = SoundGraphSerializer::DeserializeFromString(asset, "");
    EXPECT_FALSE(success);
    
    // Test YAML without SoundGraph root node
    std::string noRootYaml = R"(
NotASoundGraph:
  Name: "Test"
)";
    success = SoundGraphSerializer::DeserializeFromString(asset, noRootYaml);
    EXPECT_FALSE(success);
    
    // Test malformed YAML (safer version)
    std::string malformedYaml = "invalid:\n  - yaml";
    success = SoundGraphSerializer::DeserializeFromString(asset, malformedYaml);
    EXPECT_FALSE(success);
}

TEST_F(SoundGraphSerializationTest, EmptyGraph)
{
    // Test serialization of empty graph
    SoundGraphAsset emptyAsset;
    emptyAsset.Name = "Empty Graph";
    emptyAsset.ID = OloEngine::UUID();
    
    std::string yamlString = SoundGraphSerializer::SerializeToString(emptyAsset);
    EXPECT_FALSE(yamlString.empty());
    
    // Test round-trip
    SoundGraphAsset deserializedAsset;
    bool success = SoundGraphSerializer::DeserializeFromString(deserializedAsset, yamlString);
    
    EXPECT_TRUE(success);
    EXPECT_EQ(deserializedAsset.Name, "Empty Graph");
    EXPECT_TRUE(deserializedAsset.Nodes.empty());
    EXPECT_TRUE(deserializedAsset.Connections.empty());
}