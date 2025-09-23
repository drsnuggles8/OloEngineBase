#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/Nodes/SineNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/AddNode.h"

using namespace OloEngine;
using namespace OloEngine::Audio::SoundGraph;

//===========================================
// Graph Routing Tests
//===========================================

class GraphRoutingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_SoundGraph = CreateScope<SoundGraph>();
        m_SoundGraph->Initialize(48000.0, 512);

        // Create test nodes
        m_SineNode1 = CreateScope<SineNode>();
        m_SineNode2 = CreateScope<SineNode>();
        m_AddNode = CreateScope<AddNodeF32>();

        m_SineNode1->Initialize(48000.0, 512);
        m_SineNode2->Initialize(48000.0, 512);
        m_AddNode->Initialize(48000.0, 512);

        // Set up test frequencies
        m_SineNode1->SetParameterValue(OLO_IDENTIFIER("Frequency"), 440.0f);
        m_SineNode2->SetParameterValue(OLO_IDENTIFIER("Frequency"), 880.0f);
    }

    Scope<SoundGraph> m_SoundGraph;
    Scope<SineNode> m_SineNode1;
    Scope<SineNode> m_SineNode2;
    Scope<AddNodeF32> m_AddNode;
};

TEST_F(GraphRoutingTest, AddValueConnection)
{
    // Add nodes to graph (note: this won't work until NodeProcessor supports IDs)
    // For now, test the connection methods directly

    // Test value connection between nodes
    bool success = m_SoundGraph->AddValueConnection(
        UUID(), "Output",     // Source: SineNode1 output
        UUID(), "Value1"      // Target: AddNode input
    );

    // Since FindNodeByID is not implemented, this should fail gracefully
    EXPECT_FALSE(success);
}

TEST_F(GraphRoutingTest, AddEventConnection)
{
    // Test event connection between nodes
    bool success = m_SoundGraph->AddEventConnection(
        UUID(), "TriggerOut", // Source: node output event
        UUID(), "TriggerIn"   // Target: node input event
    );

    // Since FindNodeByID is not implemented, this should fail gracefully
    EXPECT_FALSE(success);
}

TEST_F(GraphRoutingTest, AddInputValueRoute)
{
    // Test routing graph input to node parameter
    bool success = m_SoundGraph->AddInputValueRoute(
        "MasterVolume",       // Graph input parameter
        UUID(),              // Target node ID
        "Volume"             // Target parameter
    );

    // Since FindNodeByID is not implemented, this should fail gracefully
    EXPECT_FALSE(success);
}

TEST_F(GraphRoutingTest, AddInputEventRoute)
{
    // Test routing graph input event to node event
    bool success = m_SoundGraph->AddInputEventRoute(
        "Play",              // Graph input event
        UUID(),              // Target node ID
        "Trigger"            // Target event
    );

    // Since FindNodeByID is not implemented, this should fail gracefully
    EXPECT_FALSE(success);
}

TEST_F(GraphRoutingTest, AddOutputValueRoute)
{
    // Test routing node parameter to graph output
    bool success = m_SoundGraph->AddOutputValueRoute(
        UUID(),              // Source node ID
        "Output",            // Source parameter
        "MasterOutput"       // Graph output parameter
    );

    // Since FindNodeByID is not implemented, this should fail gracefully
    EXPECT_FALSE(success);
}

TEST_F(GraphRoutingTest, AddOutputEventRoute)
{
    // Test routing node event to graph output event
    bool success = m_SoundGraph->AddOutputEventRoute(
        UUID(),              // Source node ID
        "OnFinished",        // Source event
        "GraphFinished"      // Graph output event
    );

    // Since FindNodeByID is not implemented, this should fail gracefully
    EXPECT_FALSE(success);
}

TEST_F(GraphRoutingTest, AddRoute)
{
    // Test graph-level event routing (input event to input event)
    bool success = m_SoundGraph->AddRoute(
        "Play",              // Source event name
        "Start"              // Target event name
    );

    // This should succeed as it creates graph-level events
    EXPECT_TRUE(success);
}

TEST_F(GraphRoutingTest, AddEventRoute)
{
    // Test graph-level output event routing (output event to output event)
    bool success = m_SoundGraph->AddEventRoute(
        "Finished",          // Source event name
        "Complete"           // Target event name
    );

    // This should succeed as it creates graph-level events
    EXPECT_TRUE(success);
}

