#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Tier-2 P2 of issue #357: structured/typed tool output — an optional `outputSchema`
// on tool definitions and `structuredContent` in tools/call results, so agent clients
// can parse a tool's output as typed data instead of scraping the text blob. Like the
// annotations test, this drives the httplib-free dispatch seam (McpServer.cpp is
// compiled into the test binary) with fake tools that mirror the shapes the real
// builders emit (McpTools.cpp's converted tools set OutputSchema + return
// ToolResult::Structured). No live editor, GPU, or agent required. The real
// RegisterBuiltinTools pulls the whole renderer/physics stack, so it is intentionally
// NOT linked here; the per-tool conversion is exercised live over MCP.
#include "MCP/McpServer.h"

#include <string>
#include <utility>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    Json MakeRequest(const Json& id, const std::string& method, const Json& params = Json::object())
    {
        Json req = { { "jsonrpc", "2.0" }, { "method", method } };
        if (!id.is_null())
            req["id"] = id;
        if (!params.is_null())
            req["params"] = params;
        return req;
    }

    // A representative structured payload: an object with a nested path string (to
    // exercise redaction) and a couple of scalar fields.
    Json SamplePayload()
    {
        return Json{ { "count", 3 }, { "name", "Cube" }, { "asset", "C:\\Projects\\Game\\mesh.obj" } };
    }

    // The output schema the structured tool advertises (shape only; the dispatcher
    // does not validate against it — clients may).
    Json SampleOutputSchema()
    {
        return Json{ { "type", "object" },
                     { "properties",
                       { { "count", { { "type", "integer" } } },
                         { "name", { { "type", "string" } } },
                         { "asset", { { "type", "string" } } } } },
                     { "required", Json::array({ "count", "name" }) } };
    }

    ToolResult StructuredHandler(McpServer&, const Json&)
    {
        return ToolResult::Structured(SamplePayload());
    }

    ToolResult TextHandler(McpServer&, const Json&)
    {
        return ToolResult::Text("just text");
    }

    // Register a tool with an OutputSchema + structured handler.
    void AddStructuredTool(McpServer& server, std::string name)
    {
        ToolDef tool;
        tool.Name = std::move(name);
        tool.Description = "A structured tool.";
        tool.OutputSchema = SampleOutputSchema();
        tool.Handler = StructuredHandler;
        server.RegisterTool(std::move(tool));
    }

    // Register a plain text tool (no OutputSchema, no structuredContent).
    void AddTextTool(McpServer& server, std::string name)
    {
        ToolDef tool;
        tool.Name = std::move(name);
        tool.Description = "A text tool.";
        tool.Handler = TextHandler;
        server.RegisterTool(std::move(tool));
    }

    const Json* FindToolEntry(const Json& response, const std::string& name)
    {
        if (!response.contains("result") || !response["result"].contains("tools"))
            return nullptr;
        for (const auto& tool : response["result"]["tools"])
        {
            if (tool.value("name", std::string{}) == name)
                return &tool;
        }
        return nullptr;
    }

    Json CallTool(McpServer& server, const Json& id, const std::string& name)
    {
        return server.HandleMessage(MakeRequest(id, "tools/call", Json{ { "name", name } }));
    }
} // namespace

// ---- ToolResult::Structured factory ----------------------------------------

TEST(McpStructuredOutput, StructuredFactoryMirrorsDataIntoTextAndSetsStructuredContent)
{
    const Json data = SamplePayload();
    const ToolResult r = ToolResult::Structured(data);

    EXPECT_FALSE(r.IsError);
    // StructuredContent is exactly the payload.
    EXPECT_EQ(r.StructuredContent, data);
    // content is a single text block whose text is the pretty-printed mirror.
    ASSERT_TRUE(r.Content.is_array());
    ASSERT_EQ(r.Content.size(), 1u);
    EXPECT_EQ(r.Content[0].value("type", std::string{}), "text");
    EXPECT_EQ(r.Content[0].value("text", std::string{}), data.dump(2));
}

TEST(McpStructuredOutput, TextAndErrorFactoriesLeaveStructuredContentNull)
{
    EXPECT_TRUE(ToolResult::Text("hi").StructuredContent.is_null());
    EXPECT_TRUE(ToolResult::Error("boom").StructuredContent.is_null());
}

// ---- tools/list: outputSchema serialization --------------------------------

TEST(McpStructuredOutput, ToolsListEmitsOutputSchemaWhenPresent)
{
    McpServer server(EditorMcpContext{});
    AddStructuredTool(server, "olo_fake_structured");

    const Json resp = server.HandleMessage(MakeRequest(1, "tools/list"));
    const Json* entry = FindToolEntry(resp, "olo_fake_structured");
    ASSERT_NE(entry, nullptr);

    ASSERT_TRUE(entry->contains("outputSchema"));
    EXPECT_EQ((*entry)["outputSchema"], SampleOutputSchema());
    // inputSchema is still emitted (defaulted) — outputSchema is additive.
    EXPECT_TRUE(entry->contains("inputSchema"));
}

