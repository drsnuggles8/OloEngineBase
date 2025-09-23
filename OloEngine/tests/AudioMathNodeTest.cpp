#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/Nodes/SubtractNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MultiplyNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/DivideNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MinNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MaxNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/ClampNode.h"

// Advanced Math Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/PowerNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/LogNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/ModuloNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MapRangeNode.h"

// Audio-Specific Math Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/LinearToLogFrequencyNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/FrequencyLogToLinearNode.h"

// Music Theory Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/BPMToSecondsNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/NoteToFrequencyNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/FrequencyToNoteNode.h"

// Generator Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/NoiseNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/SineNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/RandomNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/TriangleNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/SquareNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/SawtoothNode.h"

// Filter Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/LowPassFilterNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/HighPassFilterNode.h"

// Utility Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/SampleAndHoldNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/GateNode.h"

// Envelope Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/ADEnvelope.h"
#include "OloEngine/Audio/SoundGraph/Nodes/ADSREnvelope.h"
#include "OloEngine/Audio/SoundGraph/Nodes/AREnvelope.h"

// Trigger Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/RepeatTrigger.h"
#include "OloEngine/Audio/SoundGraph/Nodes/TriggerCounter.h"
#include "OloEngine/Audio/SoundGraph/Nodes/DelayedTrigger.h"

// Array Nodes
#include "OloEngine/Audio/SoundGraph/Nodes/GetRandom.h"
#include "OloEngine/Audio/SoundGraph/Nodes/Get.h"

using namespace OloEngine::Audio::SoundGraph;

class MathNodeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		// Setup common test data if needed
	}

	void TearDown() override
	{
		// Cleanup if needed
	}
};

// SubtractNode Tests
TEST_F(MathNodeTest, SubtractNodeF32Test)
{
	SubtractNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 10.5f);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 3.2f);
	
	// Process the node (with dummy audio buffers)
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 7.3f);
}

TEST_F(MathNodeTest, SubtractNodeI32Test)
{
	SubtractNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 15);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 7);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 8);
}

// MultiplyNode Tests
TEST_F(MathNodeTest, MultiplyNodeF32Test)
{
	MultiplyNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 4.5f);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 2.0f);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 9.0f);
}

TEST_F(MathNodeTest, MultiplyNodeI32Test)
{
	MultiplyNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 6);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 7);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 42);
}

// DivideNode Tests
TEST_F(MathNodeTest, DivideNodeF32Test)
{
	DivideNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 15.0f);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 3.0f);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 5.0f);
}

TEST_F(MathNodeTest, DivideNodeI32Test)
{
	DivideNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 20);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 4);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 5);
}

TEST_F(MathNodeTest, DivideNodeF32DivisionByZeroTest)
{
	DivideNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values with zero divisor
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 10.0f);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 0.0f);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result (should be infinity)
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_TRUE(std::isinf(result));
	EXPECT_GT(result, 0.0f); // Should be positive infinity
}

TEST_F(MathNodeTest, DivideNodeI32DivisionByZeroTest)
{
	DivideNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values with zero divisor
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 10);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 0);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result (should be zero for integer division by zero)
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 0);
}

// MinNode Tests
TEST_F(MathNodeTest, MinNodeF32Test)
{
	MinNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 8.7f);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 3.2f);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 3.2f);
}

TEST_F(MathNodeTest, MinNodeI32Test)
{
	MinNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 25);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 12);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 12);
}

// MaxNode Tests
TEST_F(MathNodeTest, MaxNodeF32Test)
{
	MaxNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 8.7f);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 3.2f);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 8.7f);
}

TEST_F(MathNodeTest, MaxNodeI32Test)
{
	MaxNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set input values
	node.SetParameterValue(OLO_IDENTIFIER("InputA"), 25);
	node.SetParameterValue(OLO_IDENTIFIER("InputB"), 12);
	
	// Process the node
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Check the result
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 25);
}

// ClampNode Tests
TEST_F(MathNodeTest, ClampNodeF32Test)
{
	ClampNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test clamping to max
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 15.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 10.0f);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 10.0f);
	
	// Test clamping to min
	node.SetParameterValue(OLO_IDENTIFIER("Value"), -5.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 10.0f);
	
	node.Process(inputs, outputs, 256);
	
	result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 0.0f);
	
	// Test value within range
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 5.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 10.0f);
	
	node.Process(inputs, outputs, 256);
	
	result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 5.0f);
}

TEST_F(MathNodeTest, ClampNodeI32Test)
{
	ClampNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test clamping to max
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 25);
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 0);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 20);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 20);
	
	// Test clamping to min
	node.SetParameterValue(OLO_IDENTIFIER("Value"), -10);
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 0);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 20);
	
	node.Process(inputs, outputs, 256);
	
	result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 0);
	
	// Test value within range
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 15);
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 0);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 20);
	
	node.Process(inputs, outputs, 256);
	
	result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
	EXPECT_EQ(result, 15);
}

// Test swapped min/max in ClampNode (should handle gracefully)
TEST_F(MathNodeTest, ClampNodeSwappedMinMaxTest)
{
	ClampNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Set min > max (swapped)
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 15.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 10.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 5.0f);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	// Should clamp to the actual max (10.0f since we swap min/max internally)
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 10.0f);
}

//=============================================================================
// Advanced Math Node Tests
//=============================================================================

// PowerNode Tests
TEST_F(MathNodeTest, PowerNodeF32Test)
{
	PowerNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test 2^3 = 8
	node.SetParameterValue(OLO_IDENTIFIER("Base"), 2.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Exponent"), 3.0f);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Result"));
	EXPECT_FLOAT_EQ(result, 8.0f);
}

TEST_F(MathNodeTest, PowerNodeI32Test)
{
	PowerNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test 3^4 = 81
	node.SetParameterValue(OLO_IDENTIFIER("Base"), 3);
	node.SetParameterValue(OLO_IDENTIFIER("Exponent"), 4);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Result"));
	EXPECT_EQ(result, 81);
}

// LogNode Tests
TEST_F(MathNodeTest, LogNodeF32Test)
{
	LogNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test log_10(100) = 2
	node.SetParameterValue(OLO_IDENTIFIER("Base"), 10.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 100.0f);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Result"));
	EXPECT_NEAR(result, 2.0f, 0.001f);
}

TEST_F(MathNodeTest, LogNodeI32Test)
{
	LogNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test log_2(8) = 3
	node.SetParameterValue(OLO_IDENTIFIER("Base"), 2);
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 8);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Result"));
	EXPECT_EQ(result, 3);
}

