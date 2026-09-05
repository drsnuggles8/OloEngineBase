#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpEditorPanels.h"
#include "MCP/McpServer.h"
#include "MCP/McpTokenNormalization.h"

#include <set>

// OLO_TEST_LAYER: unit

namespace
{
    namespace Panels = OloEngine::MCP::EditorPanels;
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpEditorPanelSetResult;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    Json Call(const std::string& name, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "tools/call" }, { "params", { { "name", name }, { "arguments", arguments } } } };
    }
} // namespace

TEST(McpEditorPanels, CatalogueIsCompleteUniqueAndFindable)
{
    // Bump alongside kPanels whenever a panel is added (issue #646 added the
    // Tilemap Painter, 34 -> 35). The count is asserted so a panel added to the
    // PanelId enum but forgotten in kPanels fails here rather than silently
    // becoming unaddressable over MCP.
    EXPECT_EQ(Panels::kPanels.size(), 35u);
    std::set<std::string> names;
    for (const auto& panel : Panels::kPanels)
    {
        EXPECT_FALSE(panel.Name.empty());
        EXPECT_FALSE(panel.Title.empty());
        EXPECT_TRUE(names.insert(OloEngine::MCP::NormalizeToken(panel.Name)).second) << panel.Name;
        EXPECT_EQ(Panels::Find(panel.Name), &panel);
        EXPECT_EQ(Panels::Find(panel.Title), &panel);
    }
    EXPECT_EQ(Panels::Find("auto_save_recovery"), nullptr);
    EXPECT_EQ(Panels::Find("Visual-Script Editor"), Panels::Find("visual_script_editor"));
}

TEST(McpEditorPanels, SchemasAreClosedAndSetRequiresBothArguments)
{
    const Json list = Panels::ListInputSchema();
    EXPECT_EQ(list["additionalProperties"], false);

    const Json set = Panels::SetInputSchema();
    EXPECT_EQ(set["additionalProperties"], false);
    EXPECT_TRUE(set["properties"].contains("panel"));
    EXPECT_TRUE(set["properties"].contains("open"));
    EXPECT_EQ(set["required"].size(), 2u);
}

TEST(McpEditorPanels, ResultShapingCarriesLivePanelState)
{
    McpEditorPanelSetResult result;
    result.Available = true;
    result.Ok = true;
    result.Changed = true;
    result.Panel = { "visual_script_editor", "Visual Script Editor", true };
    result.Message = "Opened Visual Script Editor.";
    const Json json = Panels::ToJson(result);
    EXPECT_TRUE(json["available"]);
    EXPECT_TRUE(json["ok"]);
    EXPECT_TRUE(json["changed"]);
    EXPECT_EQ(json["panel"]["name"], "visual_script_editor");
    EXPECT_TRUE(json["panel"]["open"]);
}

TEST(McpEditorPanels, ProjectWriteGateRejectsBeforeCallbackAndAllowsAfterConsent)
{
    int calls = 0;
    McpServer server(EditorMcpContext{});
    ToolDef tool;
    tool.Name = "olo_editor_panel_set";
    tool.ProjectWrite = true;
    tool.InputSchema = Panels::SetInputSchema();
    tool.Handler = [&calls](McpServer&, const Json& args)
    {
        ++calls;
        McpEditorPanelSetResult result;
        result.Available = true;
        result.Ok = true;
        result.Changed = true;
        result.Panel = { args.at("panel").get<std::string>(), "Panel", args.at("open").get<bool>() };
        return ToolResult::Structured(Panels::ToJson(result));
    };
    server.RegisterTool(std::move(tool));

    const Json request = Call("olo_editor_panel_set", Json{ { "panel", "console" }, { "open", false } });
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

TEST(McpEditorPanels, SchemaRejectsUnknownPropertiesBeforeCallback)
{
    int calls = 0;
    McpServer server(EditorMcpContext{});
    ToolDef tool;
    tool.Name = "olo_editor_panel_set";
    tool.ProjectWrite = true;
    tool.InputSchema = Panels::SetInputSchema();
    tool.Handler = [&calls](McpServer&, const Json&)
    {
        ++calls;
        return ToolResult::Text("unexpected");
    };
    server.RegisterTool(std::move(tool));
    server.SetAllowWrites(true);

    const Json response = server.HandleMessage(Call("olo_editor_panel_set",
                                                    Json{ { "panel", "console" }, { "open", true }, { "extra", 1 } }));
    EXPECT_TRUE(response["result"]["isError"]);
    EXPECT_EQ(calls, 0);
}
