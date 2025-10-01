#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Test SoundGraph functionality
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPrototype.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSerializer.h"
#include "OloEngine/Audio/Buffer/CircularBuffer.h"
#include "OloEngine/Audio/SampleBufferOperations.h"

using namespace OloEngine;
using namespace OloEngine::Audio;
using namespace OloEngine::Audio::SoundGraph;

class SoundGraphBasicTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Initialize logging if needed
        if (!Log::GetCoreLogger())
        {
            Log::Init();
        }
    }
};

TEST_F(SoundGraphBasicTest, CanCreateUUID)
{
    // Test basic UUID creation which is used in SoundGraph
    UUID testId1 = UUID();
    UUID testId2 = UUID();
    
    // UUIDs should be unique
    EXPECT_NE(testId1, testId2);
    
    // UUID should be convertible to u64
    u64 id1_val = static_cast<u64>(testId1);
    u64 id2_val = static_cast<u64>(testId2);
    EXPECT_NE(id1_val, id2_val);
}

TEST_F(SoundGraphBasicTest, SoundGraphAssetBasicOperations)
{
    // Test basic SoundGraphAsset functionality
    SoundGraphAsset asset;
    asset.m_Name = "Test Graph";
    asset.m_Description = "Testing basic operations";
    
    // Initially should be invalid (no nodes)
    EXPECT_FALSE(asset.IsValid());
    EXPECT_EQ(asset.m_Nodes.size(), 0);
    
    // Add a test node
    SoundGraphNodeData node;
    node.ID = UUID();
    node.Name = "Test Node";
    node.Type = "TestType";
    node.Properties["param1"] = "value1";
    node.PosX = 100.0f;
    node.PosY = 200.0f;
    
    EXPECT_TRUE(asset.AddNode(node));
    
    // Now should be valid (has nodes) and contain our node
    EXPECT_TRUE(asset.IsValid());
    EXPECT_EQ(asset.m_Nodes.size(), 1);
    EXPECT_TRUE(asset.HasNode(node.ID));
    
    // Test node retrieval
    const SoundGraphNodeData* retrieved = asset.GetNode(node.ID);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->Name, "Test Node");
    EXPECT_EQ(retrieved->Type, "TestType");
    EXPECT_EQ(retrieved->Properties.at("param1"), "value1");
}

TEST_F(SoundGraphBasicTest, SoundGraphConnections)
{
    SoundGraphAsset asset;
    asset.m_Name = "Connection Test";
    
    // Add two nodes
    SoundGraphNodeData node1, node2;
    node1.ID = UUID();
    node1.Name = "Source Node";
    node1.Type = "Generator";
    
    node2.ID = UUID();
    node2.Name = "Target Node";
    node2.Type = "Effect";
    
    EXPECT_TRUE(asset.AddNode(node1));
    EXPECT_TRUE(asset.AddNode(node2));
    
    // Add connection
    SoundGraphConnection connection;
    connection.SourceNodeID = node1.ID;
    connection.SourceEndpoint = "output";
    connection.TargetNodeID = node2.ID;
    connection.TargetEndpoint = "input";
    connection.IsEvent = false;
    
    EXPECT_TRUE(asset.AddConnection(connection));
    
    EXPECT_EQ(asset.m_Connections.size(), 1);
    EXPECT_TRUE(asset.IsValid()); // Should be valid with nodes and connections
    
    const auto& conn = asset.m_Connections[0];
    EXPECT_EQ(conn.SourceNodeID, node1.ID);
    EXPECT_EQ(conn.TargetNodeID, node2.ID);
    EXPECT_EQ(conn.SourceEndpoint, "output");
    EXPECT_EQ(conn.TargetEndpoint, "input");
    EXPECT_FALSE(conn.IsEvent);
}

