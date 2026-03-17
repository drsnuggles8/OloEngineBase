#include <gtest/gtest.h>
#include "OloEngine/Renderer/ShaderGraph/ShaderGraph.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphCompiler.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"

using namespace OloEngine;

class ShaderGraphCompilerTest : public ::testing::Test
{
  protected:
    ShaderGraph graph;
    ShaderGraphCompiler compiler;

    /// Helper: create a minimal valid graph (just PBROutput with defaults)
    void BuildMinimalGraph()
    {
        graph.AddNode(CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput));
    }

    /// Helper: build a graph with a float parameter feeding PBR metallic
    void BuildFloatParameterGraph()
    {
        auto floatNode = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
        floatNode->ParameterName = "u_Metallic";
        floatNode->Outputs[0].DefaultValue = 0.5f;
        UUID floatOut = floatNode->Outputs[0].ID;

        auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
        UUID metallicIn = outputNode->FindPinByName("Metallic", ShaderGraphPinDirection::Input)->ID;

        graph.AddNode(std::move(floatNode));
        graph.AddNode(std::move(outputNode));
        graph.AddLink(floatOut, metallicIn);
    }
};

TEST_F(ShaderGraphCompilerTest, EmptyGraphFailsCompilation)
{
    auto result = compiler.Compile(graph);
    EXPECT_FALSE(result.Success);
    EXPECT_FALSE(result.ErrorLog.empty());
}

TEST_F(ShaderGraphCompilerTest, MinimalGraphCompiles)
{
    BuildMinimalGraph();
    auto result = compiler.Compile(graph);
    EXPECT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_FALSE(result.ShaderSource.empty());
}

TEST_F(ShaderGraphCompilerTest, OutputContainsVertexAndFragmentSections)
{
    BuildMinimalGraph();
    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_NE(result.ShaderSource.find("#type vertex"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("#type fragment"), std::string::npos);
}

TEST_F(ShaderGraphCompilerTest, OutputContainsMRTLayout)
{
    BuildMinimalGraph();
    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_NE(result.ShaderSource.find("o_Color"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("o_EntityID"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("o_ViewNormal"), std::string::npos);
}

TEST_F(ShaderGraphCompilerTest, FloatParameterExposed)
{
    BuildFloatParameterGraph();
    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_FALSE(result.ExposedParameters.empty());

    bool foundMetallic = false;
    for (const auto& param : result.ExposedParameters)
    {
        if (param.Name == "u_Metallic")
        {
            foundMetallic = true;
            EXPECT_EQ(param.Type, ShaderGraphPinType::Float);
        }
    }
    EXPECT_TRUE(foundMetallic);
}

TEST_F(ShaderGraphCompilerTest, UBOBlockGenerated)
{
    BuildFloatParameterGraph();
    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    // Scalar params should be in a UBO block (SPIR-V requirement)
    EXPECT_NE(result.ShaderSource.find("uniform ShaderGraphParams"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("u_Metallic"), std::string::npos);
}

TEST_F(ShaderGraphCompilerTest, MathNodeGeneratesCode)
{
    auto floatA = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    floatA->ParameterName = "u_A";
    UUID aOut = floatA->Outputs[0].ID;

    auto floatB = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    floatB->ParameterName = "u_B";
    UUID bOut = floatB->Outputs[0].ID;

    auto addNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    UUID addInA = addNode->Inputs[0].ID;
    UUID addInB = addNode->Inputs[1].ID;
    UUID addOut = addNode->Outputs[0].ID;

    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
    UUID metallicIn = outputNode->FindPinByName("Metallic", ShaderGraphPinDirection::Input)->ID;

    graph.AddNode(std::move(floatA));
    graph.AddNode(std::move(floatB));
    graph.AddNode(std::move(addNode));
    graph.AddNode(std::move(outputNode));
    graph.AddLink(aOut, addInA);
    graph.AddLink(bOut, addInB);
    graph.AddLink(addOut, metallicIn);

    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    // The generated code should contain an addition expression
    EXPECT_NE(result.ShaderSource.find("+"), std::string::npos);
}

TEST_F(ShaderGraphCompilerTest, TextureSamplerBindingGenerated)
{
    auto texNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Texture2DParameter);
    texNode->ParameterName = "u_AlbedoTex";
    UUID texOut = texNode->Outputs[0].ID;

    auto sampleNode = CreateShaderGraphNode(ShaderGraphNodeTypes::SampleTexture2D);
    UUID texIn = sampleNode->FindPinByName("Texture", ShaderGraphPinDirection::Input)->ID;
    UUID rgbOut = sampleNode->FindPinByName("RGB", ShaderGraphPinDirection::Output)->ID;

    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
    UUID albedoIn = outputNode->FindPinByName("Albedo", ShaderGraphPinDirection::Input)->ID;

    graph.AddNode(std::move(texNode));
    graph.AddNode(std::move(sampleNode));
    graph.AddNode(std::move(outputNode));
    graph.AddLink(texOut, texIn);
    graph.AddLink(rgbOut, albedoIn);

    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_NE(result.ShaderSource.find("sampler2D u_AlbedoTex"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("texture("), std::string::npos);
}

// ── Compute Shader Compilation ──

TEST_F(ShaderGraphCompilerTest, MinimalComputeGraphCompiles)
{
    graph.AddNode(CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput));

    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_TRUE(result.IsCompute);
    EXPECT_NE(result.ShaderSource.find("local_size_x"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("void main()"), std::string::npos);
}

TEST_F(ShaderGraphCompilerTest, ComputeGraphWithBuffersCompiles)
{
    auto bufInput = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeBufferInput);
    bufInput->ParameterName = "srcData";
    bufInput->BufferBinding = 0;
    UUID bufOut = bufInput->Outputs[0].ID;

    auto bufStore = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeBufferStore);
    bufStore->ParameterName = "dstData";
    bufStore->BufferBinding = 1;
    UUID storeIn = bufStore->Inputs[0].ID;

    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput);

    graph.AddNode(std::move(bufInput));
    graph.AddNode(std::move(bufStore));
    graph.AddNode(std::move(outputNode));
    graph.AddLink(bufOut, storeIn);

    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_TRUE(result.IsCompute);
    EXPECT_NE(result.ShaderSource.find("srcData"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("dstData"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("binding = 0"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("binding = 1"), std::string::npos);
}

TEST_F(ShaderGraphCompilerTest, ComputeGraphCustomWorkgroupSize)
{
    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput);
    outputNode->WorkgroupSize = glm::ivec3(64, 1, 1);
    graph.AddNode(std::move(outputNode));

    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_NE(result.ShaderSource.find("local_size_x = 64"), std::string::npos);
    EXPECT_NE(result.ShaderSource.find("local_size_y = 1"), std::string::npos);
}

TEST_F(ShaderGraphCompilerTest, ComputeGraphIsNotPBR)
{
    graph.AddNode(CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput));

    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_TRUE(result.IsCompute);
    EXPECT_EQ(result.ShaderSource.find("#type vertex"), std::string::npos);
    EXPECT_EQ(result.ShaderSource.find("#type fragment"), std::string::npos);
}

TEST_F(ShaderGraphCompilerTest, PBRGraphIsNotCompute)
{
    BuildMinimalGraph();
    auto result = compiler.Compile(graph);
    ASSERT_TRUE(result.Success) << result.ErrorLog;
    EXPECT_FALSE(result.IsCompute);
}
