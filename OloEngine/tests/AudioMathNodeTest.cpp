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