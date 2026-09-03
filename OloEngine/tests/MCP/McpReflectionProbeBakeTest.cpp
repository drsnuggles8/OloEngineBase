// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpReflectionProbeBake.h"

namespace
{
    namespace Bake = OloEngine::MCP::ReflectionProbeBake;
    using Json = Bake::Json;
} // namespace

TEST(McpReflectionProbeBake, RequiresNonEmptyEntityName)
{
    std::string name;
    EXPECT_TRUE(Bake::ParseEntityName(Json::object(), name).has_value());
    EXPECT_TRUE(Bake::ParseEntityName(Json{ { "entity", 42 } }, name).has_value());
    EXPECT_TRUE(Bake::ParseEntityName(Json{ { "entity", "" } }, name).has_value());
    EXPECT_FALSE(Bake::ParseEntityName(Json{ { "entity", "Hall Probe" } }, name).has_value());
    EXPECT_EQ(name, "Hall Probe");
}

TEST(McpReflectionProbeBake, InputSchemaRequiresOnlyEntity)
{
    const Json schema = Bake::InputSchema();
    EXPECT_EQ(schema["type"], "object");
    EXPECT_EQ(schema["required"], Json::array({ "entity" }));
    EXPECT_EQ(schema["properties"]["entity"]["type"], "string");
    EXPECT_EQ(schema["additionalProperties"], false);
}

TEST(McpReflectionProbeBake, ResultDoesNotHideBakeFailure)
{
    const Json result = Bake::Result("Hall Probe", false, "Bake failed");
    EXPECT_FALSE(result["baked"].get<bool>());
    EXPECT_EQ(result["entity"], "Hall Probe");
    EXPECT_EQ(result["message"], "Bake failed");
}
