#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Test the Enhanced Event System: Flags, Event Routing, and Core Functionality
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Audio/SoundGraph/Events.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"

using namespace OloEngine::Audio::SoundGraph;

TEST(EventSystemTest, BasicFlagOperations)
{
	Flag flag;
	
	// Initially clean
	EXPECT_FALSE(flag.IsDirty());
	EXPECT_FALSE(flag.CheckAndResetIfDirty());
	
	// Set dirty
	flag.SetDirty();
	EXPECT_TRUE(flag.IsDirty());
	
	// Check and reset atomically
	EXPECT_TRUE(flag.CheckAndResetIfDirty());
	EXPECT_FALSE(flag.IsDirty());
	EXPECT_FALSE(flag.CheckAndResetIfDirty());
}

TEST(EventSystemTest, FlagManagerOperations)
{
	FlagManager manager;
	bool callbackExecuted = false;
	
	// Add flag with callback
	manager.AddFlag("test", [&callbackExecuted]() { callbackExecuted = true; });
	
	// Initially clean
	EXPECT_FALSE(manager.IsFlagSet("test"));
	
	// Set flag and verify callback
	manager.SetFlag("test");
	EXPECT_TRUE(manager.IsFlagSet("test"));
	EXPECT_TRUE(callbackExecuted);
	
	// Clear flag
	manager.ClearFlag("test");
	EXPECT_FALSE(manager.IsFlagSet("test"));
}

TEST(EventSystemTest, EventConnectionAndTriggering)
{
	// Create a mock node processor for testing
	class MockNode : public NodeProcessor 
	{
	public:
		void Process(f32**, f32**, u32) override {}
		void Initialize(f64, u32) override {}
		
		OloEngine::Identifier GetTypeID() const override 
		{ 
			return OloEngine::Identifier("MockNode"); 
		}
		
		const char* GetDisplayName() const override { return "Mock Node"; }
	};
	
	MockNode sourceNode;
	MockNode destNode;
	
	f32 receivedValue = 0.0f;
	bool eventReceived = false;
	
	// Create events
	auto outputEvent = sourceNode.AddOutputEvent<f32>(OloEngine::Identifier("output"), "Output");
	auto inputEvent = destNode.AddInputEvent<f32>(OloEngine::Identifier("input"), "Input", 
		[&](f32 value) { 
			receivedValue = value;
			eventReceived = true;
		});
	
	// Connect events
	EventUtils::ConnectEvents(outputEvent, inputEvent);
	EXPECT_EQ(outputEvent->GetConnectionCount(), 1);
	EXPECT_TRUE(outputEvent->IsConnectedTo(inputEvent));
	
	// Trigger event
	(*outputEvent)(42.0f);
	EXPECT_TRUE(eventReceived);
	EXPECT_EQ(receivedValue, 42.0f);
}

TEST(EventSystemTest, EventUtilitiesAndForwarding)
{
	Flag testFlag;
	f32 setValue = 0.0f;
	
	// Test flag trigger utility
	auto flagTrigger = EventUtils::CreateFlagTrigger(testFlag);
	flagTrigger(42.0f);
	EXPECT_TRUE(testFlag.IsDirty());
	testFlag.CheckAndResetIfDirty();
	
	// Test value setter utility
	auto valueSetter = EventUtils::CreateValueSetter(setValue, testFlag);
	valueSetter(123.45f);
	EXPECT_EQ(setValue, 123.45f);
	EXPECT_TRUE(testFlag.IsDirty());
}

TEST(EventSystemTest, ParameterSystemIntegration)
{
	// Create a simple test node
	class TestNode : public NodeProcessor 
	{
	public:
		TestNode()
		{
			// Add some parameters
			AddParameter<f32>(OloEngine::Identifier("gain"), "Gain", 1.0f);
			AddParameter<i32>(OloEngine::Identifier("mode"), "Mode", 0);
			
			// Add an event that sets a flag
			m_TriggerEvent = AddInputEvent<f32>(OloEngine::Identifier("trigger"), "Trigger",
				[this](f32) { m_TriggerFlag.SetDirty(); });
		}
		
		void Process(f32**, f32**, u32) override 
		{
			// Process the trigger flag
			if (m_TriggerFlag.CheckAndResetIfDirty())
			{
				m_TriggerCount++;
			}
		}
		
		void Initialize(f64, u32) override {}
		
		OloEngine::Identifier GetTypeID() const override 
		{ 
			return OloEngine::Identifier("TestNode"); 
		}
		
		const char* GetDisplayName() const override { return "Test Node"; }
		
		int GetTriggerCount() const { return m_TriggerCount; }
		
	private:
		Flag m_TriggerFlag;
		int m_TriggerCount = 0;
		std::shared_ptr<InputEvent> m_TriggerEvent;
	};
	
	TestNode node;
	
	// Test parameter access
	EXPECT_EQ(node.GetParameterValue<f32>(OloEngine::Identifier("gain")), 1.0f);
	EXPECT_EQ(node.GetParameterValue<i32>(OloEngine::Identifier("mode")), 0);
	
	// Test parameter modification
	node.SetParameterValue(OloEngine::Identifier("gain"), 2.5f);
	EXPECT_EQ(node.GetParameterValue<f32>(OloEngine::Identifier("gain")), 2.5f);
	
	// Test event triggering
	auto triggerEvent = node.GetInputEvent(OloEngine::Identifier("trigger"));
	EXPECT_TRUE(triggerEvent != nullptr);
	
	// Initially no triggers
	EXPECT_EQ(node.GetTriggerCount(), 0);
	
	// Trigger and process
	(*triggerEvent)(1.0f);
	node.Process(nullptr, nullptr, 0);
	EXPECT_EQ(node.GetTriggerCount(), 1);
	
	// Trigger again
	(*triggerEvent)(1.0f);
	node.Process(nullptr, nullptr, 0);
	EXPECT_EQ(node.GetTriggerCount(), 2);
}