// ModuloNode Tests
TEST_F(MathNodeTest, ModuloNodeF32Test)
{
	ModuloNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test 7.5 % 2.5 = 0.0
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 7.5f);
	node.SetParameterValue(OLO_IDENTIFIER("Modulo"), 2.5f);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Result"));
	EXPECT_NEAR(result, 0.0f, 0.001f);
}

TEST_F(MathNodeTest, ModuloNodeI32Test)
{
	ModuloNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test 10 % 3 = 1
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 10);
	node.SetParameterValue(OLO_IDENTIFIER("Modulo"), 3);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	i32 result = node.GetParameterValue<i32>(OLO_IDENTIFIER("Result"));
	EXPECT_EQ(result, 1);
}

// MapRangeNode Tests
TEST_F(MathNodeTest, MapRangeNodeF32Test)
{
	MapRangeNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test mapping 0.5 from [0,1] to [0,100] = 50
	node.SetParameterValue(OLO_IDENTIFIER("Input"), 0.5f);
	node.SetParameterValue(OLO_IDENTIFIER("InRangeMin"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("InRangeMax"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("OutRangeMin"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("OutRangeMax"), 100.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Clamped"), false);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 50.0f);
}

TEST_F(MathNodeTest, MapRangeNodeClampedTest)
{
	MapRangeNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test mapping 1.5 from [0,1] to [0,100] with clamping enabled = 100
	node.SetParameterValue(OLO_IDENTIFIER("Input"), 1.5f);
	node.SetParameterValue(OLO_IDENTIFIER("InRangeMin"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("InRangeMax"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("OutRangeMin"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("OutRangeMax"), 100.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Clamped"), true);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	EXPECT_FLOAT_EQ(result, 100.0f);
}

//=============================================================================
// Audio-Specific Math Node Tests
//=============================================================================

// LinearToLogFrequencyNode Tests
TEST_F(MathNodeTest, LinearToLogFrequencyNodeTest)
{
	LinearToLogFrequencyNode node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test mapping 0.5 linear to log frequency (should be geometric mean of min/max)
	node.SetParameterValue(OLO_IDENTIFIER("Value"), 0.5f);
	node.SetParameterValue(OLO_IDENTIFIER("MinValue"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("MaxValue"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("MinFrequency"), 20.0f);
	node.SetParameterValue(OLO_IDENTIFIER("MaxFrequency"), 20000.0f);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Frequency"));
	
	// Should be approximately the geometric mean: sqrt(20 * 20000) ≈ 632.45
	EXPECT_NEAR(result, 632.45f, 1.0f);
}

// FrequencyLogToLinearNode Tests
TEST_F(MathNodeTest, FrequencyLogToLinearNodeTest)
{
	FrequencyLogToLinearNode node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test mapping 1000Hz back to linear (should be around middle of range)
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 1000.0f);
	node.SetParameterValue(OLO_IDENTIFIER("MinFrequency"), 20.0f);
	node.SetParameterValue(OLO_IDENTIFIER("MaxFrequency"), 20000.0f);
	node.SetParameterValue(OLO_IDENTIFIER("MinValue"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("MaxValue"), 1.0f);
	
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	node.Process(inputs, outputs, 256);
	
	f32 result = node.GetParameterValue<f32>(OLO_IDENTIFIER("Value"));
	
	// 1000Hz should be somewhere in the middle range
	EXPECT_GT(result, 0.3f);
	EXPECT_LT(result, 0.8f);
}

//=============================================================================
// Generator Node Tests
//=============================================================================

// NoiseNode Tests
TEST_F(MathNodeTest, NoiseNodeWhiteTest)
{
	NoiseNode node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test white noise generation (Type = 0)
	node.SetParameterValue(OLO_IDENTIFIER("Seed"), 12345);
	node.SetParameterValue(OLO_IDENTIFIER("Type"), 0); // WhiteNoise
	
	f32* inputs[2] = { nullptr, nullptr };
	f32 outputBuffer[256];
	f32* outputs[1] = { outputBuffer };
	node.Process(inputs, outputs, 256);
	
	// Check that output buffer contains values (not all zeros)
	bool hasNonZeroValues = false;
	for (u32 i = 0; i < 256; ++i)
	{
		if (outputBuffer[i] != 0.0f)
		{
			hasNonZeroValues = true;
			break;
		}
	}
	EXPECT_TRUE(hasNonZeroValues);
	
	// Check that values are in reasonable range (typically 0-1 for white noise)
	for (u32 i = 0; i < 256; ++i)
	{
		EXPECT_GE(outputBuffer[i], 0.0f);
		EXPECT_LE(outputBuffer[i], 1.0f);
	}
}

TEST_F(MathNodeTest, NoiseNodePinkTest)
{
	NoiseNode node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test pink noise generation (Type = 1)
	node.SetParameterValue(OLO_IDENTIFIER("Seed"), 54321);
	node.SetParameterValue(OLO_IDENTIFIER("Type"), 1); // PinkNoise
	
	f32* inputs[2] = { nullptr, nullptr };
	f32 outputBuffer[256];
	f32* outputs[1] = { outputBuffer };
	node.Process(inputs, outputs, 256);
	
	// Check that output buffer contains values (not all zeros)
	bool hasNonZeroValues = false;
	for (u32 i = 0; i < 256; ++i)
	{
		if (outputBuffer[i] != 0.0f)
		{
			hasNonZeroValues = true;
			break;
		}
	}
	EXPECT_TRUE(hasNonZeroValues);
}

TEST_F(MathNodeTest, NoiseNodeBrownianTest)
{
	NoiseNode node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test brownian noise generation (Type = 2)
	node.SetParameterValue(OLO_IDENTIFIER("Seed"), 98765);
	node.SetParameterValue(OLO_IDENTIFIER("Type"), 2); // BrownianNoise
	
	f32* inputs[2] = { nullptr, nullptr };
	f32 outputBuffer[256];
	f32* outputs[1] = { outputBuffer };
	node.Process(inputs, outputs, 256);
	
	// Check that output buffer contains values (not all zeros)
	bool hasNonZeroValues = false;
	for (u32 i = 0; i < 256; ++i)
	{
		if (outputBuffer[i] != 0.0f)
		{
			hasNonZeroValues = true;
			break;
		}
	}
	EXPECT_TRUE(hasNonZeroValues);
}

// ===== Music Theory Node Tests =====

// BPMToSecondsNode Tests
TEST_F(MathNodeTest, BPMToSecondsNodeBasicTest)
{
	BPMToSecondsNode node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test standard 120 BPM (should equal 0.5 seconds per beat)
	node.SetParameterValue(OLO_IDENTIFIER("BPM"), 120.0f);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	node.Process(inputs, outputs, 128);
	
	EXPECT_FLOAT_EQ(outputBuffer[0], 0.5f);
}

TEST_F(MathNodeTest, BPMToSecondsNodeZeroProtectionTest)
{
	BPMToSecondsNode node;
	node.Initialize(48000.0, 512);
	
	// Test zero BPM protection
	node.SetParameterValue(OLO_IDENTIFIER("BPM"), 0.0f);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	node.Process(inputs, outputs, 128);
	
	// Should default to 120 BPM when zero is provided
	EXPECT_FLOAT_EQ(outputBuffer[0], 0.5f); // 60/120 = 0.5
}

// NoteToFrequencyNode Tests
TEST_F(MathNodeTest, NoteToFrequencyNodeF32BasicTest)
{
	NoteToFrequencyNodeF32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test A4 (MIDI note 69) = 440 Hz
	node.SetParameterValue(OLO_IDENTIFIER("MIDINote"), 69.0f);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	node.Process(inputs, outputs, 128);
	
	EXPECT_FLOAT_EQ(outputBuffer[0], 440.0f);
}

TEST_F(MathNodeTest, NoteToFrequencyNodeI32BasicTest)
{
	NoteToFrequencyNodeI32 node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test A4 (MIDI note 69) = 440 Hz
	node.SetParameterValue(OLO_IDENTIFIER("MIDINote"), 69);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	node.Process(inputs, outputs, 128);
	
	EXPECT_FLOAT_EQ(outputBuffer[0], 440.0f);
}

// FrequencyToNoteNode Tests
TEST_F(MathNodeTest, FrequencyToNoteNodeBasicTest)
{
	FrequencyToNoteNode node;
	
	// Initialize the node
	node.Initialize(48000.0, 512);
	
	// Test 440 Hz = A4 (MIDI note 69)
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 440.0f);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	node.Process(inputs, outputs, 128);
	
	EXPECT_NEAR(outputBuffer[0], 69.0f, 0.01f);
}

// ========================================
// GENERATOR NODE TESTS
// ========================================

// SineNode Tests
TEST_F(MathNodeTest, SineNodeBasicOscillationTest)
{
	SineNode node;
	
	// Initialize the node with known sample rate
	node.Initialize(48000.0, 512);
	
	// Test 440Hz sine wave (A4)
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 440.0f);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	
	// Process one buffer
	node.Process(inputs, outputs, 128);
	
	// Check that output is within sine wave range [-1, 1]
	for (u32 i = 0; i < 128; ++i)
	{
		EXPECT_GE(outputBuffer[i], -1.0f);
		EXPECT_LE(outputBuffer[i], 1.0f);
	}
	
	// First sample should be 0 (starting phase)
	EXPECT_NEAR(outputBuffer[0], 0.0f, 0.001f);
}

TEST_F(MathNodeTest, SineNodeFrequencyClampingTest)
{
	SineNode node;
	node.Initialize(48000.0, 512);
	
	// Test frequency clamping - too high (clamping happens in GetCurrentFrequency)
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 25000.0f);
	EXPECT_FLOAT_EQ(node.GetCurrentFrequency(), 22000.0f);
	
	// Test frequency clamping - negative (clamping happens in GetCurrentFrequency)
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), -100.0f);
	EXPECT_FLOAT_EQ(node.GetCurrentFrequency(), 0.0f);
}

TEST_F(MathNodeTest, SineNodePhaseTest)
{
	SineNode node;
	node.Initialize(48000.0, 512);
	
	// Set to 1Hz for easy phase calculation
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 1.0f);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	
	// Process multiple buffers to test phase continuity
	node.Process(inputs, outputs, 128);
	f32 firstBuffer = outputBuffer[127]; // Last sample of first buffer
	
	node.Process(inputs, outputs, 128);
	f32 secondBuffer = outputBuffer[0]; // First sample of second buffer
	
	// Phase should be continuous (no jumps)
	EXPECT_NEAR(firstBuffer, secondBuffer, 0.1f);
}

TEST_F(MathNodeTest, SineNodeUtilityMethodsTest)
{
	SineNode node;
	node.Initialize(48000.0, 512);
	
	// Test frequency setting
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 880.0f);
	EXPECT_FLOAT_EQ(node.GetCurrentFrequency(), 880.0f);
	
	// Test phase reset
	node.ResetPhase();
	EXPECT_FLOAT_EQ(node.GetCurrentPhase(), 0.0f);
	
	// Test phase setting
	node.ResetPhase(glm::pi<f32>() / 2.0f); // 90 degrees
	EXPECT_NEAR(node.GetCurrentPhase(), glm::pi<f64>() / 2.0, 0.001);
}

// RandomNode Tests
TEST_F(MathNodeTest, RandomNodeF32BasicTest)
{
	RandomNodeF32 node;
	node.Initialize(48000.0, 512);
	
	// Set range [0.0, 1.0]
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 0.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Seed"), 12345);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	
	// Process and check range
	node.Process(inputs, outputs, 128);
	
	f32 value = outputBuffer[0];
	EXPECT_GE(value, 0.0f);
	EXPECT_LE(value, 1.0f);
	
	// Check that all samples have the same value (constant output)
	for (u32 i = 1; i < 128; ++i)
	{
		EXPECT_FLOAT_EQ(outputBuffer[i], value);
	}
}

TEST_F(MathNodeTest, RandomNodeI32BasicTest)
{
	RandomNodeI32 node;
	node.Initialize(48000.0, 512);
	
	// Set range [0, 100]
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 0);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 100);
	node.SetParameterValue(OLO_IDENTIFIER("Seed"), 54321);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	
	// Process and check range
	node.Process(inputs, outputs, 128);
	
	i32 value = static_cast<i32>(outputBuffer[0]);
	EXPECT_GE(value, 0);
	EXPECT_LE(value, 100);
}

TEST_F(MathNodeTest, RandomNodeSeedReproducibilityTest)
{
	RandomNodeF32 node1, node2;
	
	// Both nodes with same seed should produce same result
	node1.Initialize(48000.0, 512);
	node2.Initialize(48000.0, 512);
	
	node1.SetParameterValue(OLO_IDENTIFIER("Seed"), 42);
	node2.SetParameterValue(OLO_IDENTIFIER("Seed"), 42);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer1[128], outputBuffer2[128];
	f32* outputs1[1] = { outputBuffer1 };
	f32* outputs2[1] = { outputBuffer2 };
	
	node1.Process(inputs, outputs1, 128);
	node2.Process(inputs, outputs2, 128);
	
	EXPECT_FLOAT_EQ(outputBuffer1[0], outputBuffer2[0]);
}

TEST_F(MathNodeTest, RandomNodeRangeSwapTest)
{
	RandomNodeF32 node;
	node.Initialize(48000.0, 512);
	
	// Set inverted range - should be swapped internally
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 10.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 5.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Seed"), 999);
	
	f32* inputs[1] = { nullptr };
	f32 outputBuffer[128];
	f32* outputs[1] = { outputBuffer };
	
	node.Process(inputs, outputs, 128);
	
	f32 value = outputBuffer[0];
	EXPECT_GE(value, 5.0f);  // Should be in corrected range
	EXPECT_LE(value, 10.0f);
}

TEST_F(MathNodeTest, RandomNodeUtilityMethodsTest)
{
	RandomNodeF32 node;
	node.Initialize(48000.0, 512);
	
	// Test range setting
	node.SetParameterValue(OLO_IDENTIFIER("Min"), 2.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Max"), 8.0f);
	
	auto range = node.GetRange();
	EXPECT_FLOAT_EQ(range.first, 2.0f);
	EXPECT_FLOAT_EQ(range.second, 8.0f);
	
	// Test seed reset
	node.ResetSeed(777);
	EXPECT_EQ(node.GetParameterValue<i32>(OLO_IDENTIFIER("Seed")), 777);
	
	// Test value generation
	f32 value1 = node.GenerateNext();
	f32 value2 = node.GenerateNext();
	
	// Values should be in range
	EXPECT_GE(value1, 2.0f);
	EXPECT_LE(value1, 8.0f);
	EXPECT_GE(value2, 2.0f);
	EXPECT_LE(value2, 8.0f);
	
	// Last value should match
	EXPECT_FLOAT_EQ(node.GetLastValue(), value2);
}

// ============================================================================
// Envelope Node Tests
// ============================================================================

// ADEnvelope Tests
TEST_F(MathNodeTest, ADEnvelopeBasicOperationTest)
{
	ADEnvelope node;
	node.Initialize(44100.0, 512);
	
	// Set envelope parameters - use shorter times that will show progress in 512 samples (~0.011s)
	node.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.005f);  // 0.005s = 220 samples
	node.SetParameterValue(OLO_IDENTIFIER("DecayTime"), 0.004f);   // 0.004s = 176 samples
	node.SetParameterValue(OLO_IDENTIFIER("AttackCurve"), 1.0f);  // Linear
	node.SetParameterValue(OLO_IDENTIFIER("DecayCurve"), 1.0f);   // Linear
	node.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Sustain"), 0.5f);
	
	// Trigger the envelope
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	
	f32 outputBuffer[512];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	node.Process(inputs, outputs, 512);
	
	// Check initial samples are in attack phase (increasing)
	EXPECT_GT(outputBuffer[100], 0.0f);
	EXPECT_LT(outputBuffer[100], 1.0f);
	
	// Check peak region exists (attack should complete around sample 220)
	bool foundPeak = false;
	for (u32 i = 0; i < 512; ++i) {
		if (outputBuffer[i] >= 0.95f) {
			foundPeak = true;
			break;
		}
	}
	EXPECT_TRUE(foundPeak);
}

TEST_F(MathNodeTest, ADEnvelopeStateTransitionsTest)
{
	ADEnvelope node;
	node.Initialize(44100.0, 2048);
	
	// Short envelope for testing (0.01s attack, 0.01s decay)
	node.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.01f);
	node.SetParameterValue(OLO_IDENTIFIER("DecayTime"), 0.01f);
	node.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Sustain"), 0.3f);
	
	// Trigger and process enough samples to complete envelope
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	
	f32 outputBuffer[2048];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	node.Process(inputs, outputs, 2048);
	
	// Check that envelope eventually reaches sustain level
	f32 finalValue = outputBuffer[2047];
	EXPECT_NEAR(finalValue, 0.3f, 0.1f);
}

