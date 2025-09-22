#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/SoundGraph/Nodes/SubtractNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MultiplyNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/DivideNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MinNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MaxNode.h"
#include "OloEngine/Audio/SoundGraph/Nodes/ClampNode.h"

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