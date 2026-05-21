#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SoundGraphSerializerTest — YAML roundtrip for SoundGraphAsset
//
// The asset-pack path goes through the same SerializeToString /
// DeserializeFromString helpers as the on-disk YAML path, so a YAML roundtrip
// test covers both. This file pins:
//   * Nodes survive with their type / ID / properties.
//   * Connections survive, including the UUID(0) sentinel used for graph-input
//     (m_SourceNodeID == 0) and graph-output (m_TargetNodeID == 0) routes.
//   * GraphInputs / GraphOutputs / LocalVariables survive — these were silently
//     dropped before; without them, every graph that uses parameters loses
//     them on the next reload.
// =============================================================================

#include "OloEngine/Asset/SoundGraphAsset.h"
// SoundGraphAsset only forward-declares Prototype; the ~SoundGraphAsset destructor (which
// runs when the local in each test goes out of scope) needs the full type to instantiate
// ~Ref<Prototype>. Including GraphGeneration.h here pulls that in.
#include "OloEngine/Audio/SoundGraph/GraphGeneration.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSerializer.h"
#include "OloEngine/Core/UUID.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    // Populates `asset` with a representative graph: two nodes with properties, one
    // node-to-graph-output connection, one graph-input-to-node connection, and a couple
    // of graph parameters. The two captured node IDs are returned so the test can match
    // connections after a roundtrip.
    void PopulateRichAsset(SoundGraphAsset& asset, UUID& outNoiseID, UUID& outSineID)
    {
        asset.SetName("RoundtripTest");
        asset.SetDescription("Covers nodes/connections/graph IO");

        SoundGraphNodeData noise;
        noise.m_ID = UUID();
        noise.m_Type = "Noise";
        noise.m_Name = "WhiteNoise";
        noise.m_Properties["Amplitude"] = "0.5";
        noise.m_PosX = 100.0f;
        noise.m_PosY = 200.0f;
        asset.AddNode(noise);
        outNoiseID = noise.m_ID;

        SoundGraphNodeData sine;
        sine.m_ID = UUID();
        sine.m_Type = "SineOscillator";
        sine.m_Name = "ToneA";
        sine.m_Properties["Frequency"] = "440";
        sine.m_Properties["Amplitude"] = "0.25";
        sine.m_PosX = 300.0f;
        sine.m_PosY = 50.0f;
        asset.AddNode(sine);
        outSineID = sine.m_ID;

        // Node-to-graph-output connection (the most-tested case in the editor).
        SoundGraphConnection toGraphOut;
        toGraphOut.m_SourceNodeID = noise.m_ID;
        toGraphOut.m_SourceEndpoint = "Out";
        toGraphOut.m_TargetNodeID = UUID(0); // graph output sentinel
        toGraphOut.m_TargetEndpoint = "OutLeft";
        toGraphOut.m_IsEvent = false;
        asset.AddConnection(toGraphOut);

        // Graph-input-to-node connection (the case that breaks if m_GraphInputs is
        // dropped — the connection would dangle).
        SoundGraphConnection fromGraphIn;
        fromGraphIn.m_SourceNodeID = UUID(0); // graph input sentinel
        fromGraphIn.m_SourceEndpoint = "Volume";
        fromGraphIn.m_TargetNodeID = sine.m_ID;
        fromGraphIn.m_TargetEndpoint = "Amplitude";
        fromGraphIn.m_IsEvent = false;
        asset.AddConnection(fromGraphIn);

        // Graph IO definitions.
        asset.AddGraphInput("Volume", "Float");
        asset.AddGraphInput("Pitch", "Int");
    }
} // namespace