//===========================================
// Graph Event Management Tests
//===========================================

class GraphEventManagementTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_SoundGraph = CreateScope<SoundGraph>();
        m_SoundGraph->Initialize(48000.0, 512);
    }

    Scope<SoundGraph> m_SoundGraph;
};

TEST_F(GraphEventManagementTest, GetOrCreateGraphInputEvent)
{
    // Test that we can create and retrieve graph input events
    // We can't directly test GetOrCreateGraphInputEvent since it's private,
    // but we can test the functionality through AddRoute
    
    bool success1 = m_SoundGraph->AddRoute("TestInput1", "TestInput2");
    EXPECT_TRUE(success1);
    
    bool success2 = m_SoundGraph->AddRoute("TestInput1", "TestInput3");
    EXPECT_TRUE(success2);
    
    // Verify that the same input event can be used multiple times
    EXPECT_TRUE(success1 && success2);
}

TEST_F(GraphEventManagementTest, GetOrCreateGraphOutputEvent)
{
    // Test that we can create and retrieve graph output events
    bool success1 = m_SoundGraph->AddEventRoute("TestOutput1", "TestOutput2");
    EXPECT_TRUE(success1);
    
    bool success2 = m_SoundGraph->AddEventRoute("TestOutput1", "TestOutput3");
    EXPECT_TRUE(success2);
    
    // Verify that the same output event can be used multiple times
    EXPECT_TRUE(success1 && success2);
}

TEST_F(GraphEventManagementTest, GraphEventTriggering)
{
    // Test that graph events can be triggered and processed
    m_SoundGraph->TriggerGraphEvent("TestEvent", 1.0f);
    
    // Get pending events
    auto events = m_SoundGraph->GetPendingEvents();
    
    // Should have at least one event
    EXPECT_FALSE(events.empty());
    
    if (!events.empty())
    {
        auto event = events.front();
        EXPECT_EQ(event.EventName, "TestEvent");
        EXPECT_FLOAT_EQ(event.Value, 1.0f);
    }
}

//===========================================
// Connection Utility Tests
//===========================================

class ConnectionUtilityTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_SoundGraph = CreateScope<SoundGraph>();
        m_SoundGraph->Initialize(48000.0, 512);
    }

    Scope<SoundGraph> m_SoundGraph;
};

TEST_F(ConnectionUtilityTest, EventConnectionUtility)
{
    // Test the internal event connection utility
    // Create mock events for testing
    auto sourceEvent = m_SoundGraph->AddOutputEvent<f32>(OLO_IDENTIFIER("TestSource"), "TestSource");
    auto targetEvent = m_SoundGraph->AddInputEvent<f32>(OLO_IDENTIFIER("TestTarget"), "TestTarget",
        [](f32 value) { /* Test callback */ });
    
    EXPECT_TRUE(sourceEvent != nullptr);
    EXPECT_TRUE(targetEvent != nullptr);
    
    // Verify events exist in the graph
    auto retrievedSource = m_SoundGraph->GetOutputEvent(OLO_IDENTIFIER("TestSource"));
    auto retrievedTarget = m_SoundGraph->GetInputEvent(OLO_IDENTIFIER("TestTarget"));
    
    EXPECT_EQ(sourceEvent, retrievedSource);
    EXPECT_EQ(targetEvent, retrievedTarget);
}

TEST_F(ConnectionUtilityTest, ParameterConnectionUtility)
{
    // Test parameter creation and retrieval
    m_SoundGraph->AddParameter<f32>(OLO_IDENTIFIER("TestParam1"), "TestParam1", 1.0f);
    m_SoundGraph->AddParameter<i32>(OLO_IDENTIFIER("TestParam2"), "TestParam2", 42);
    m_SoundGraph->AddParameter<bool>(OLO_IDENTIFIER("TestParam3"), "TestParam3", true);
    
    // Verify parameters were created
    EXPECT_TRUE(m_SoundGraph->HasParameter(OLO_IDENTIFIER("TestParam1")));
    EXPECT_TRUE(m_SoundGraph->HasParameter(OLO_IDENTIFIER("TestParam2")));
    EXPECT_TRUE(m_SoundGraph->HasParameter(OLO_IDENTIFIER("TestParam3")));
    
    // Verify parameter values
    EXPECT_FLOAT_EQ(m_SoundGraph->GetParameterValue<f32>(OLO_IDENTIFIER("TestParam1")), 1.0f);
    EXPECT_EQ(m_SoundGraph->GetParameterValue<i32>(OLO_IDENTIFIER("TestParam2")), 42);
    EXPECT_EQ(m_SoundGraph->GetParameterValue<bool>(OLO_IDENTIFIER("TestParam3")), true);
}