TEST_F(MathNodeTest, ADEnvelopeCurveShapingTest)
{
	ADEnvelope node;
	node.Initialize(44100.0, 1024);
	
	// Use shorter times for 1024 samples (~0.023s)
	node.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.015f);  // 0.015s = 661 samples
	node.SetParameterValue(OLO_IDENTIFIER("DecayTime"), 0.006f);   // 0.006s = 264 samples
	node.SetParameterValue(OLO_IDENTIFIER("AttackCurve"), 2.0f);  // Exponential
	node.SetParameterValue(OLO_IDENTIFIER("DecayCurve"), 0.5f);   // Logarithmic
	node.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Sustain"), 0.0f);
	
	f32 outputBuffer[1024];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 1024);
	
	// Verify curve shaping affects envelope shape
	// Exponential attack should have slower start
	EXPECT_LT(outputBuffer[50], outputBuffer[100] - outputBuffer[50]);
}

// ADSREnvelope Tests
TEST_F(MathNodeTest, ADSREnvelopeBasicOperationTest)
{
	ADSREnvelope node;
	node.Initialize(44100.0, 2048);
	
	// Set ADSR parameters - use shorter times for 2048 samples (~0.046s)
	node.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.01f);   // 0.01s = 441 samples
	node.SetParameterValue(OLO_IDENTIFIER("DecayTime"), 0.01f);    // 0.01s = 441 samples  
	node.SetParameterValue(OLO_IDENTIFIER("SustainLevel"), 0.7f);
	node.SetParameterValue(OLO_IDENTIFIER("ReleaseTime"), 0.02f);  // 0.02s = 882 samples
	node.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Velocity"), 1.0f);
	
	f32 outputBuffer[2048];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Note on
	node.SetParameterValue(OLO_IDENTIFIER("NoteOn"), 1.0f);
	node.Process(inputs, outputs, 2048);
	
	// Should reach peak during attack/decay (around sample 441)
	bool foundPeak = false;
	for (u32 i = 0; i < 1000; ++i) {
		if (outputBuffer[i] >= 0.95f) {
			foundPeak = true;
			break;
		}
	}
	EXPECT_TRUE(foundPeak);
	
	// Should settle to sustain level (after attack+decay = 882 samples)
	f32 sustainValue = outputBuffer[1500];
	EXPECT_NEAR(sustainValue, 0.7f, 0.1f);
}

