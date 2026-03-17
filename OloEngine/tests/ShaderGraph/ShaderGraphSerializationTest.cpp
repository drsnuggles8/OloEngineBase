#include <gtest/gtest.h>
#include <set>
#include "OloEngine/Scene/Scene.h" // Required: AssetSerializer.h uses Ref<Scene> inline
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphAsset.h"
#include "OloEngine/Renderer/ShaderGraph/ShaderGraphSerializer.h"

using namespace OloEngine;

class ShaderGraphSerializationTest : public ::testing::Test
{
  protected:
    Ref<ShaderGraphAsset> CreateSampleGraph()
    {
        auto asset = Ref<ShaderGraphAsset>::Create();
        auto& graph = asset->GetMutableGraph();
        graph.SetName("TestMaterial");

        // Add a float parameter → Add → PBR Output
        auto floatNode = CreateShaderGraphNode(ShaderGraphNodeTypes::FloatParameter);
        floatNode->ParameterName = "u_Metallic";
        floatNode->Outputs[0].DefaultValue = 0.5f;
        UUID floatOut = floatNode->Outputs[0].ID;

        auto addNode = CreateShaderGraphNode(ShaderGraphNodeTypes::Add);
        UUID addInA = addNode->Inputs[0].ID;
        UUID addOut = addNode->Outputs[0].ID;

        auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::PBROutput);
        UUID metallicIn = outputNode->FindPinByName("Metallic", ShaderGraphPinDirection::Input)->ID;

        graph.AddNode(std::move(floatNode));
        graph.AddNode(std::move(addNode));
        graph.AddNode(std::move(outputNode));
        graph.AddLink(floatOut, addInA);
        graph.AddLink(addOut, metallicIn);

        return asset;
    }

    ShaderGraphSerializer serializer;
};

TEST_F(ShaderGraphSerializationTest, SerializeProducesNonEmptyYAML)
{
    auto asset = CreateSampleGraph();
    std::string yaml = serializer.TestSerializeToYAML(asset);
    EXPECT_FALSE(yaml.empty());
    EXPECT_NE(yaml.find("ShaderGraph"), std::string::npos);
}

TEST_F(ShaderGraphSerializationTest, RoundTripPreservesGraphName)
{
    auto original = CreateSampleGraph();
    std::string yaml = serializer.TestSerializeToYAML(original);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));
    EXPECT_EQ(deserialized->GetGraph().GetName(), "TestMaterial");
}

TEST_F(ShaderGraphSerializationTest, RoundTripPreservesNodeCount)
{
    auto original = CreateSampleGraph();
    std::string yaml = serializer.TestSerializeToYAML(original);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));
    EXPECT_EQ(deserialized->GetGraph().GetNodes().size(), original->GetGraph().GetNodes().size());
}

TEST_F(ShaderGraphSerializationTest, RoundTripPreservesLinkCount)
{
    auto original = CreateSampleGraph();
    std::string yaml = serializer.TestSerializeToYAML(original);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));
    EXPECT_EQ(deserialized->GetGraph().GetLinks().size(), original->GetGraph().GetLinks().size());
}

TEST_F(ShaderGraphSerializationTest, RoundTripPreservesParameterName)
{
    auto original = CreateSampleGraph();
    std::string yaml = serializer.TestSerializeToYAML(original);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));

    bool foundParam = false;
    for (const auto& node : deserialized->GetGraph().GetNodes())
    {
        if (node->TypeName == ShaderGraphNodeTypes::FloatParameter)
        {
            EXPECT_EQ(node->ParameterName, "u_Metallic");
            // Verify pin default value survives round-trip
            ASSERT_FALSE(node->Outputs.empty());
            auto* val = std::get_if<f32>(&node->Outputs[0].DefaultValue);
            ASSERT_NE(val, nullptr);
            EXPECT_FLOAT_EQ(*val, 0.5f);
            foundParam = true;
        }
    }
    EXPECT_TRUE(foundParam);
}

TEST_F(ShaderGraphSerializationTest, RoundTripPreservesNodeTypes)
{
    auto original = CreateSampleGraph();
    std::string yaml = serializer.TestSerializeToYAML(original);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));

    std::set<std::string> originalTypes, deserializedTypes;
    for (const auto& node : original->GetGraph().GetNodes())
        originalTypes.insert(node->TypeName);
    for (const auto& node : deserialized->GetGraph().GetNodes())
        deserializedTypes.insert(node->TypeName);

    EXPECT_EQ(originalTypes, deserializedTypes);
}

TEST_F(ShaderGraphSerializationTest, DeserializeInvalidYAMLReturnsFalse)
{
    auto asset = Ref<ShaderGraphAsset>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML("invalid yaml: [", asset));
}

TEST_F(ShaderGraphSerializationTest, DeserializeMissingRootNodeReturnsFalse)
{
    auto asset = Ref<ShaderGraphAsset>::Create();
    EXPECT_FALSE(serializer.TestDeserializeFromYAML("SomethingElse:\n  Key: Value", asset));
}

TEST_F(ShaderGraphSerializationTest, DeserializedGraphIsDirty)
{
    auto original = CreateSampleGraph();
    std::string yaml = serializer.TestSerializeToYAML(original);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));
    EXPECT_TRUE(deserialized->IsDirty());
}

TEST_F(ShaderGraphSerializationTest, DeserializedGraphCanCompile)
{
    auto original = CreateSampleGraph();
    std::string yaml = serializer.TestSerializeToYAML(original);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));

    const auto& result = deserialized->Compile();
    EXPECT_TRUE(result.Success) << result.ErrorLog;
}

// ── Compute Serialization ──

TEST_F(ShaderGraphSerializationTest, RoundTripPreservesComputeWorkgroupSize)
{
    auto asset = Ref<ShaderGraphAsset>::Create();
    auto& graph = asset->GetMutableGraph();
    graph.SetName("ComputeGraph");

    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput);
    outputNode->WorkgroupSize = glm::ivec3(64, 2, 1);
    UUID id = outputNode->ID;
    graph.AddNode(std::move(outputNode));

    std::string yaml = serializer.TestSerializeToYAML(asset);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));

    const auto* node = deserialized->GetGraph().FindNode(id);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->WorkgroupSize, glm::ivec3(64, 2, 1));
}

TEST_F(ShaderGraphSerializationTest, RoundTripPreservesBufferBinding)
{
    auto asset = Ref<ShaderGraphAsset>::Create();
    auto& graph = asset->GetMutableGraph();
    graph.SetName("ComputeGraph");

    auto bufNode = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeBufferInput);
    bufNode->BufferBinding = 3;
    bufNode->ParameterName = "myBuffer";
    UUID id = bufNode->ID;

    auto outputNode = CreateShaderGraphNode(ShaderGraphNodeTypes::ComputeOutput);

    graph.AddNode(std::move(bufNode));
    graph.AddNode(std::move(outputNode));

    std::string yaml = serializer.TestSerializeToYAML(asset);

    auto deserialized = Ref<ShaderGraphAsset>::Create();
    ASSERT_TRUE(serializer.TestDeserializeFromYAML(yaml, deserialized));

    const auto* node = deserialized->GetGraph().FindNode(id);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->BufferBinding, 3);
    EXPECT_EQ(node->ParameterName, "myBuffer");
}
