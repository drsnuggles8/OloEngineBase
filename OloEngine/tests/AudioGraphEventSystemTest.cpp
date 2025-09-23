#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Events.h"
#include "OloEngine/Core/Identifier.h"

using namespace OloEngine::Audio::SoundGraph;

//==============================================================================
/// Simple test node for event system testing
class EventTestNode : public NodeProcessor
{
public:
    EventTestNode() : m_TriggerCount(0)
    {
        InitializeEndpoints();
    }

    void Process(f32** inputs, f32** outputs, u32 numSamples) override
    {
        // Fill outputs with silence - this is just for testing events
        if (outputs[0]) 
        {
            for (u32 i = 0; i < numSamples; ++i)
            {
                outputs[0][i] = 0.0f;
                if (outputs[1]) outputs[1][i] = 0.0f;
            }
        }
    }

    void Initialize(f64 sampleRate, u32 maxBufferSize) override
    {
        m_SampleRate = sampleRate;
    }

    OloEngine::Identifier GetTypeID() const override { return OLO_IDENTIFIER("EventTest"); }
    const char* GetDisplayName() const override { return "Event Test"; }

    // Test accessors
    u32 GetTriggerCount() const { return m_TriggerCount; }
    f32 GetLastValue() const { return GetParameterValue<f32>(OLO_IDENTIFIER("LastValue"), 0.0f); }

private:
    u32 m_TriggerCount;

    void InitializeEndpoints()
    {
        // Input event - when triggered, increments counter and forwards event
        AddInputEvent<f32>(OLO_IDENTIFIER("TriggerIn"), "TriggerIn", 
            [this](f32 value) { OnTriggerReceived(value); });

        // Output event - forwards received triggers
        AddOutputEvent<f32>(OLO_IDENTIFIER("TriggerOut"), "TriggerOut");

        // Parameters for testing
        AddParameter<f32>(OLO_IDENTIFIER("Multiplier"), "Multiplier", 1.0f);
        AddParameter<f32>(OLO_IDENTIFIER("LastValue"), "LastValue", 0.0f);
    }

    void OnTriggerReceived(f32 value)
    {
        m_TriggerCount++;
        
        // Apply multiplier and store last value
        f32 multiplier = GetParameterValue<f32>(OLO_IDENTIFIER("Multiplier"), 1.0f);
        f32 result = value * multiplier;
        
        SetParameterValue(OLO_IDENTIFIER("LastValue"), result);
        
        // Forward the event to output
        TriggerOutputEvent("TriggerOut", result);
    }
};

//==============================================================================
/// Event System Tests
class AudioGraphEventSystemTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        sourceNode = std::make_unique<EventTestNode>();
        targetNode = std::make_unique<EventTestNode>();
        
        sourceNode->Initialize(48000.0, 512);
        targetNode->Initialize(48000.0, 512);
    }

    void TearDown() override
    {
        sourceNode.reset();
        targetNode.reset();
    }

    std::unique_ptr<EventTestNode> sourceNode;
    std::unique_ptr<EventTestNode> targetNode;
};

//==============================================================================
/// Test basic event triggering
TEST_F(AudioGraphEventSystemTest, BasicEventTriggering)
{
    // Set up a multiplier to test parameter interaction
    sourceNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 2.0f);
    
    // Trigger the input event
    auto inputEvent = sourceNode->GetInputEvent(OLO_IDENTIFIER("TriggerIn"));
    ASSERT_NE(inputEvent, nullptr);
    
    // Trigger with value 5.0
    (*inputEvent)(5.0f);
    
    // Verify the node received the trigger
    EXPECT_EQ(sourceNode->GetTriggerCount(), 1u);
    EXPECT_FLOAT_EQ(sourceNode->GetLastValue(), 10.0f); // 5.0 * 2.0 = 10.0
}

//==============================================================================
/// Test event connection between nodes
TEST_F(AudioGraphEventSystemTest, EventConnection)
{
    // Connect source output to target input
    bool connected = sourceNode->ConnectTo("TriggerOut", targetNode.get(), "TriggerIn");
    EXPECT_TRUE(connected);
    
    // Set different multipliers to verify both nodes process independently
    sourceNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 2.0f);
    targetNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 3.0f);
    
    // Trigger the source node
    auto inputEvent = sourceNode->GetInputEvent(OLO_IDENTIFIER("TriggerIn"));
    ASSERT_NE(inputEvent, nullptr);
    
    (*inputEvent)(4.0f);
    
    // Verify both nodes were triggered
    EXPECT_EQ(sourceNode->GetTriggerCount(), 1u);
    EXPECT_EQ(targetNode->GetTriggerCount(), 1u);
    
    // Verify values were processed correctly
    EXPECT_FLOAT_EQ(sourceNode->GetLastValue(), 8.0f);  // 4.0 * 2.0 = 8.0
    EXPECT_FLOAT_EQ(targetNode->GetLastValue(), 24.0f); // 8.0 * 3.0 = 24.0
}

//==============================================================================
/// Test multiple triggers
TEST_F(AudioGraphEventSystemTest, MultipleTriggers)
{
    auto inputEvent = sourceNode->GetInputEvent(OLO_IDENTIFIER("TriggerIn"));
    ASSERT_NE(inputEvent, nullptr);
    
    // Trigger multiple times
    (*inputEvent)(1.0f);
    (*inputEvent)(2.0f);
    (*inputEvent)(3.0f);
    
    // Verify counter incremented correctly
    EXPECT_EQ(sourceNode->GetTriggerCount(), 3u);
    EXPECT_FLOAT_EQ(sourceNode->GetLastValue(), 3.0f); // Last value should be 3.0
}