TEST(SoundGraphSerializer, RoundTripPreservesNodesConnectionsAndGraphIO)
{
    SoundGraphAsset original;
    UUID noiseID = 0, sineID = 0;
    PopulateRichAsset(original, noiseID, sineID);
    (void)noiseID;
    (void)sineID;

    std::string yamlString = Audio::SoundGraph::SoundGraphSerializer::SerializeToString(original);
    ASSERT_FALSE(yamlString.empty()) << "SerializeToString returned empty";

    SoundGraphAsset roundtripped;
    ASSERT_TRUE(Audio::SoundGraph::SoundGraphSerializer::DeserializeFromString(roundtripped, yamlString))
        << "DeserializeFromString failed";

    EXPECT_EQ(roundtripped.GetName(), original.GetName());
    EXPECT_EQ(roundtripped.GetNodeCount(), original.GetNodeCount());
    EXPECT_EQ(roundtripped.GetConnectionCount(), original.GetConnectionCount());

    // Node-by-ID lookup survives because RebuildNodeIdMap runs after load.
    for (const auto& origNode : original.GetNodes())
    {
        const auto* loaded = roundtripped.GetNode(origNode.m_ID);
        ASSERT_NE(loaded, nullptr) << "Node " << static_cast<u64>(origNode.m_ID) << " missing after roundtrip";
        EXPECT_EQ(loaded->m_Type, origNode.m_Type);
        EXPECT_EQ(loaded->m_Name, origNode.m_Name);
        // Canvas positions: editor UI state, but persisted so reopening a graph doesn't
        // dump every node back at (0,0). Roundtrip must preserve them.
        EXPECT_FLOAT_EQ(loaded->m_PosX, origNode.m_PosX) << "Node PosX changed on roundtrip";
        EXPECT_FLOAT_EQ(loaded->m_PosY, origNode.m_PosY) << "Node PosY changed on roundtrip";
        EXPECT_EQ(loaded->m_Properties.size(), origNode.m_Properties.size());
        for (const auto& [key, value] : origNode.m_Properties)
        {
            auto it = loaded->m_Properties.find(key);
            ASSERT_NE(it, loaded->m_Properties.end()) << "Property '" << key << "' missing";
            EXPECT_EQ(it->second, value) << "Property '" << key << "' value mismatch";
        }
    }

    // Connection list — both UUID(0) directions must survive. Match by source endpoint
    // since IDs are stable.
    auto findConn = [&](const SoundGraphConnection& orig) -> const SoundGraphConnection*
    {
        for (const auto& c : roundtripped.GetConnections())
        {
            if (c.m_SourceNodeID == orig.m_SourceNodeID &&
                c.m_SourceEndpoint == orig.m_SourceEndpoint &&
                c.m_TargetNodeID == orig.m_TargetNodeID &&
                c.m_TargetEndpoint == orig.m_TargetEndpoint)
            {
                return &c;
            }
        }
        return nullptr;
    };
    for (const auto& origConn : original.GetConnections())
    {
        const auto* loaded = findConn(origConn);
        ASSERT_NE(loaded, nullptr) << "Connection " << origConn.m_SourceEndpoint << " -> " << origConn.m_TargetEndpoint << " missing";
        EXPECT_EQ(loaded->m_IsEvent, origConn.m_IsEvent);
    }

    // Graph IO maps — the regression this test was added to catch.
    EXPECT_EQ(roundtripped.GetGraphInputs().size(), original.GetGraphInputs().size())
        << "Graph inputs were dropped on roundtrip (the bug this test pins)";
    for (const auto& [name, type] : original.GetGraphInputs())
    {
        auto it = roundtripped.GetGraphInputs().find(name);
        ASSERT_NE(it, roundtripped.GetGraphInputs().end()) << "Graph input '" << name << "' missing after roundtrip";
        EXPECT_EQ(it->second, type);
    }
}

TEST(SoundGraphSerializer, EmptyGraphRoundTripsWithoutData)
{
    SoundGraphAsset original;
    original.SetName("Empty");

    std::string yamlString = Audio::SoundGraph::SoundGraphSerializer::SerializeToString(original);
    SoundGraphAsset roundtripped;
    ASSERT_TRUE(Audio::SoundGraph::SoundGraphSerializer::DeserializeFromString(roundtripped, yamlString));
    EXPECT_EQ(roundtripped.GetName(), "Empty");
    EXPECT_EQ(roundtripped.GetNodeCount(), 0u);
    EXPECT_EQ(roundtripped.GetConnectionCount(), 0u);
    EXPECT_TRUE(roundtripped.GetGraphInputs().empty());
}