TEST_F(SoundGraphBasicTest, SoundGraphRemoveConnection)
{
    SoundGraphAsset asset;
    asset.m_Name = "Remove Connection Test";
    
    // Add two nodes
    SoundGraphNodeData node1, node2;
    node1.ID = UUID();
    node1.Name = "Source Node";
    node1.Type = "Generator";
    
    node2.ID = UUID();
    node2.Name = "Target Node";
    node2.Type = "Effect";
    
    EXPECT_TRUE(asset.AddNode(node1));
    EXPECT_TRUE(asset.AddNode(node2));
    
    // Add two connections with same endpoints but different IsEvent flags
    SoundGraphConnection dataConnection;
    dataConnection.SourceNodeID = node1.ID;
    dataConnection.SourceEndpoint = "output";
    dataConnection.TargetNodeID = node2.ID;
    dataConnection.TargetEndpoint = "input";
    dataConnection.IsEvent = false; // Data connection
    
    SoundGraphConnection eventConnection;
    eventConnection.SourceNodeID = node1.ID;
    eventConnection.SourceEndpoint = "output";
    eventConnection.TargetNodeID = node2.ID;
    eventConnection.TargetEndpoint = "input";
    eventConnection.IsEvent = true; // Event connection
    
    EXPECT_TRUE(asset.AddConnection(dataConnection));
    EXPECT_TRUE(asset.AddConnection(eventConnection));
    
    EXPECT_EQ(asset.m_Connections.size(), 2);
    
    // Test removing the data connection should leave the event connection
    bool removed = asset.RemoveConnection(node1.ID, "output", node2.ID, "input", false);
    EXPECT_TRUE(removed);
    EXPECT_EQ(asset.m_Connections.size(), 1);
    EXPECT_TRUE(asset.m_Connections[0].IsEvent); // Only event connection should remain
    
    // Test removing the event connection should leave no connections
    removed = asset.RemoveConnection(node1.ID, "output", node2.ID, "input", true);
    EXPECT_TRUE(removed);
    EXPECT_EQ(asset.m_Connections.size(), 0);
    
    // Test removing non-existent connection should return false
    removed = asset.RemoveConnection(node1.ID, "output", node2.ID, "input", false);
    EXPECT_FALSE(removed);
    EXPECT_EQ(asset.m_Connections.size(), 0);
}

TEST_F(SoundGraphBasicTest, CircularBufferSingleChannel)
{
    // Test the enhanced CircularBuffer with single channel
    MonoCircularBuffer<f32, 64> buffer;
    
    EXPECT_EQ(buffer.Available(), 0);
    EXPECT_EQ(buffer.GetNumChannels(), 1);
    EXPECT_EQ(buffer.GetFrameCapacity(), 64);
    
    // Push some samples
    buffer.Push(1.0f);
    buffer.Push(2.0f);
    buffer.Push(3.0f);
    
    EXPECT_EQ(buffer.Available(), 3);
    
    // Get samples back
    EXPECT_FLOAT_EQ(buffer.Get(), 1.0f);
    EXPECT_FLOAT_EQ(buffer.Get(), 2.0f);
    EXPECT_EQ(buffer.Available(), 1);
    
    EXPECT_FLOAT_EQ(buffer.Get(), 3.0f);
    EXPECT_EQ(buffer.Available(), 0);
}

TEST_F(SoundGraphBasicTest, CircularBufferMultiChannel)
{
    // Test the enhanced CircularBuffer with multiple channels
    StereoCircularBuffer<f32, 128> buffer;
    
    EXPECT_EQ(buffer.Available(), 0);
    EXPECT_EQ(buffer.GetNumChannels(), 2);
    EXPECT_EQ(buffer.GetFrameCapacity(), 64); // 128 / 2 channels
    
    // Push stereo frames
    f32 frame1[2] = { 1.0f, -1.0f };
    f32 frame2[2] = { 2.0f, -2.0f };
    
    buffer.PushFrame(frame1);
    buffer.PushFrame(frame2);
    
    EXPECT_EQ(buffer.Available(), 2);
    
    // Get frames back
    f32 outFrame[2];
    buffer.GetFrame(outFrame);
    EXPECT_FLOAT_EQ(outFrame[0], 1.0f);
    EXPECT_FLOAT_EQ(outFrame[1], -1.0f);
    
    buffer.GetFrame(outFrame);
    EXPECT_FLOAT_EQ(outFrame[0], 2.0f);
    EXPECT_FLOAT_EQ(outFrame[1], -2.0f);
    
    EXPECT_EQ(buffer.Available(), 0);
}

