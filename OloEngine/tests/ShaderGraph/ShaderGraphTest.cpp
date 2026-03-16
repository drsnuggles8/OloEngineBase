#include <gtest/gtest.h>
#include "OloEngine/Renderer/ShaderGraph/ShaderGraph.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphNode.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphTypes.h"

using namespace OloEngine;

class ShaderGraphTest : public ::testing::Test
{
  protected:
    ShaderGraph graph;
};

// ── Node Management ──

TEST_F(ShaderGraphTest, AddNodeAndFindIt)
{
    auto node = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    ASSERT_NE(node, nullptr);
    UUID id = node->ID;
    graph.AddNode(std::move(node));

    EXPECT_NE(graph.FindNode(id), nullptr);
    EXPECT_EQ(graph.GetNodes().size(), 1u);
}

TEST_F(ShaderGraphTest, RemoveNodeCleansUpLinks)
{
    auto floatNode = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto addNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    UUID floatID = floatNode->ID;
    UUID floatOutPinID = floatNode->Outputs[0].ID;
    UUID addInPinID = addNode->Inputs[0].ID;

    graph.AddNode(std::move(floatNode));
    graph.AddNode(std::move(addNode));
    graph.AddLink(floatOutPinID, addInPinID);
    EXPECT_EQ(graph.GetLinks().size(), 1u);

    graph.RemoveNode(floatID);
    EXPECT_EQ(graph.GetLinks().size(), 0u);
    EXPECT_EQ(graph.FindNode(floatID), nullptr);
}

TEST_F(ShaderGraphTest, FindPinAcrossNodes)
{
    auto node = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    UUID inputPinID = node->Inputs[0].ID;
    UUID outputPinID = node->Outputs[0].ID;
    graph.AddNode(std::move(node));

    EXPECT_NE(graph.FindPin(inputPinID), nullptr);
    EXPECT_NE(graph.FindPin(outputPinID), nullptr);
    EXPECT_EQ(graph.FindPin(UUID(999999)), nullptr);
}

// ── Link Management ──

TEST_F(ShaderGraphTest, AddLinkConnectsCompatiblePins)
{
    auto floatNode = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto addNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    UUID outPin = floatNode->Outputs[0].ID;
    UUID inPin = addNode->Inputs[0].ID;

    graph.AddNode(std::move(floatNode));
    graph.AddNode(std::move(addNode));

    auto* link = graph.AddLink(outPin, inPin);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->OutputPinID, outPin);
    EXPECT_EQ(link->InputPinID, inPin);
}

TEST_F(ShaderGraphTest, AddLinkRejectsIncompatibleTypes)
{
    auto texNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Texture2DParameter);
    auto addNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    UUID texOut = texNode->Outputs[0].ID; // Texture2D type
    UUID addIn = addNode->Inputs[0].ID;   // Float type

    graph.AddNode(std::move(texNode));
    graph.AddNode(std::move(addNode));

    EXPECT_EQ(graph.AddLink(texOut, addIn), nullptr);
}

TEST_F(ShaderGraphTest, InputPinCanOnlyHaveOneLink)
{
    auto float1 = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto float2 = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto addNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    UUID out1 = float1->Outputs[0].ID;
    UUID out2 = float2->Outputs[0].ID;
    UUID inA = addNode->Inputs[0].ID;

    graph.AddNode(std::move(float1));
    graph.AddNode(std::move(float2));
    graph.AddNode(std::move(addNode));

    auto* link1 = graph.AddLink(out1, inA);
    ASSERT_NE(link1, nullptr);

    // Second link to same input replaces the first
    auto* link2 = graph.AddLink(out2, inA);
    ASSERT_NE(link2, nullptr);
    EXPECT_EQ(graph.GetLinks().size(), 1u);
    EXPECT_EQ(graph.GetLinkForInputPin(inA)->OutputPinID, out2);
}

// ── Cycle Detection ──