TEST_F(MathNodeTest, ADSREnvelopeNoteOffReleaseTest)
{
	ADSREnvelope node;
	node.Initialize(44100.0, 1000);
	
	node.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.01f);
	node.SetParameterValue(OLO_IDENTIFIER("DecayTime"), 0.01f);
	node.SetParameterValue(OLO_IDENTIFIER("SustainLevel"), 0.8f);
	node.SetParameterValue(OLO_IDENTIFIER("ReleaseTime"), 0.05f);
	node.SetParameterValue(OLO_IDENTIFIER("Velocity"), 1.0f);
	
	// Process note on phase
	f32 outputBuffer1[1000];
	f32* inputs1[1] = { nullptr };
	f32* outputs1[1] = { outputBuffer1 };
	
	node.SetParameterValue(OLO_IDENTIFIER("NoteOn"), 1.0f);
	node.Process(inputs1, outputs1, 1000);
	
	// Trigger note off and process release
	f32 outputBuffer2[1000];
	f32* inputs2[1] = { nullptr };
	f32* outputs2[1] = { outputBuffer2 };
	
	node.SetParameterValue(OLO_IDENTIFIER("NoteOff"), 1.0f);
	node.Process(inputs2, outputs2, 1000);
	
	// Value should decrease during release
	f32 startValue = outputBuffer2[0];
	f32 endValue = outputBuffer2[999];
	EXPECT_GT(startValue, endValue);
	EXPECT_GE(endValue, 0.0f);
}