TEST(McpStructuredOutput, ToolsListOmitsOutputSchemaForTextTool)
{
    McpServer server(EditorMcpContext{});
    AddTextTool(server, "olo_fake_text");

    const Json resp = server.HandleMessage(MakeRequest(2, "tools/list"));
    const Json* entry = FindToolEntry(resp, "olo_fake_text");
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->contains("outputSchema"));
}

TEST(McpStructuredOutput, ToolsListOmitsOutputSchemaWhenEmptyObject)
{
    McpServer server(EditorMcpContext{});
    // An explicitly empty object is "no schema" — must be omitted, not serialized
    // as "outputSchema": {} (mirrors the annotations omit-when-empty rule).
    ToolDef tool;
    tool.Name = "olo_fake_emptyschema";
    tool.Description = "A tool.";
    tool.OutputSchema = Json::object();
    tool.Handler = TextHandler;
    server.RegisterTool(std::move(tool));

    const Json resp = server.HandleMessage(MakeRequest(3, "tools/list"));
    const Json* entry = FindToolEntry(resp, "olo_fake_emptyschema");
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->contains("outputSchema"));
}

// ---- tools/call: structuredContent alongside the text mirror ---------------

TEST(McpStructuredOutput, ToolsCallReturnsStructuredContentAndTextMirror)
{
    McpServer server(EditorMcpContext{});
    AddStructuredTool(server, "olo_fake_structured");

    const Json resp = CallTool(server, 4, "olo_fake_structured");
    ASSERT_TRUE(resp.contains("result"));
    const Json& result = resp["result"];

    // Typed result is the payload, verbatim.
    ASSERT_TRUE(result.contains("structuredContent"));
    EXPECT_EQ(result["structuredContent"], SamplePayload());

    // Back-compat text mirror is still present for clients that don't parse it.
    ASSERT_TRUE(result.contains("content"));
    ASSERT_TRUE(result["content"].is_array());
    ASSERT_EQ(result["content"].size(), 1u);
    EXPECT_EQ(result["content"][0].value("type", std::string{}), "text");
    EXPECT_EQ(result["content"][0].value("text", std::string{}), SamplePayload().dump(2));

    EXPECT_EQ(result.value("isError", true), false);
}

TEST(McpStructuredOutput, ToolsCallOmitsStructuredContentForTextTool)
{
    McpServer server(EditorMcpContext{});
    AddTextTool(server, "olo_fake_text");

    const Json resp = CallTool(server, 5, "olo_fake_text");
    ASSERT_TRUE(resp.contains("result"));
    const Json& result = resp["result"];

    EXPECT_FALSE(result.contains("structuredContent"));
    // The text path is unchanged: content + isError only.
    EXPECT_TRUE(result.contains("content"));
    EXPECT_EQ(result.value("isError", true), false);
}

// A tool that throws is surfaced as an isError text result with no structuredContent
// (an error is not validated against outputSchema).
TEST(McpStructuredOutput, ToolsCallErrorHasNoStructuredContent)
{
    McpServer server(EditorMcpContext{});
    ToolDef tool;
    tool.Name = "olo_fake_throws";
    tool.Description = "Throws.";
    tool.OutputSchema = SampleOutputSchema();
    tool.Handler = [](McpServer&, const Json&) -> ToolResult
    { throw std::runtime_error("nope"); };
    server.RegisterTool(std::move(tool));

    const Json resp = CallTool(server, 6, "olo_fake_throws");
    ASSERT_TRUE(resp.contains("result"));
    const Json& result = resp["result"];
    EXPECT_EQ(result.value("isError", false), true);
    EXPECT_FALSE(result.contains("structuredContent"));
}

// ---- redaction reaches structuredContent -----------------------------------

TEST(McpStructuredOutput, RedactionScrubsPathsInStructuredContent)
{
    McpServer server(EditorMcpContext{});
    AddStructuredTool(server, "olo_fake_structured");
    server.SetRedactPaths(true);

    const Json resp = CallTool(server, 7, "olo_fake_structured");
    ASSERT_TRUE(resp.contains("result"));
    const Json& result = resp["result"];

    // The absolute path leaf is scrubbed in the structured object, not just the text.
    ASSERT_TRUE(result.contains("structuredContent"));
    EXPECT_EQ(result["structuredContent"].value("asset", std::string{}), "<path>");
    // Non-path fields pass through untouched.
    EXPECT_EQ(result["structuredContent"].value("name", std::string{}), "Cube");
    EXPECT_EQ(result["structuredContent"].value("count", 0), 3);

    // The text mirror is also redacted (no raw drive path leaks).
    const std::string text = result["content"][0].value("text", std::string{});
    EXPECT_EQ(text.find("C:\\"), std::string::npos);
    EXPECT_NE(text.find("<path>"), std::string::npos);
}

TEST(McpStructuredOutput, RedactionOffLeavesStructuredContentIntact)
{
    McpServer server(EditorMcpContext{});
    AddStructuredTool(server, "olo_fake_structured");
    // Redaction defaults off.

    const Json resp = CallTool(server, 8, "olo_fake_structured");
    ASSERT_TRUE(resp["result"].contains("structuredContent"));
    EXPECT_EQ(resp["result"]["structuredContent"], SamplePayload());
}