TEST_F(ShaderGraphTest, WouldCreateCycleDetectsCycle)
{
    auto nodeA = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    auto nodeB = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    UUID aOut = nodeA->Outputs[0].ID;
    UUID aInA = nodeA->Inputs[0].ID;
    UUID bOut = nodeB->Outputs[0].ID;
    UUID bInA = nodeB->Inputs[0].ID;

    graph.AddNode(std::move(nodeA));
    graph.AddNode(std::move(nodeB));

    // A → B
    graph.AddLink(aOut, bInA);

    // B → A would create cycle
    EXPECT_TRUE(graph.WouldCreateCycle(bOut, aInA));
}

TEST_F(ShaderGraphTest, WouldCreateCycleAllowsValidLink)
{
    auto nodeA = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto nodeB = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    UUID aOut = nodeA->Outputs[0].ID;
    UUID bInA = nodeB->Inputs[0].ID;

    graph.AddNode(std::move(nodeA));
    graph.AddNode(std::move(nodeB));

    EXPECT_FALSE(graph.WouldCreateCycle(aOut, bInA));
}

// ── Validation ──

TEST_F(ShaderGraphTest, ValidateEmptyGraphIsInvalid)
{
    auto result = graph.Validate();
    EXPECT_FALSE(result.IsValid);
    EXPECT_FALSE(result.Errors.empty());
}

TEST_F(ShaderGraphTest, ValidateGraphWithOutputNodeIsValid)
{
    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
    graph.AddNode(std::move(outputNode));

    auto result = graph.Validate();
    EXPECT_TRUE(result.IsValid);
}

TEST_F(ShaderGraphTest, ValidateGraphWithMultipleOutputsIsInvalid)
{
    graph.AddNode(CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput));
    graph.AddNode(CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput));

    auto result = graph.Validate();
    EXPECT_FALSE(result.IsValid);
}

// ── Topological Sort ──

TEST_F(ShaderGraphTest, TopologicalOrderPutsOutputLast)
{
    auto floatNode = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
    auto addNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);

    UUID floatOut = floatNode->Outputs[0].ID;
    UUID addInA = addNode->Inputs[0].ID;
    UUID addOut = addNode->Outputs[0].ID;
    UUID outputMetallic = outputNode->FindPinByName("Metallic", ShaderGraphPinDirection::Input)->ID;

    graph.AddNode(std::move(floatNode));
    graph.AddNode(std::move(addNode));
    graph.AddNode(std::move(outputNode));

    graph.AddLink(floatOut, addInA);
    graph.AddLink(addOut, outputMetallic);

    auto sorted = graph.GetTopologicalOrder();
    ASSERT_FALSE(sorted.empty());
    EXPECT_EQ(sorted.back()->TypeName, ShaderGraphNodeTypes::PBROutput);
}

// ── Node Factory ──

TEST_F(ShaderGraphTest, NodeFactoryCreatesAllRegisteredTypes)
{
    auto allTypes = GetAllNodeTypeNames();
    EXPECT_FALSE(allTypes.empty());

    for (const auto& typeName : allTypes)
    {
        auto node = CreateShaderGraphNode(typeName);
        ASSERT_NE(node, nullptr) << "Failed to create node: " << typeName;
        EXPECT_EQ(node->TypeName, typeName);
    }
}

TEST_F(ShaderGraphTest, NodeFactoryReturnsNullForUnknown)
{
    auto node = CreateShaderGraphNode("NonExistentNodeType");
    EXPECT_EQ(node, nullptr);
}

// ── Type Conversion ──

TEST(ShaderGraphTypeTest, FloatBroadcastsToVectors)
{
    EXPECT_TRUE(CanConvertPinType(ShaderGraphPinType::Float, ShaderGraphPinType::Vec2));
    EXPECT_TRUE(CanConvertPinType(ShaderGraphPinType::Float, ShaderGraphPinType::Vec3));
    EXPECT_TRUE(CanConvertPinType(ShaderGraphPinType::Float, ShaderGraphPinType::Vec4));
}