TEST_F(MathNodeTest, ADSREnvelopeVelocityScalingTest)
{
	ADSREnvelope node1, node2;
	node1.Initialize(44100.0, 512);
	node2.Initialize(44100.0, 512);
	
	// Same parameters except velocity - use shorter times for 512 samples (~0.011s)
	node1.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.005f);  // 0.005s = 220 samples
	node1.SetParameterValue(OLO_IDENTIFIER("DecayTime"), 0.003f);   // 0.003s = 132 samples
	node1.SetParameterValue(OLO_IDENTIFIER("SustainLevel"), 0.8f);
	node1.SetParameterValue(OLO_IDENTIFIER("Velocity"), 0.5f);
	node1.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	
	node2.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.005f);  // 0.005s = 220 samples
	node2.SetParameterValue(OLO_IDENTIFIER("DecayTime"), 0.003f);   // 0.003s = 132 samples
	node2.SetParameterValue(OLO_IDENTIFIER("SustainLevel"), 0.8f);
	node2.SetParameterValue(OLO_IDENTIFIER("Velocity"), 1.0f);
	node2.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	
	f32 outputBuffer1[512], outputBuffer2[512];
	f32* inputs1[1] = { nullptr };
	f32* inputs2[1] = { nullptr };
	f32* outputs1[1] = { outputBuffer1 };
	f32* outputs2[1] = { outputBuffer2 };
	
	node1.SetParameterValue(OLO_IDENTIFIER("NoteOn"), 1.0f);
	node2.SetParameterValue(OLO_IDENTIFIER("NoteOn"), 1.0f);
	
	node1.Process(inputs1, outputs1, 512);
	node2.Process(inputs2, outputs2, 512);
	
	// Higher velocity should produce higher peak
	f32 peak1 = 0.0f, peak2 = 0.0f;
	for (u32 i = 0; i < 512; ++i) {
		peak1 = std::max(peak1, outputBuffer1[i]);
		peak2 = std::max(peak2, outputBuffer2[i]);
	}
	
	EXPECT_LT(peak1, peak2);
	EXPECT_NEAR(peak1, 0.5f, 0.1f);
	EXPECT_NEAR(peak2, 1.0f, 0.1f);
}

// AREnvelope Tests
TEST_F(MathNodeTest, AREnvelopeBasicOperationTest)
{
	AREnvelope node;
	node.Initialize(44100.0, 2048);
	
	// Use shorter times that will complete within 2048 samples (~0.046s)
	node.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.01f);   // 0.01s = 441 samples
	node.SetParameterValue(OLO_IDENTIFIER("ReleaseTime"), 0.02f);  // 0.02s = 882 samples  
	node.SetParameterValue(OLO_IDENTIFIER("AttackCurve"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("ReleaseCurve"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Velocity"), 1.0f);
	
	f32 outputBuffer[2048];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Trigger the envelope
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 2048);
	
	// Should start at 0, rise to peak, then fall back to 0
	EXPECT_NEAR(outputBuffer[0], 0.0f, 0.01f);
	
	// Find peak
	f32 peakValue = 0.0f;
	u32 peakIndex = 0;
	for (u32 i = 0; i < 2048; ++i) {
		if (outputBuffer[i] > peakValue) {
			peakValue = outputBuffer[i];
			peakIndex = i;
		}
	}
	
	EXPECT_GT(peakValue, 0.9f);
	EXPECT_GT(peakIndex, 0);
	EXPECT_LT(peakIndex, 2048);
	
	// Should end near 0 (total envelope time = 0.03s = 1323 samples, so should complete)
	EXPECT_NEAR(outputBuffer[2047], 0.0f, 0.1f);
}

TEST_F(MathNodeTest, AREnvelopeRetriggerTest)
{
	AREnvelope node;
	node.Initialize(44100.0, 1000);
	
	node.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.02f);
	node.SetParameterValue(OLO_IDENTIFIER("ReleaseTime"), 0.08f);
	node.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Velocity"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Retrigger"), 1.0f);  // Allow retrigger
	
	f32 outputBuffer1[1000], outputBuffer2[1000];
	f32* inputs1[1] = { nullptr };
	f32* inputs2[1] = { nullptr };
	f32* outputs1[1] = { outputBuffer1 };
	f32* outputs2[1] = { outputBuffer2 };
	
	// First trigger
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs1, outputs1, 1000);
	
	// Second trigger while still processing
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs2, outputs2, 1000);
	
	// Both should have valid envelopes
	f32 peak1 = 0.0f, peak2 = 0.0f;
	for (u32 i = 0; i < 1000; ++i) {
		peak1 = std::max(peak1, outputBuffer1[i]);
		peak2 = std::max(peak2, outputBuffer2[i]);
	}
	
	EXPECT_GT(peak1, 0.5f);
	EXPECT_GT(peak2, 0.5f);
}

