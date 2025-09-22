#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine.h"
#include "OloEngine/Audio/SoundGraph/Nodes/AddNode.h"

using namespace OloEngine::Audio::SoundGraph;

TEST(EndpointRegistrationTest, BasicParameterOperations)
{
	// Create an AddNode
	auto addNode = std::make_unique<AddNodeF32>();
	
	// Initialize the node
	addNode->Initialize(48000.0, 512);
	
	// Test parameter setting and getting
	addNode->SetParameterValue(OLO_IDENTIFIER("InputA"), 5.0f);
	addNode->SetParameterValue(OLO_IDENTIFIER("InputB"), 3.0f);
	
	// Get the parameter values back
	f32 inputA = addNode->GetParameterValue<f32>(OLO_IDENTIFIER("InputA"));
	f32 inputB = addNode->GetParameterValue<f32>(OLO_IDENTIFIER("InputB"));
	
	// Verify parameter values were set correctly
	EXPECT_EQ(inputA, 5.0f);
	EXPECT_EQ(inputB, 3.0f);
}

TEST(EndpointRegistrationTest, NodeProcessing)
{
	// Create an AddNode
	auto addNode = std::make_unique<AddNodeF32>();
	
	// Initialize the node
	addNode->Initialize(48000.0, 512);
	
	// Set input parameters
	addNode->SetParameterValue(OLO_IDENTIFIER("InputA"), 5.0f);
	addNode->SetParameterValue(OLO_IDENTIFIER("InputB"), 3.0f);
	
	// Process the node (simulate audio processing)
	f32* inputs[2] = { nullptr, nullptr };
	f32* outputs[1] = { nullptr };
	addNode->Process(inputs, outputs, 1);
	
	// Get the result
	f32 result = addNode->GetParameterValue<f32>(OLO_IDENTIFIER("Output"));
	
	// Verify addition was performed correctly
	EXPECT_EQ(result, 8.0f);
}

TEST(EndpointRegistrationTest, ParameterRegistry)
{
	// Create an AddNode
	auto addNode = std::make_unique<AddNodeF32>();
	
	// Initialize the node
	addNode->Initialize(48000.0, 512);
	
	// Test parameter registry introspection
	const auto& params = addNode->GetParameterRegistry().GetParameters();
	
	// Should have 3 parameters: InputA, InputB, Output
	EXPECT_EQ(params.size(), 3);
	
	// Check that specific identifiers exist in the registry
	auto inputA_ID = OLO_IDENTIFIER("InputA");
	auto inputB_ID = OLO_IDENTIFIER("InputB");
	auto output_ID = OLO_IDENTIFIER("Output");
	
	EXPECT_TRUE(params.find(inputA_ID) != params.end());
	EXPECT_TRUE(params.find(inputB_ID) != params.end());
	EXPECT_TRUE(params.find(output_ID) != params.end());
}

TEST(EndpointRegistrationTest, NodeMetadata)
{
	// Create an AddNode
	auto addNode = std::make_unique<AddNodeF32>();
	
	// Test node metadata
	EXPECT_TRUE(addNode->GetTypeID().IsValid());
	EXPECT_STREQ(addNode->GetDisplayName(), "Add (f32)");
}