TEST(ShaderGraphTypeTest, Vec4TruncatesToVec3AndVec2)
{
    EXPECT_TRUE(CanConvertPinType(ShaderGraphPinType::Vec4, ShaderGraphPinType::Vec3));
    EXPECT_TRUE(CanConvertPinType(ShaderGraphPinType::Vec4, ShaderGraphPinType::Vec2));
    EXPECT_FALSE(CanConvertPinType(ShaderGraphPinType::Vec4, ShaderGraphPinType::Float));
}

TEST(ShaderGraphTypeTest, IncompatibleTypesCannotConvert)
{
    EXPECT_FALSE(CanConvertPinType(ShaderGraphPinType::Bool, ShaderGraphPinType::Float));
    EXPECT_FALSE(CanConvertPinType(ShaderGraphPinType::Texture2D, ShaderGraphPinType::Vec3));
}

TEST(ShaderGraphTypeTest, GenerateTypeConversionProducesValidGLSL)
{
    EXPECT_EQ(GenerateTypeConversion("x", ShaderGraphPinType::Float, ShaderGraphPinType::Vec3), "vec3(x)");
    EXPECT_EQ(GenerateTypeConversion("v", ShaderGraphPinType::Vec4, ShaderGraphPinType::Vec3), "v.xyz");
    EXPECT_EQ(GenerateTypeConversion("v", ShaderGraphPinType::Vec3, ShaderGraphPinType::Vec2), "v.xy");
    EXPECT_EQ(GenerateTypeConversion("x", ShaderGraphPinType::Float, ShaderGraphPinType::Float), "x");
}

// ── Compute Graph ──

TEST_F(ShaderGraphTest, ComputeOutputNodeCreation)
{
    auto node = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->TypeName, ShaderGraphNodeTypes::ComputeOutput);
    EXPECT_EQ(node->Category, ShaderGraphNodeCategory::Output);
    EXPECT_EQ(node->WorkgroupSize, glm::ivec3(16, 16, 1));
}

TEST_F(ShaderGraphTest, ComputeBufferNodeCreation)
{
    auto input = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeBufferInput);
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->Category, ShaderGraphNodeCategory::Compute);
    EXPECT_EQ(input->BufferBinding, 0);
    EXPECT_FALSE(input->Outputs.empty());

    auto store = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeBufferStore);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->Category, ShaderGraphNodeCategory::Compute);
    EXPECT_EQ(store->BufferBinding, 1);
    EXPECT_FALSE(store->Inputs.empty());
}

TEST_F(ShaderGraphTest, ComputeInvocationIDNodes)
{
    auto global = CreateShaderGraphNode(ShaderGraphNodeTypes::GlobalInvocationID);
    ASSERT_NE(global, nullptr);
    EXPECT_EQ(global->Category, ShaderGraphNodeCategory::Compute);
    EXPECT_FALSE(global->Outputs.empty());
    EXPECT_EQ(global->Outputs[0].Type, ShaderGraphPinType::Vec3);

    auto local = CreateShaderGraphNode(ShaderGraphNodeTypes::LocalInvocationID);
    ASSERT_NE(local, nullptr);

    auto workgroup = CreateShaderGraphNode(ShaderGraphNodeTypes::WorkgroupID);
    ASSERT_NE(workgroup, nullptr);
}

TEST_F(ShaderGraphTest, ValidateComputeGraphIsValid)
{
    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput);
    graph.AddNode(std::move(outputNode));

    auto result = graph.Validate();
    EXPECT_TRUE(result.IsValid);
}

TEST_F(ShaderGraphTest, ValidateMixedOutputNodesIsInvalid)
{
    graph.AddNode(CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput));
    graph.AddNode(CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput));

    auto result = graph.Validate();
    EXPECT_FALSE(result.IsValid);
}

TEST_F(ShaderGraphTest, FindOutputNodeReturnsComputeOutput)
{
    auto node = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput);
    UUID id = node->ID;
    graph.AddNode(std::move(node));

    const auto* found = graph.FindOutputNode();
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->ID, id);
    EXPECT_EQ(found->TypeName, ShaderGraphNodeTypes::ComputeOutput);
}