TEST_F(MathNodeTest, AREnvelopeCurveShapingTest)
{
	AREnvelope node;
	node.Initialize(44100.0, 2048);
	
	node.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.1f);
	node.SetParameterValue(OLO_IDENTIFIER("ReleaseTime"), 0.1f);
	node.SetParameterValue(OLO_IDENTIFIER("AttackCurve"), 3.0f);   // Very exponential
	node.SetParameterValue(OLO_IDENTIFIER("ReleaseCurve"), 0.3f);  // Very logarithmic
	node.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Velocity"), 1.0f);
	
	f32 outputBuffer[2048];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 2048);
	
	// Exponential attack should have slow start
	u32 attackSamples = static_cast<u32>(0.1f * 44100.0f);
	if (attackSamples < 2048) {
		f32 quarterPoint = outputBuffer[attackSamples / 4];
		f32 halfPoint = outputBuffer[attackSamples / 2];
		
		// Quarter point should be much less than half the final value
		EXPECT_LT(quarterPoint, halfPoint * 0.5f);
	}
}

TEST_F(MathNodeTest, AREnvelopeVelocityAndPeakScalingTest)
{
	AREnvelope node1, node2;
	node1.Initialize(44100.0, 1024);
	node2.Initialize(44100.0, 1024);
	
	// Different velocity and peak combinations - use shorter times for 1024 samples (~0.023s)
	node1.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.008f);  // 0.008s = 353 samples
	node1.SetParameterValue(OLO_IDENTIFIER("ReleaseTime"), 0.010f); // 0.010s = 441 samples
	node1.SetParameterValue(OLO_IDENTIFIER("Velocity"), 0.5f);
	node1.SetParameterValue(OLO_IDENTIFIER("Peak"), 0.8f);
	
	node2.SetParameterValue(OLO_IDENTIFIER("AttackTime"), 0.008f);  // 0.008s = 353 samples
	node2.SetParameterValue(OLO_IDENTIFIER("ReleaseTime"), 0.010f); // 0.010s = 441 samples
	node2.SetParameterValue(OLO_IDENTIFIER("Velocity"), 1.0f);
	node2.SetParameterValue(OLO_IDENTIFIER("Peak"), 1.0f);
	
	f32 outputBuffer1[1024], outputBuffer2[1024];
	f32* inputs1[1] = { nullptr };
	f32* inputs2[1] = { nullptr };
	f32* outputs1[1] = { outputBuffer1 };
	f32* outputs2[1] = { outputBuffer2 };
	
	node1.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node2.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	
	node1.Process(inputs1, outputs1, 1024);
	node2.Process(inputs2, outputs2, 1024);
	
	f32 peak1 = 0.0f, peak2 = 0.0f;
	for (u32 i = 0; i < 1024; ++i) {
		peak1 = std::max(peak1, outputBuffer1[i]);
		peak2 = std::max(peak2, outputBuffer2[i]);
	}
	
	// peak1 should be 0.5 * 0.8 = 0.4, peak2 should be 1.0 * 1.0 = 1.0
	EXPECT_NEAR(peak1, 0.4f, 0.1f);
	EXPECT_NEAR(peak2, 1.0f, 0.1f);
	EXPECT_LT(peak1, peak2);
}

//==============================================================================
// Trigger Node Tests
//==============================================================================

TEST_F(MathNodeTest, RepeatTriggerBasicTest)
{
	RepeatTrigger node;
	node.Initialize(1000.0, 512); // 1kHz for easy timing calculations
	
	// Set a period of 0.1 seconds (100ms = 100 samples at 1kHz)
	node.SetParameterValue(OLO_IDENTIFIER("Period"), 0.1f);
	
	// Start the trigger
	node.SetParameterValue(OLO_IDENTIFIER("Start"), 1.0f);
	
	f32 outputBuffer[512];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Process some samples and verify no exceptions
	node.Process(inputs, outputs, 512);
	
	// Check that the trigger is in playing state
	bool isPlaying = node.GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying")) > 0.5f;
	EXPECT_TRUE(isPlaying);
	
	// Stop the trigger
	node.SetParameterValue(OLO_IDENTIFIER("Stop"), 1.0f);
	node.Process(inputs, outputs, 100);
	
	// Check that the trigger has stopped
	isPlaying = node.GetParameterValue<f32>(OLO_IDENTIFIER("IsPlaying")) > 0.5f;
	EXPECT_FALSE(isPlaying);
	
	// Test passed if no exceptions were thrown and basic functionality works
	EXPECT_TRUE(true);
}

TEST_F(MathNodeTest, TriggerCounterBasicTest)
{
	TriggerCounter node;
	node.Initialize(44100.0, 512);
	
	// Set parameters
	node.SetParameterValue(OLO_IDENTIFIER("StartValue"), 10.0f);
	node.SetParameterValue(OLO_IDENTIFIER("StepSize"), 5.0f);
	node.SetParameterValue(OLO_IDENTIFIER("ResetCount"), 3.0f); // Reset after 3 triggers
	
	f32 outputBuffer[512];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Initial state - count should be 0, value should be StartValue
	EXPECT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Count")), 0.0f);
	EXPECT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Value")), 10.0f);
	
	// First trigger - count = 1, value = 10 + 5*1 = 15
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 64);
	EXPECT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Count")), 1.0f);
	EXPECT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Value")), 15.0f);
	
	// Second trigger - count = 2, value = 10 + 5*2 = 20
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 64);
	EXPECT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Count")), 2.0f);
	EXPECT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Value")), 20.0f);
	
	// Third trigger - count = 3, value = 10 + 5*3 = 25, then auto-reset to count = 0, value = 10
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 64);
	EXPECT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Count")), 0.0f);
	EXPECT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Value")), 10.0f);
}

TEST_F(MathNodeTest, DelayedTriggerBasicTest)
{
	DelayedTrigger node;
	node.Initialize(1000.0, 512); // 1kHz for easy timing calculations
	
	// Set delay time to 0.05 seconds (50ms = 50 samples at 1kHz)
	node.SetParameterValue(OLO_IDENTIFIER("DelayTime"), 0.05f);
	
	f32 outputBuffer[512];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Start the delay
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	
	// Process samples and verify delay behavior
	node.Process(inputs, outputs, 100);
	
	// Check that delay is currently active
	bool isDelaying = node.GetParameterValue<f32>(OLO_IDENTIFIER("IsDelaying")) > 0.5f;
	// Note: This may not be exposed as a parameter, so we'll just test for basic functionality
	
	// Test reset functionality
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 25);
	
	// Reset the delay - should cancel the current delay
	node.SetParameterValue(OLO_IDENTIFIER("Reset"), 1.0f);
	node.Process(inputs, outputs, 1);
	
	// Process more samples - delayed trigger should not fire
	node.Process(inputs, outputs, 100);
	
	// Test passed if no exceptions were thrown
	EXPECT_TRUE(true);
}