//==============================================================================
/// Test parameter system
TEST_F(AudioGraphEventSystemTest, ParameterSystem)
{
    // Test parameter setting and getting
    sourceNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 5.5f);
    f32 multiplier = sourceNode->GetParameterValue<f32>(OLO_IDENTIFIER("Multiplier"), 0.0f);
    EXPECT_FLOAT_EQ(multiplier, 5.5f);
    
    // Test parameter interaction with events
    auto inputEvent = sourceNode->GetInputEvent(OLO_IDENTIFIER("TriggerIn"));
    (*inputEvent)(2.0f);
    
    EXPECT_FLOAT_EQ(sourceNode->GetLastValue(), 11.0f); // 2.0 * 5.5 = 11.0
}

//==============================================================================
/// Test invalid connections
TEST_F(AudioGraphEventSystemTest, InvalidConnections)
{
    // Test connection to non-existent endpoint
    bool connected = sourceNode->ConnectTo("NonExistent", targetNode.get(), "TriggerIn");
    EXPECT_FALSE(connected);
    
    // Test connection from valid to non-existent endpoint
    connected = sourceNode->ConnectTo("TriggerOut", targetNode.get(), "NonExistent");
    EXPECT_FALSE(connected);
}

//==============================================================================
/// Test parameter connection system
TEST_F(AudioGraphEventSystemTest, ParameterConnections)
{
    // Set up source node with an output parameter
    sourceNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 5.0f);
    
    // Connect source multiplier to target multiplier
    bool connected = sourceNode->ConnectTo("Multiplier", targetNode.get(), "Multiplier");
    EXPECT_TRUE(connected);
    
    // Process parameter connections to propagate values
    sourceNode->ProcessParameterConnections();
    
    // Verify the value was propagated
    f32 targetMultiplier = targetNode->GetParameterValue<f32>(OLO_IDENTIFIER("Multiplier"), 0.0f);
    EXPECT_FLOAT_EQ(targetMultiplier, 5.0f);
    
    // Change source value and propagate again
    sourceNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 10.0f);
    sourceNode->ProcessParameterConnections();
    
    // Verify new value was propagated
    targetMultiplier = targetNode->GetParameterValue<f32>(OLO_IDENTIFIER("Multiplier"), 0.0f);
    EXPECT_FLOAT_EQ(targetMultiplier, 10.0f);
}

//==============================================================================
/// Test parameter connection with audio processing
TEST_F(AudioGraphEventSystemTest, ParameterConnectionWithAudioProcessing)
{
    // Connect parameter from source to target
    bool connected = sourceNode->ConnectTo("Multiplier", targetNode.get(), "Multiplier");
    EXPECT_TRUE(connected);
    
    // Set source multiplier value
    sourceNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 3.0f);
    
    // Simulate audio processing (which should propagate parameters first)
    sourceNode->ProcessBeforeAudio();
    
    // Now trigger an event to test the combined system
    auto inputEvent = targetNode->GetInputEvent(OLO_IDENTIFIER("TriggerIn"));
    ASSERT_NE(inputEvent, nullptr);
    
    (*inputEvent)(2.0f);
    
    // Verify the target used the propagated multiplier value
    EXPECT_FLOAT_EQ(targetNode->GetLastValue(), 6.0f); // 2.0 * 3.0 = 6.0
}

//==============================================================================
/// Test parameter connection removal
TEST_F(AudioGraphEventSystemTest, ParameterConnectionRemoval)
{
    // Create a connection
    bool connected = sourceNode->ConnectTo("Multiplier", targetNode.get(), "Multiplier");
    EXPECT_TRUE(connected);
    
    // Verify connection exists
    EXPECT_GT(sourceNode->GetParameterConnections().size(), 0u);
    
    // Set and propagate value
    sourceNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 7.0f);
    sourceNode->ProcessParameterConnections();
    EXPECT_FLOAT_EQ(targetNode->GetParameterValue<f32>(OLO_IDENTIFIER("Multiplier"), 0.0f), 7.0f);
    
    // Remove the connection
    bool removed = sourceNode->RemoveParameterConnection("Multiplier", targetNode.get(), "Multiplier");
    EXPECT_TRUE(removed);
    
    // Verify connection no longer exists
    EXPECT_EQ(sourceNode->GetParameterConnections().size(), 0u);
    
    // Change source value and try to propagate
    sourceNode->SetParameterValue(OLO_IDENTIFIER("Multiplier"), 99.0f);
    sourceNode->ProcessParameterConnections();
    
    // Target should still have the old value (no propagation)
    EXPECT_FLOAT_EQ(targetNode->GetParameterValue<f32>(OLO_IDENTIFIER("Multiplier"), 0.0f), 7.0f);
}

//==============================================================================
/// Test parameter connection type validation
TEST_F(AudioGraphEventSystemTest, ParameterConnectionTypeValidation)
{
    // Try to connect to non-existent parameter
    bool connected = sourceNode->ConnectTo("NonExistent", targetNode.get(), "Multiplier");
    EXPECT_FALSE(connected);
    
    // Try to connect from valid to non-existent parameter
    connected = sourceNode->ConnectTo("Multiplier", targetNode.get(), "NonExistent");
    EXPECT_FALSE(connected);
    
    // Try to connect to null node
    connected = sourceNode->ConnectTo("Multiplier", nullptr, "Multiplier");
    EXPECT_FALSE(connected);
}