//===========================================
// Integration Tests
//===========================================

class GraphRoutingIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_SoundGraph = CreateScope<SoundGraph>();
        m_SoundGraph->Initialize(48000.0, 512);
    }

    Scope<SoundGraph> m_SoundGraph;
};

TEST_F(GraphRoutingIntegrationTest, ComplexEventRouting)
{
    // Test complex event routing patterns
    
    // Create a chain of event routes
    EXPECT_TRUE(m_SoundGraph->AddRoute("InputTrigger", "ProcessTrigger"));
    EXPECT_TRUE(m_SoundGraph->AddRoute("ProcessTrigger", "OutputTrigger"));
    
    // Create output event routes
    EXPECT_TRUE(m_SoundGraph->AddEventRoute("OutputEvent1", "OutputEvent2"));
    EXPECT_TRUE(m_SoundGraph->AddEventRoute("OutputEvent2", "FinalOutput"));
    
    // All routes should be created successfully
    // The actual event flow would need to be tested with proper node connections
}

TEST_F(GraphRoutingIntegrationTest, MixedParameterAndEventRouting)
{
    // Test mixing parameter and event routing
    
    // Create graph input parameters
    m_SoundGraph->AddParameter<f32>(OLO_IDENTIFIER("MasterVolume"), "MasterVolume", 1.0f);
    m_SoundGraph->AddParameter<f32>(OLO_IDENTIFIER("MasterPitch"), "MasterPitch", 1.0f);
    
    // Create graph events
    bool eventRoute1 = m_SoundGraph->AddRoute("Play", "Start");
    bool eventRoute2 = m_SoundGraph->AddRoute("Stop", "Finish");
    
    EXPECT_TRUE(eventRoute1);
    EXPECT_TRUE(eventRoute2);
    
    // Verify parameters exist
    EXPECT_TRUE(m_SoundGraph->HasParameter(OLO_IDENTIFIER("MasterVolume")));
    EXPECT_TRUE(m_SoundGraph->HasParameter(OLO_IDENTIFIER("MasterPitch")));
}

TEST_F(GraphRoutingIntegrationTest, GraphPlaybackIntegration)
{
    // Test that routing doesn't interfere with basic graph playback
    
    // Set up some routing
    m_SoundGraph->AddRoute("Play", "Start");
    m_SoundGraph->AddEventRoute("Finished", "Complete");
    
    // Test basic playback functionality
    EXPECT_FALSE(m_SoundGraph->IsPlaying());
    
    m_SoundGraph->Play();
    EXPECT_TRUE(m_SoundGraph->IsPlaying());
    
    m_SoundGraph->Stop();
    EXPECT_FALSE(m_SoundGraph->IsPlaying());
}

TEST_F(GraphRoutingIntegrationTest, RoutingAPIConsistency)
{
    // Test that all routing methods have consistent behavior
    
    // Test with valid parameters - should succeed for graph-level routes
    EXPECT_TRUE(m_SoundGraph->AddRoute("Event1", "Event2"));
    EXPECT_TRUE(m_SoundGraph->AddEventRoute("Output1", "Output2"));
    
    // Test with node-based routes - should fail gracefully until node ID system is implemented
    EXPECT_FALSE(m_SoundGraph->AddValueConnection(UUID(), "Out", UUID(), "In"));
    EXPECT_FALSE(m_SoundGraph->AddEventConnection(UUID(), "OutEvent", UUID(), "InEvent"));
    EXPECT_FALSE(m_SoundGraph->AddInputValueRoute("GraphIn", UUID(), "NodeIn"));
    EXPECT_FALSE(m_SoundGraph->AddInputEventRoute("GraphInEvent", UUID(), "NodeInEvent"));
    EXPECT_FALSE(m_SoundGraph->AddOutputValueRoute(UUID(), "NodeOut", "GraphOut"));
    EXPECT_FALSE(m_SoundGraph->AddOutputEventRoute(UUID(), "NodeOutEvent", "GraphOutEvent"));
}