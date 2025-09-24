#include "OloEngine/Audio/SoundGraph/Nodes/AddNode.h"
#include "OloEngine/Core/Reflection/Reflection.h"

#include <gtest/gtest.h>
#include <iostream>

using namespace OloEngine::Audio::SoundGraph;
using namespace OloEngine::Core::Reflection;

class ReflectionSystemTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        node = std::make_unique<AddNodeF32>();
        node->Initialize(44100.0, 512);
    }

    std::unique_ptr<AddNodeF32> node;
};

TEST_F(ReflectionSystemTest, NodeHasReflectionDescription)
{
    // Test that our node is properly described
    EXPECT_TRUE(IsDescribedNode_v<AddNodeReflectedF32>);
    
    // Test that we have the expected number of inputs and outputs
    using Inputs = NodeDescription<AddNodeReflectedF32>::Inputs;
    using Outputs = NodeDescription<AddNodeReflectedF32>::Outputs;
    
    EXPECT_EQ(Inputs::Count(), 2); // in_InputA, in_InputB
    EXPECT_EQ(Outputs::Count(), 1); // out_Result
}

TEST_F(ReflectionSystemTest, MemberNamesAreExtractedCorrectly)
{
    using Inputs = NodeDescription<AddNodeReflectedF32>::Inputs;
    using Outputs = NodeDescription<AddNodeReflectedF32>::Outputs;
    
    // Check input member names (should have prefixes removed)
    EXPECT_EQ(Inputs::MemberNames[0], "InputA");
    EXPECT_EQ(Inputs::MemberNames[1], "InputB");
    
    // Check output member names
    EXPECT_EQ(Outputs::MemberNames[0], "Result");
}

TEST_F(ReflectionSystemTest, EndpointRegistrationWorks)
{
    // The constructor should have called RegisterEndpoints
    // Verify that parameters were registered
    EXPECT_TRUE(node->HasParameter(OLO_IDENTIFIER("InputA")));
    EXPECT_TRUE(node->HasParameter(OLO_IDENTIFIER("InputB")));
}

TEST_F(ReflectionSystemTest, InputPointersAreConnected)
{
    // After Initialize, input pointers should be connected
    EXPECT_NE(node->in_InputA, nullptr);
    EXPECT_NE(node->in_InputB, nullptr);
}

TEST_F(ReflectionSystemTest, ProcessingWorks)
{
    // Set parameter values
    node->SetParameterValue(OLO_IDENTIFIER("InputA"), 5.0f);
    node->SetParameterValue(OLO_IDENTIFIER("InputB"), 3.0f);
    
    // Process (this should compute the result)
    float* dummyInputs[2] = {};
    float* dummyOutputs[2] = {};
    node->Process(dummyInputs, dummyOutputs, 1);
    
    // Check that the result was computed correctly
    EXPECT_FLOAT_EQ(node->out_Result, 8.0f);
}

TEST_F(ReflectionSystemTest, ReflectionIntrospectionWorks)
{
    // Test that we can use the reflection system for introspection
    using InputProvider = Provider<AddNodeReflectedF32, TagInputs>;
    using OutputProvider = Provider<AddNodeReflectedF32, TagOutputs>;
    
    // Check member count
    EXPECT_EQ(InputProvider::NumberOfMembers, 2);
    EXPECT_EQ(OutputProvider::NumberOfMembers, 1);
    
    // Check member name lookup
    EXPECT_EQ(InputProvider::IndexOf("InputA"), 0);
    EXPECT_EQ(InputProvider::IndexOf("InputB"), 1);
    EXPECT_EQ(InputProvider::IndexOf("NonExistent"), InputProvider::INVALID_INDEX);
    
    // Test getting member names by index
    EXPECT_EQ(InputProvider::GetMemberName(0), "InputA");
    EXPECT_EQ(InputProvider::GetMemberName(1), "InputB");
    EXPECT_FALSE(InputProvider::GetMemberName(999).has_value());
}

TEST_F(ReflectionSystemTest, RuntimeMemberAccess)
{
    // We should be able to access members by name at runtime using reflection
    using OutputProvider = Provider<AddNodeReflectedF32, TagOutputs>;
    
    // Set up the node with some computed result
    node->out_Result = 42.0f;
    
    // Get the value using reflection
    auto value = OutputProvider::GetMemberValueByName<f32>("Result", *node);
    ASSERT_TRUE(value.has_value());
    EXPECT_FLOAT_EQ(*value, 42.0f);
    
    // Test setting value using reflection (for outputs this may not be typical, but should work)
    bool success = OutputProvider::SetMemberValueByName("Result", 100.0f, *node);
    EXPECT_TRUE(success);
    EXPECT_FLOAT_EQ(node->out_Result, 100.0f);
}

TEST_F(ReflectionSystemTest, DebugInformationOutput)
{
    using InputProvider = Provider<AddNodeReflectedF32, TagInputs>;
    
    // This should not crash and should produce meaningful output
    std::ostringstream stream;
    InputProvider::PrintInfo(stream);
    
    std::string output = stream.str();
    EXPECT_TRUE(output.find("InputA") != std::string::npos);
    EXPECT_TRUE(output.find("InputB") != std::string::npos);
    EXPECT_TRUE(output.find("bytes") != std::string::npos);
}