TEST_F(SoundGraphBasicTest, SampleBufferOperationsInterleaving)
{
    // Test interleaving and deinterleaving operations
    const u32 numChannels = 2;
    const u32 numSamples = 4;
    
    // Create test data: interleaved stereo
    f32 interleavedData[8] = { 1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f };
    
    // Deinterleave using raw pointers
    f32 leftChannel[4], rightChannel[4];
    f32* deinterleavedChannels[2] = { leftChannel, rightChannel };
    
    SampleBufferOperations::Deinterleave(deinterleavedChannels, interleavedData, numChannels, numSamples);
    
    // Check deinterleaved data
    EXPECT_FLOAT_EQ(leftChannel[0], 1.0f);
    EXPECT_FLOAT_EQ(leftChannel[1], 2.0f);
    EXPECT_FLOAT_EQ(leftChannel[2], 3.0f);
    EXPECT_FLOAT_EQ(leftChannel[3], 4.0f);
    
    EXPECT_FLOAT_EQ(rightChannel[0], -1.0f);
    EXPECT_FLOAT_EQ(rightChannel[1], -2.0f);
    EXPECT_FLOAT_EQ(rightChannel[2], -3.0f);
    EXPECT_FLOAT_EQ(rightChannel[3], -4.0f);
    
    // Interleave back
    f32 reinterleavedData[8] = { 0 };
    const f32* sourceChannels[2] = { leftChannel, rightChannel };
    
    SampleBufferOperations::Interleave(reinterleavedData, sourceChannels, numChannels, numSamples);
    
    // Check that we got back the original data
    for (u32 i = 0; i < 8; ++i)
    {
        EXPECT_FLOAT_EQ(reinterleavedData[i], interleavedData[i]);
    }
}

TEST_F(SoundGraphBasicTest, SampleBufferOperationsGain)
{
    f32 data[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    
    // Apply constant gain
    SampleBufferOperations::ApplyGainRamp(data, 4, 1, 0.5f, 0.5f);
    
    EXPECT_FLOAT_EQ(data[0], 0.5f);
    EXPECT_FLOAT_EQ(data[1], 1.0f);
    EXPECT_FLOAT_EQ(data[2], 1.5f);
    EXPECT_FLOAT_EQ(data[3], 2.0f);
}

TEST_F(SoundGraphBasicTest, SoundGraphValidation)
{
    SoundGraphAsset asset;
    
    // Empty asset should be invalid
    EXPECT_FALSE(asset.IsValid());
    
    auto errors = asset.GetValidationErrors();
    EXPECT_GT(errors.size(), 0);
    EXPECT_NE(std::find_if(errors.begin(), errors.end(), 
        [](const std::string& error) { return error.find("no nodes") != std::string::npos; }), 
        errors.end());
    
    // Add a valid node
    SoundGraphNodeData node;
    node.ID = UUID();
    node.Name = "Valid Node";
    node.Type = "TestType";
    
    EXPECT_TRUE(asset.AddNode(node));
    EXPECT_TRUE(asset.IsValid());
    
    // Try to add invalid connection (referencing non-existent node)
    // This should be rejected, keeping the asset valid
    SoundGraphConnection badConnection;
    badConnection.SourceNodeID = node.ID;
    badConnection.TargetNodeID = UUID(); // Non-existent node
    badConnection.SourceEndpoint = "out";
    badConnection.TargetEndpoint = "in";
    
    sizet connectionCountBefore = asset.m_Connections.size();
    EXPECT_FALSE(asset.AddConnection(badConnection));
    
    // Connection should not be added, asset should remain valid
    EXPECT_EQ(asset.m_Connections.size(), connectionCountBefore);
    EXPECT_TRUE(asset.IsValid());
}
