#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpEditorDebugDraw.h"
#include "MCP/McpServer.h"
#include "MCP/McpTokenNormalization.h"

#include <set>

// OLO_TEST_LAYER: unit

namespace
{
    namespace DebugDraw = OloEngine::MCP::EditorDebugDraw;
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpEditorDebugDrawSetResult;
    using OloEngine::MCP::McpEditorDebugDrawState;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    Json Call(const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "tools/call" }, { "params", { { "name", "olo_editor_debug_draw_set" }, { "arguments", arguments } } } };
    }
} // namespace

TEST(McpEditorDebugDraw, CategoriesAreUniqueAndNormalizationIsFriendly)
{
    std::set<std::string> categories;
    for (const auto category : DebugDraw::kCategories)
    {
        EXPECT_TRUE(categories.insert(OloEngine::MCP::NormalizeToken(category)).second) << category;
        EXPECT_EQ(DebugDraw::CanonicalCategory(category), category);
    }
    EXPECT_EQ(DebugDraw::CanonicalCategory("Physics Colliders"), "physics_colliders");
    EXPECT_EQ(DebugDraw::CanonicalCategory("selection-outline"), "selection_outline");
    EXPECT_TRUE(DebugDraw::CanonicalCategory("not_a_category").empty());
}

TEST(McpEditorDebugDraw, SchemaEnumeratesEveryCategoryAndIsClosed)
{
    const Json schema = DebugDraw::SetInputSchema();
    EXPECT_EQ(schema["additionalProperties"], false);
    EXPECT_EQ(schema["required"].size(), 2u);
    EXPECT_EQ(schema["properties"]["category"]["enum"].size(), DebugDraw::kCategories.size());
    EXPECT_TRUE(schema["properties"].contains("enabled"));
}

TEST(McpEditorDebugDraw, StateAndResultShapingCarryEveryCategory)
{
    McpEditorDebugDrawSetResult result;
    result.Available = true;
    result.Ok = true;
    result.Changed = true;
    result.Category = "all";
    result.Enabled = false;
    result.State = McpEditorDebugDrawState{};
    result.State.All = false;

    const Json json = DebugDraw::ToJson(result);
    EXPECT_FALSE(json["state"]["all"]);
    for (const auto category : DebugDraw::kCategories)
        EXPECT_TRUE(json["state"].contains(category)) << category;
}

TEST(McpEditorDebugDraw, ProjectWriteGateRejectsBeforeCallbackAndAllowsAfterConsent)
{
    int calls = 0;
    McpServer server(EditorMcpContext{});
    ToolDef tool;
    tool.Name = "olo_editor_debug_draw_set";
    tool.ProjectWrite = true;
    tool.InputSchema = DebugDraw::SetInputSchema();
    tool.Handler = [&calls](McpServer&, const Json& args)
    {
        ++calls;
        McpEditorDebugDrawSetResult result;
        result.Available = true;
        result.Ok = true;
        result.Changed = true;
        result.Category = args.at("category").get<std::string>();
        result.Enabled = args.at("enabled").get<bool>();
        result.State.All = result.Enabled;
        return ToolResult::Structured(DebugDraw::ToJson(result));
    };
    server.RegisterTool(std::move(tool));

    const Json request = Call(Json{ { "category", "all" }, { "enabled", false } });
    const Json denied = server.HandleMessage(request);
    EXPECT_TRUE(denied.contains("error"));
    EXPECT_FALSE(denied.contains("result"));
    EXPECT_EQ(calls, 0);

    server.SetAllowWrites(true);
    const Json response = server.HandleMessage(request);
    ASSERT_TRUE(response.contains("result"));
    EXPECT_FALSE(response["result"]["isError"]);
    EXPECT_EQ(calls, 1);
}

TEST(McpEditorDebugDraw, SchemaRejectsUnknownCategoryBeforeCallback)
{
    int calls = 0;
    McpServer server(EditorMcpContext{});
    ToolDef tool;
    tool.Name = "olo_editor_debug_draw_set";
    tool.ProjectWrite = true;
    tool.InputSchema = DebugDraw::SetInputSchema();
    tool.Handler = [&calls](McpServer&, const Json&)
    {
        ++calls;
        return ToolResult::Text("unexpected");
    };
    server.RegisterTool(std::move(tool));
    server.SetAllowWrites(true);

    const Json response = server.HandleMessage(Call(Json{ { "category", "fog" }, { "enabled", false } }));
    EXPECT_TRUE(response["result"]["isError"]);
    EXPECT_EQ(calls, 0);
}