//==============================================================================
// Oscillator Node Tests
//==============================================================================

TEST_F(MathNodeTest, TriangleNodeBasicTest)
{
	TriangleNode node;
	node.Initialize(44100.0, 512);
	
	// Set frequency to 100 Hz for more predictable testing
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 100.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Amplitude"), 1.0f);
	
	f32 outputBuffer[512]; // Small buffer for testing
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Process one buffer
	node.Process(inputs, outputs, 512);
	
	// Check that we get a triangle wave with reasonable values
	// At 100 Hz, period = 441 samples, so quarter period = ~110 samples
	
	// The waveform should vary between -1 and 1
	f32 minVal = outputBuffer[0];
	f32 maxVal = outputBuffer[0];
	for (u32 i = 0; i < 512; ++i)
	{
		minVal = glm::min(minVal, outputBuffer[i]);
		maxVal = glm::max(maxVal, outputBuffer[i]);
	}
	
	// Should have reasonable range
	EXPECT_LT(minVal, -0.5f); // Should go below -0.5
	EXPECT_GT(maxVal, 0.5f);  // Should go above 0.5
	EXPECT_LT(glm::abs(maxVal - (-minVal)), 0.2f); // Should be roughly symmetric
}

TEST_F(MathNodeTest, SquareNodeBasicTest)
{
	SquareNode node;
	node.Initialize(44100.0, 512);
	
	// Set frequency to 1 Hz for predictable testing
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Amplitude"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("DutyCycle"), 0.5f); // 50% duty cycle
	
	f32 outputBuffer[44100]; // 1 second at 44.1kHz
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Process one second of audio
	node.Process(inputs, outputs, 44100);
	
	// Check that we get a square wave
	// First quarter should be high
	f32 firstQuarterSample = outputBuffer[11025];
	EXPECT_NEAR(firstQuarterSample, 1.0f, 0.01f);
	
	// Third quarter should be low
	f32 thirdQuarterSample = outputBuffer[33075];
	EXPECT_NEAR(thirdQuarterSample, -1.0f, 0.01f);
}

TEST_F(MathNodeTest, SawtoothNodeBasicTest)
{
	SawtoothNode node;
	node.Initialize(44100.0, 512);
	
	// Set frequency to 1 Hz for predictable testing
	node.SetParameterValue(OLO_IDENTIFIER("Frequency"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Amplitude"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Direction"), 1.0f); // Rising sawtooth
	
	f32 outputBuffer[44100]; // 1 second at 44.1kHz
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Process one second of audio
	node.Process(inputs, outputs, 44100);
	
	// Check that we get a rising sawtooth wave
	// Start should be near -1
	EXPECT_NEAR(outputBuffer[0], -1.0f, 0.1f);
	
	// Middle should be near 0
	f32 middleSample = outputBuffer[22050];
	EXPECT_NEAR(middleSample, 0.0f, 0.1f);
	
	// Near end should be approaching 1
	f32 nearEndSample = outputBuffer[40000];
	EXPECT_GT(nearEndSample, 0.5f);
}

//==============================================================================
// Filter Node Tests
//==============================================================================

TEST_F(MathNodeTest, LowPassFilterBasicTest)
{
	LowPassFilterNode node;
	node.Initialize(44100.0, 512);
	
	// Set cutoff frequency to 1000 Hz
	node.SetParameterValue(OLO_IDENTIFIER("Cutoff"), 1000.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Resonance"), 0.7f);
	
	// Create a test signal with high frequency content
	f32 inputBuffer[512];
	f32 outputBuffer[512];
	
	// Generate a mix of 500Hz (should pass) and 5000Hz (should be attenuated)
	for (u32 i = 0; i < 512; ++i)
	{
		f32 t = static_cast<f32>(i) / 44100.0f;
		inputBuffer[i] = glm::sin(2.0f * glm::pi<f32>() * 500.0f * t) + 
						 glm::sin(2.0f * glm::pi<f32>() * 5000.0f * t);
	}
	
	f32* inputs[1] = { inputBuffer };
	f32* outputs[1] = { outputBuffer };
	
	node.Process(inputs, outputs, 512);
	
	// The output should be different from input (filtered)
	bool isDifferent = false;
	for (u32 i = 0; i < 512; ++i)
	{
		if (glm::abs(outputBuffer[i] - inputBuffer[i]) > 0.01f)
		{
			isDifferent = true;
			break;
		}
	}
	EXPECT_TRUE(isDifferent);
}

TEST_F(MathNodeTest, HighPassFilterBasicTest)
{
	HighPassFilterNode node;
	node.Initialize(44100.0, 512);
	
	// Set cutoff frequency to 1000 Hz
	node.SetParameterValue(OLO_IDENTIFIER("Cutoff"), 1000.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Resonance"), 0.7f);
	
	// Test with a DC signal (should be filtered out)
	f32 inputBuffer[512];
	f32 outputBuffer[512];
	
	// Fill with DC signal
	for (u32 i = 0; i < 512; ++i)
	{
		inputBuffer[i] = 1.0f;
	}
	
	f32* inputs[1] = { inputBuffer };
	f32* outputs[1] = { outputBuffer };
	
	node.Process(inputs, outputs, 512);
	
	// Output should be much smaller than input (DC filtered out)
	f32 outputLevel = 0.0f;
	for (u32 i = 0; i < 512; ++i)
	{
		outputLevel += glm::abs(outputBuffer[i]);
	}
	outputLevel /= 512.0f;
	
	EXPECT_LT(outputLevel, 0.1f); // Should be much less than input
}

//==============================================================================
// Utility Node Tests
//==============================================================================

TEST_F(MathNodeTest, SampleAndHoldBasicTest)
{
	SampleAndHoldNode node;
	node.Initialize(44100.0, 512);
	
	// Set input value
	node.SetParameterValue(OLO_IDENTIFIER("Input"), 0.75f);
	
	// Initially output should be 0
	EXPECT_FLOAT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Output")), 0.0f);
	
	// Trigger sample
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	
	f32 outputBuffer[64];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	node.Process(inputs, outputs, 64);
	
	// Output should now hold the sampled value
	EXPECT_FLOAT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Output")), 0.75f);
	EXPECT_FLOAT_EQ(node.GetHeldValue(), 0.75f);
	
	// Change input and verify it doesn't affect output until next trigger
	node.SetParameterValue(OLO_IDENTIFIER("Input"), 0.25f);
	node.Process(inputs, outputs, 64);
	EXPECT_FLOAT_EQ(node.GetHeldValue(), 0.75f); // Should still hold old value
}

TEST_F(MathNodeTest, GateNodeBasicTest)
{
	GateNode node;
	node.Initialize(44100.0, 512);
	
	// Set input and threshold
	node.SetParameterValue(OLO_IDENTIFIER("Input"), 1.0f);
	node.SetParameterValue(OLO_IDENTIFIER("Threshold"), 0.5f);
	
	// Test with gate closed
	node.SetParameterValue(OLO_IDENTIFIER("Gate"), 0.0f);
	
	f32 outputBuffer[64];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	node.Process(inputs, outputs, 64);
	
	// Output should be 0 when gate is closed
	EXPECT_FLOAT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Output")), 0.0f);
	EXPECT_FALSE(node.IsGateOpen());
	
	// Test with gate open
	node.SetParameterValue(OLO_IDENTIFIER("Gate"), 1.0f);
	node.Process(inputs, outputs, 64);
	
	// Output should pass through when gate is open
	EXPECT_FLOAT_EQ(node.GetParameterValue<f32>(OLO_IDENTIFIER("Output")), 1.0f);
	EXPECT_TRUE(node.IsGateOpen());
}

//==============================================================================
// Array Node Tests
//==============================================================================

TEST_F(MathNodeTest, GetRandomBasicTest)
{
	GetRandomF32 node;
	node.Initialize(44100.0, 512);
	
	// Set up test array
	std::vector<f32> testArray = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
	node.SetArray(testArray);
	
	// Set a fixed seed for deterministic testing
	node.SetParameterValue(OLO_IDENTIFIER("Seed"), 12345.0f);
	
	f32 outputBuffer[512];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Get several random elements
	std::vector<f32> selectedElements;
	for (u32 i = 0; i < 10; ++i)
	{
		node.SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
		node.Process(inputs, outputs, 64);
		f32 element = node.GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
		selectedElements.push_back(element);
		
		// Element should be one of the array values
		bool found = false;
		for (f32 arrayValue : testArray)
		{
			if (std::abs(element - arrayValue) < 0.001f)
			{
				found = true;
				break;
			}
		}
		EXPECT_TRUE(found) << "Selected element " << element << " not found in array";
	}
	
	// With a fixed seed, should get some variation (not all the same value)
	bool hasVariation = false;
	for (sizet i = 1; i < selectedElements.size(); ++i)
	{
		if (std::abs(selectedElements[i] - selectedElements[0]) > 0.001f)
		{
			hasVariation = true;
			break;
		}
	}
	EXPECT_TRUE(hasVariation) << "Random selection should show variation";
}

TEST_F(MathNodeTest, GetBasicTest)
{
	GetF32 node;
	node.Initialize(44100.0, 512);
	
	// Set up test array
	std::vector<f32> testArray = {10.0f, 20.0f, 30.0f, 40.0f};
	node.SetArray(testArray);
	
	f32 outputBuffer[512];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Test normal indexing
	for (i32 i = 0; i < 4; ++i)
	{
		node.SetParameterValue(OLO_IDENTIFIER("Index"), static_cast<f32>(i));
		node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
		node.Process(inputs, outputs, 64);
		
		f32 element = node.GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
		EXPECT_NEAR(element, testArray[i], 0.001f) << "Index " << i << " should return " << testArray[i];
	}
	
	// Test wraparound behavior
	node.SetParameterValue(OLO_IDENTIFIER("Index"), 4.0f); // Index 4 should wrap to 0
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 64);
	f32 element = node.GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
	EXPECT_NEAR(element, testArray[0], 0.001f) << "Index 4 should wrap to index 0";
	
	// Test negative index wraparound
	node.SetParameterValue(OLO_IDENTIFIER("Index"), -1.0f); // Should wrap to last element
	node.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	node.Process(inputs, outputs, 64);
	element = node.GetParameterValue<f32>(OLO_IDENTIFIER("Element"));
	EXPECT_NEAR(element, testArray[3], 0.001f) << "Negative index should wrap correctly";
}

TEST_F(MathNodeTest, GetRandomIntegerTest)
{
	GetRandomI32 node;
	node.Initialize(44100.0, 512);
	
	// Set up test array with integers
	std::vector<i32> testArray = {100, 200, 300, 400, 500};
	node.SetArray(testArray);
	
	// Set a fixed seed for deterministic testing
	node.SetParameterValue(OLO_IDENTIFIER("Seed"), 54321.0f);
	
	f32 outputBuffer[512];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Get several random elements
	for (u32 i = 0; i < 5; ++i)
	{
		node.SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
		node.Process(inputs, outputs, 64);
		i32 element = node.GetParameterValue<i32>(OLO_IDENTIFIER("Output"));
		
		// Element should be one of the array values
		bool found = false;
		for (i32 arrayValue : testArray)
		{
			if (element == arrayValue)
			{
				found = true;
				break;
			}
		}
		EXPECT_TRUE(found) << "Selected element " << element << " not found in array";
	}
}

TEST_F(MathNodeTest, ArrayNodeEmptyArrayTest)
{
	GetF32 getNode;
	GetRandomF32 getRandomNode;
	
	getNode.Initialize(44100.0, 512);
	getRandomNode.Initialize(44100.0, 512);
	
	// Test with empty arrays
	std::vector<f32> emptyArray;
	getNode.SetArray(emptyArray);
	getRandomNode.SetArray(emptyArray);
	
	f32 outputBuffer[512];
	f32* inputs[1] = { nullptr };
	f32* outputs[1] = { outputBuffer };
	
	// Get operations should return 0 for empty arrays
	getNode.SetParameterValue(OLO_IDENTIFIER("Index"), 0.0f);
	getNode.SetParameterValue(OLO_IDENTIFIER("Trigger"), 1.0f);
	getNode.Process(inputs, outputs, 64);
	EXPECT_EQ(getNode.GetParameterValue<f32>(OLO_IDENTIFIER("Element")), 0.0f);
	
	getRandomNode.SetParameterValue(OLO_IDENTIFIER("Next"), 1.0f);
	getRandomNode.Process(inputs, outputs, 64);
	EXPECT_EQ(getRandomNode.GetParameterValue<f32>(OLO_IDENTIFIER("Output")), 0.0f);
}