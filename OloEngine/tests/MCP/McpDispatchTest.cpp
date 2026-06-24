#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// The MCP dispatch core (McpServer.cpp) is compiled into the test binary; this
// header only forward-declares httplib, so no socket / winsock types are pulled
// in here. We exercise the transport-agnostic seam added for issue #306 item D:
//   * HandleMessage(Json)        — one JSON-RPC message in, one response out
//   * ProcessRequestBody(string) — the framing layer (parse, batch, sessions)
//   * CheckBearerAuth / IsOriginAllowed — the security checks, as pure helpers
// No live OloEditor, GPU, or agent is required.
#include "MCP/McpServer.h"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::PromptDef;
    using OloEngine::MCP::ResourceDef;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    // The protocol version the server advertises when the client requests one it
    // does not recognise (see HandleInitialize's kSupported list).
    constexpr const char* kLatestProtocol = "2025-06-18";

    // Build a single JSON-RPC request object. Pass a null `id` to forge a
    // notification (no "id" key is added).
    Json MakeRequest(const Json& id, const std::string& method, const Json& params = Json::object())
    {
        Json req = { { "jsonrpc", "2.0" }, { "method", method } };
        if (!id.is_null())
            req["id"] = id;
        if (!params.is_null())
            req["params"] = params;
        return req;
    }

    // Return the first entry of a JSON tools array whose "name" equals `name`, else
    // nullptr. Used by the tools/list and tools/search assertions below.
    const Json* FindToolNamed(const Json& tools, const std::string& name)
    {
        for (const auto& tool : tools)
        {
            if (tool.contains("name") && tool["name"] == name)
                return &tool;
        }
        return nullptr;
    }

    // Fixture: an McpServer with a stub (empty) editor context plus a handful of
    // fake registrations, so dispatch can be exercised without the real 39 tools
    // / 2 resources / 3 prompts and without an editor backing the server.
    //
    // Toolsets for the tools/search tests: fake_echo and fake_boom are both in the
    // "diag" toolset; fake_tool_error is deliberately left uncategorized (empty
    // Toolset) to exercise the _meta-omission and catalogue-exclusion paths.
    class McpDispatchTest : public ::testing::Test
    {
      protected:
        McpDispatchTest()
            : m_Server(EditorMcpContext{})
        {
            ToolDef echo;
            echo.Name = "fake_echo";
            echo.Description = "Echo back the 'text' argument.";
            echo.Toolset = "diag";
            echo.InputSchema = Json{ { "type", "object" },
                                     { "properties", { { "text", { { "type", "string" } } } } } };
            echo.Handler = [](McpServer&, const Json& args)
            { return ToolResult::Text(args.value("text", std::string{})); };
            m_Server.RegisterTool(std::move(echo));

            // No InputSchema set -> tools/list must substitute a default object schema.
            ToolDef boom;
            boom.Name = "fake_boom";
            boom.Description = "Always throws.";
            boom.Toolset = "diag";
            boom.Handler = [](McpServer&, const Json&) -> ToolResult
            { throw std::runtime_error("kaboom"); };
            m_Server.RegisterTool(std::move(boom));

            ToolDef toolError;
            toolError.Name = "fake_tool_error";
            toolError.Description = "Returns a tool-level error (isError=true).";
            // Intentionally no Toolset (uncategorized).
            toolError.Handler = [](McpServer&, const Json&)
            { return ToolResult::Error("nope"); };
            m_Server.RegisterTool(std::move(toolError));

            ResourceDef resource;
            resource.Uri = "olo://fake";
            resource.Name = "fake-resource";
            resource.Description = "A fake resource.";
            resource.MimeType = "text/plain";
            resource.Reader = [](McpServer&)
            { return std::string("resource-body"); };
            m_Server.RegisterResource(std::move(resource));

            PromptDef prompt;
            prompt.Name = "fake-prompt";
            prompt.Title = "Fake Prompt";
            prompt.Description = "A fake prompt.";
            prompt.Text = "do the thing";
            m_Server.RegisterPrompt(std::move(prompt));
        }

        McpServer m_Server;
    };
} // namespace

// ---- initialize ------------------------------------------------------------

TEST_F(McpDispatchTest, InitializeReturnsHandshakeShape)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(1, "initialize", Json{ { "protocolVersion", kLatestProtocol } }));

    EXPECT_EQ(resp.value("jsonrpc", std::string{}), "2.0");
    EXPECT_EQ(resp["id"], 1);
    ASSERT_TRUE(resp.contains("result"));
    const Json& result = resp["result"];

    EXPECT_EQ(result["protocolVersion"], kLatestProtocol);
    EXPECT_EQ(result["serverInfo"]["name"], "OloEditor");
    EXPECT_TRUE(result["serverInfo"].contains("version"));
    EXPECT_TRUE(result.contains("instructions"));

    ASSERT_TRUE(result.contains("capabilities"));
    const Json& caps = result["capabilities"];
    EXPECT_TRUE(caps.contains("tools"));
    EXPECT_TRUE(caps.contains("resources"));
    EXPECT_TRUE(caps.contains("prompts"));
}

TEST_F(McpDispatchTest, InitializeEchoesSupportedProtocolVersion)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(1, "initialize", Json{ { "protocolVersion", "2024-11-05" } }));
    EXPECT_EQ(resp["result"]["protocolVersion"], "2024-11-05");
}

TEST_F(McpDispatchTest, InitializeFallsBackToLatestForUnknownVersion)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(1, "initialize", Json{ { "protocolVersion", "1999-01-01" } }));
    EXPECT_EQ(resp["result"]["protocolVersion"], kLatestProtocol);
}

// ---- ping ------------------------------------------------------------------

TEST_F(McpDispatchTest, PingReturnsEmptyResult)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(2, "ping"));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"].is_object());
    EXPECT_TRUE(resp["result"].empty());
}

// ---- list methods ----------------------------------------------------------

TEST_F(McpDispatchTest, ToolsListSurfacesRegisteredToolsWithSchema)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(3, "tools/list"));
    ASSERT_TRUE(resp.contains("result"));
    const Json& tools = resp["result"]["tools"];
    ASSERT_TRUE(tools.is_array());
    EXPECT_EQ(tools.size(), 3u);

    const Json* echo = nullptr;
    const Json* boom = nullptr;
    for (const auto& tool : tools)
    {
        if (tool["name"] == "fake_echo")
            echo = &tool;
        else if (tool["name"] == "fake_boom")
            boom = &tool;
    }
    ASSERT_NE(echo, nullptr);
    EXPECT_EQ((*echo)["description"], "Echo back the 'text' argument.");
    EXPECT_EQ((*echo)["inputSchema"]["type"], "object");
    EXPECT_TRUE((*echo)["inputSchema"]["properties"].contains("text"));

    // A tool with no InputSchema must still advertise a default object schema.
    ASSERT_NE(boom, nullptr);
    EXPECT_EQ((*boom)["inputSchema"], (Json{ { "type", "object" } }));
}

// A categorized tool surfaces its toolset under `_meta` (the spec extension point)
// in tools/list; an uncategorized tool omits `_meta` entirely, so tools/list is
// byte-for-byte unchanged for tools that predate toolsets.
TEST_F(McpDispatchTest, ToolsListSurfacesToolsetUnderMeta)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(3, "tools/list"));
    const Json& tools = resp["result"]["tools"];

    const Json* echo = FindToolNamed(tools, "fake_echo");
    ASSERT_NE(echo, nullptr);
    ASSERT_TRUE(echo->contains("_meta"));
    EXPECT_EQ((*echo)["_meta"]["io.oloengine/toolset"], "diag");

    // The uncategorized tool stays clean — no _meta, no toolset.
    const Json* toolError = FindToolNamed(tools, "fake_tool_error");
    ASSERT_NE(toolError, nullptr);
    EXPECT_FALSE(toolError->contains("_meta"));
    EXPECT_FALSE(toolError->contains("toolset"));
}

// ---- tools/search (custom discovery method) --------------------------------

// No query / no toolset: returns every tool (uncategorized included) plus a
// catalogue of toolsets with per-toolset counts. The uncategorized tool is in
// `tools` but contributes nothing to the catalogue.
TEST_F(McpDispatchTest, ToolsSearchNoArgsReturnsAllToolsAndCatalogue)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(20, "tools/search"));
    ASSERT_TRUE(resp.contains("result"));
    const Json& result = resp["result"];

    ASSERT_TRUE(result["tools"].is_array());
    EXPECT_EQ(result["tools"].size(), 3u); // all three fakes, including uncategorized

    ASSERT_TRUE(result["toolsets"].is_array());
    ASSERT_EQ(result["toolsets"].size(), 1u); // only "diag" is categorized
    EXPECT_EQ(result["toolsets"][0]["name"], "diag");
    EXPECT_EQ(result["toolsets"][0]["count"], 2); // fake_echo + fake_boom
}

// A matched, categorized entry carries both the friendly top-level `toolset` field
// and the `_meta` mirror; an uncategorized matched entry carries neither.
TEST_F(McpDispatchTest, ToolsSearchEntryCarriesToolsetFieldAndMeta)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(21, "tools/search"));
    const Json& tools = resp["result"]["tools"];

    const Json* echo = FindToolNamed(tools, "fake_echo");
    ASSERT_NE(echo, nullptr);
    EXPECT_EQ((*echo)["toolset"], "diag");
    EXPECT_EQ((*echo)["_meta"]["io.oloengine/toolset"], "diag");

    const Json* toolError = FindToolNamed(tools, "fake_tool_error");
    ASSERT_NE(toolError, nullptr);
    EXPECT_FALSE(toolError->contains("toolset"));
    EXPECT_FALSE(toolError->contains("_meta"));
}

// Filtering by toolset returns exactly the tools in that category.
TEST_F(McpDispatchTest, ToolsSearchFiltersByToolset)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(22, "tools/search", Json{ { "toolset", "diag" } }));
    const Json& tools = resp["result"]["tools"];
    ASSERT_EQ(tools.size(), 2u);
    EXPECT_NE(FindToolNamed(tools, "fake_echo"), nullptr);
    EXPECT_NE(FindToolNamed(tools, "fake_boom"), nullptr);
    EXPECT_EQ(FindToolNamed(tools, "fake_tool_error"), nullptr); // uncategorized excluded

    // The catalogue is always the full set, regardless of the active filter.
    EXPECT_EQ(resp["result"]["toolsets"].size(), 1u);
}

TEST_F(McpDispatchTest, ToolsSearchToolsetFilterIsCaseInsensitive)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(23, "tools/search", Json{ { "toolset", "DIAG" } }));
    EXPECT_EQ(resp["result"]["tools"].size(), 2u);
}

TEST_F(McpDispatchTest, ToolsSearchUnknownToolsetReturnsNoToolsButFullCatalogue)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(24, "tools/search", Json{ { "toolset", "nope" } }));
    EXPECT_TRUE(resp["result"]["tools"].empty());
    EXPECT_EQ(resp["result"]["toolsets"].size(), 1u); // catalogue still lists "diag"
}

// A query matches against name AND description (and title/toolset).
TEST_F(McpDispatchTest, ToolsSearchQueryMatchesNameAndDescription)
{
    const Json byName = m_Server.HandleMessage(MakeRequest(25, "tools/search", Json{ { "query", "echo" } }));
    ASSERT_EQ(byName["result"]["tools"].size(), 1u);
    EXPECT_EQ(byName["result"]["tools"][0]["name"], "fake_echo");

    const Json byDesc = m_Server.HandleMessage(MakeRequest(26, "tools/search", Json{ { "query", "throws" } }));
    ASSERT_EQ(byDesc["result"]["tools"].size(), 1u);
    EXPECT_EQ(byDesc["result"]["tools"][0]["name"], "fake_boom");
}

TEST_F(McpDispatchTest, ToolsSearchQueryIsCaseInsensitive)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(27, "tools/search", Json{ { "query", "ECHO" } }));
    ASSERT_EQ(resp["result"]["tools"].size(), 1u);
    EXPECT_EQ(resp["result"]["tools"][0]["name"], "fake_echo");
}

// Multi-term query is AND: every whitespace-separated term must appear somewhere.
TEST_F(McpDispatchTest, ToolsSearchQueryRequiresAllTerms)
{
    // "fake" + "throws" both live in fake_boom (name + description) -> one match.
    const Json both = m_Server.HandleMessage(MakeRequest(28, "tools/search", Json{ { "query", "fake throws" } }));
    ASSERT_EQ(both["result"]["tools"].size(), 1u);
    EXPECT_EQ(both["result"]["tools"][0]["name"], "fake_boom");

    // No single tool has both "echo" and "throws" -> zero matches.
    const Json none = m_Server.HandleMessage(MakeRequest(29, "tools/search", Json{ { "query", "echo throws" } }));
    EXPECT_TRUE(none["result"]["tools"].empty());
}

// A whitespace-only query is treated as "no query" (matches all, toolset filter
// still applies).
TEST_F(McpDispatchTest, ToolsSearchBlankQueryMatchesAll)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(30, "tools/search", Json{ { "query", "   " } }));
    EXPECT_EQ(resp["result"]["tools"].size(), 3u);
}

// Query and toolset combine with AND.
TEST_F(McpDispatchTest, ToolsSearchCombinesQueryAndToolset)
{
    const Json hit = m_Server.HandleMessage(
        MakeRequest(31, "tools/search", Json{ { "query", "throws" }, { "toolset", "diag" } }));
    ASSERT_EQ(hit["result"]["tools"].size(), 1u);
    EXPECT_EQ(hit["result"]["tools"][0]["name"], "fake_boom");

    // Right query, wrong toolset -> no match.
    const Json miss = m_Server.HandleMessage(
        MakeRequest(32, "tools/search", Json{ { "query", "throws" }, { "toolset", "other" } }));
    EXPECT_TRUE(miss["result"]["tools"].empty());
}

TEST_F(McpDispatchTest, ToolsSearchRejectsNonStringQuery)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(33, "tools/search", Json{ { "query", 123 } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602); // invalid params
}

TEST_F(McpDispatchTest, ToolsSearchRejectsNonStringToolset)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(34, "tools/search", Json{ { "toolset", true } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602);
}

// Matched search entries keep the full tools/list entry shape (name, description,
// inputSchema) so an agent can call a tool straight from a search result.
TEST_F(McpDispatchTest, ToolsSearchEntriesKeepToolListShape)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(35, "tools/search", Json{ { "query", "echo" } }));
    const Json& entry = resp["result"]["tools"][0];
    EXPECT_EQ(entry["name"], "fake_echo");
    EXPECT_EQ(entry["description"], "Echo back the 'text' argument.");
    EXPECT_EQ(entry["inputSchema"]["type"], "object");
    EXPECT_TRUE(entry["inputSchema"]["properties"].contains("text"));
}

// Regression: the toolset catalogue is keyed on the case-folded toolset and the
// filter compares case-folded, so a mixed-case Toolset value cannot split the
// catalogue (two "Render"/"render" entries) while a single case-insensitive filter
// still matches both — the catalogue count must reconcile with the filtered result.
// Standalone (own server) so the shared fixture's tool-count assertions are unaffected.
TEST(McpToolsSearchCaseFold, MixedCaseToolsetCollapsesInCatalogueAndFilter)
{
    McpServer server(EditorMcpContext{});

    const auto addTool = [&server](std::string name, std::string toolset)
    {
        ToolDef tool;
        tool.Name = std::move(name);
        tool.Toolset = std::move(toolset);
        tool.Description = "fake";
        tool.Handler = [](McpServer&, const Json&)
        { return ToolResult::Text("ok"); };
        server.RegisterTool(std::move(tool));
    };
    addTool("fake_a", "Render"); // capitalized
    addTool("fake_b", "render"); // lowercase — same logical toolset

    // Catalogue collapses to ONE canonical "render" entry covering both tools.
    const Json all = server.HandleMessage(MakeRequest(1, "tools/search"));
    const Json& toolsets = all["result"]["toolsets"];
    ASSERT_EQ(toolsets.size(), 1u);
    EXPECT_EQ(toolsets[0]["name"], "render");
    EXPECT_EQ(toolsets[0]["count"], 2);

    // A filter of any case returns BOTH tools, reconciling with the catalogue count.
    const Json filtered = server.HandleMessage(MakeRequest(2, "tools/search", Json{ { "toolset", "RENDER" } }));
    EXPECT_EQ(filtered["result"]["tools"].size(), 2u);
}

TEST_F(McpDispatchTest, ResourcesListSurfacesRegisteredResources)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(4, "resources/list"));
    const Json& resources = resp["result"]["resources"];
    ASSERT_TRUE(resources.is_array());
    ASSERT_EQ(resources.size(), 1u);
    EXPECT_EQ(resources[0]["uri"], "olo://fake");
    EXPECT_EQ(resources[0]["name"], "fake-resource");
    EXPECT_EQ(resources[0]["mimeType"], "text/plain");
}

TEST_F(McpDispatchTest, PromptsListSurfacesRegisteredPrompts)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(5, "prompts/list"));
    const Json& prompts = resp["result"]["prompts"];
    ASSERT_TRUE(prompts.is_array());
    ASSERT_EQ(prompts.size(), 1u);
    EXPECT_EQ(prompts[0]["name"], "fake-prompt");
    EXPECT_EQ(prompts[0]["title"], "Fake Prompt");
}

// ---- tools/call ------------------------------------------------------------

TEST_F(McpDispatchTest, ToolsCallHappyPathReturnsContent)
{
    const Json resp = m_Server.HandleMessage(
        MakeRequest(6, "tools/call", Json{ { "name", "fake_echo" }, { "arguments", { { "text", "hello" } } } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    const Json& result = resp["result"];
    EXPECT_FALSE(result["isError"]);
    ASSERT_TRUE(result["content"].is_array());
    ASSERT_EQ(result["content"].size(), 1u);
    EXPECT_EQ(result["content"][0]["type"], "text");
    EXPECT_EQ(result["content"][0]["text"], "hello");
}

TEST_F(McpDispatchTest, ToolsCallUnknownToolIsProtocolError)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(7, "tools/call", Json{ { "name", "does_not_exist" } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_FALSE(resp.contains("result"));
    EXPECT_EQ(resp["error"]["code"], -32602); // invalid params
}

TEST_F(McpDispatchTest, ToolsCallMissingNameIsInvalidParams)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(8, "tools/call", Json::object()));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602);
}

// A tool that reports failure surfaces as a *tool-level* error (result.isError),
// NOT a JSON-RPC protocol error. This distinction matters: the call succeeded at
// the protocol layer; the tool itself failed.
TEST_F(McpDispatchTest, ToolsCallToolErrorIsNotProtocolError)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(9, "tools/call", Json{ { "name", "fake_tool_error" } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_TRUE(resp["result"]["isError"]);
}

TEST_F(McpDispatchTest, ToolsCallHandlerThrowIsCaughtAsToolError)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(10, "tools/call", Json{ { "name", "fake_boom" } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"]["isError"]);
    const std::string text = resp["result"]["content"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("Tool failed"), std::string::npos);
    EXPECT_NE(text.find("kaboom"), std::string::npos);
}

// ---- resources/read --------------------------------------------------------

TEST_F(McpDispatchTest, ResourcesReadHappyPathReturnsContents)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(11, "resources/read", Json{ { "uri", "olo://fake" } }));
    ASSERT_TRUE(resp.contains("result"));
    const Json& contents = resp["result"]["contents"];
    ASSERT_TRUE(contents.is_array());
    ASSERT_EQ(contents.size(), 1u);
    EXPECT_EQ(contents[0]["uri"], "olo://fake");
    EXPECT_EQ(contents[0]["mimeType"], "text/plain");
    EXPECT_EQ(contents[0]["text"], "resource-body");
}

TEST_F(McpDispatchTest, ResourcesReadUnknownUriIsError)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(12, "resources/read", Json{ { "uri", "olo://nope" } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602);
}

TEST_F(McpDispatchTest, ResourcesReadMissingUriIsInvalidParams)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(13, "resources/read", Json::object()));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602);
}

// ---- prompts/get -----------------------------------------------------------

TEST_F(McpDispatchTest, PromptsGetHappyPathExpandsToUserMessage)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(14, "prompts/get", Json{ { "name", "fake-prompt" } }));
    ASSERT_TRUE(resp.contains("result"));
    const Json& messages = resp["result"]["messages"];
    ASSERT_TRUE(messages.is_array());
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0]["role"], "user");
    EXPECT_EQ(messages[0]["content"]["type"], "text");
    EXPECT_EQ(messages[0]["content"]["text"], "do the thing");
}

TEST_F(McpDispatchTest, PromptsGetUnknownNameIsError)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(15, "prompts/get", Json{ { "name", "missing" } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602);
}

// ---- dispatch-level error envelopes ----------------------------------------

TEST_F(McpDispatchTest, UnknownMethodIsMethodNotFound)
{
    const Json resp = m_Server.HandleMessage(MakeRequest(16, "frobnicate"));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32601); // method not found
}

TEST_F(McpDispatchTest, MissingMethodIsInvalidRequest)
{
    Json msg = { { "jsonrpc", "2.0" }, { "id", 17 } }; // no "method"
    const Json resp = m_Server.HandleMessage(msg);
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32600); // invalid request
}

TEST_F(McpDispatchTest, NonObjectMessageIsInvalidRequest)
{
    const Json resp = m_Server.HandleMessage(Json::array({ 1, 2, 3 }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32600);
}

// A non-string "method" must yield a clean Invalid Request, not throw (nlohmann's
// value() would throw type_error.302 on the type mismatch).
TEST_F(McpDispatchTest, NonStringMethodIsInvalidRequest)
{
    Json msg = { { "jsonrpc", "2.0" }, { "id", 18 }, { "method", 123 } };
    Json resp;
    ASSERT_NO_THROW(resp = m_Server.HandleMessage(msg));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32600); // invalid request
}

TEST_F(McpDispatchTest, NotificationProducesNoResponse)
{
    // A message with no "id" is a notification — dispatch returns a null Json.
    const Json resp = m_Server.HandleMessage(MakeRequest(Json(nullptr), "ping"));
    EXPECT_TRUE(resp.is_null());
}

// ---- framing (ProcessRequestBody) ------------------------------------------

TEST_F(McpDispatchTest, FramingMalformedBodyIsParseError)
{
    const McpServer::FramedResponse framed = m_Server.ProcessRequestBody("{ this is not json");
    ASSERT_FALSE(framed.Body.is_null());
    EXPECT_EQ(framed.Status, 200);
    EXPECT_EQ(framed.Body["error"]["code"], -32700); // parse error
    EXPECT_TRUE(framed.SessionId.empty());
}

TEST_F(McpDispatchTest, FramingSingleMessageReturnsResponse)
{
    const McpServer::FramedResponse framed = m_Server.ProcessRequestBody(MakeRequest(1, "ping").dump());
    EXPECT_EQ(framed.Status, 200);
    ASSERT_TRUE(framed.Body.contains("result"));
    EXPECT_EQ(framed.Body["id"], 1);
}

TEST_F(McpDispatchTest, FramingBatchPreservesOrder)
{
    const Json batch = Json::array({ MakeRequest(1, "ping"),
                                     MakeRequest(2, "tools/list"),
                                     MakeRequest(3, "ping") });
    const McpServer::FramedResponse framed = m_Server.ProcessRequestBody(batch.dump());

    ASSERT_TRUE(framed.Body.is_array());
    ASSERT_EQ(framed.Body.size(), 3u);
    EXPECT_EQ(framed.Body[0]["id"], 1);
    EXPECT_EQ(framed.Body[1]["id"], 2);
    EXPECT_EQ(framed.Body[2]["id"], 3);
    EXPECT_EQ(framed.Status, 200);
}

TEST_F(McpDispatchTest, FramingBatchDropsNotifications)
{
    const Json batch = Json::array({ MakeRequest(1, "ping"),
                                     MakeRequest(Json(nullptr), "ping"), // notification
                                     MakeRequest(2, "ping") });
    const McpServer::FramedResponse framed = m_Server.ProcessRequestBody(batch.dump());

    ASSERT_TRUE(framed.Body.is_array());
    ASSERT_EQ(framed.Body.size(), 2u);
    EXPECT_EQ(framed.Body[0]["id"], 1);
    EXPECT_EQ(framed.Body[1]["id"], 2);
}

TEST_F(McpDispatchTest, FramingAllNotificationBatchReturnsNoBody)
{
    const Json batch = Json::array({ MakeRequest(Json(nullptr), "ping"),
                                     MakeRequest(Json(nullptr), "ping") });
    const McpServer::FramedResponse framed = m_Server.ProcessRequestBody(batch.dump());

    EXPECT_TRUE(framed.Body.is_null());
    EXPECT_EQ(framed.Status, 202);
}

TEST_F(McpDispatchTest, FramingEmptyBatchIsInvalidRequest)
{
    const McpServer::FramedResponse framed = m_Server.ProcessRequestBody("[]");
    ASSERT_FALSE(framed.Body.is_null());
    EXPECT_EQ(framed.Status, 200);
    EXPECT_EQ(framed.Body["error"]["code"], -32600); // invalid request
}

TEST_F(McpDispatchTest, FramingSingleNotificationReturnsNoBody)
{
    const McpServer::FramedResponse framed =
        m_Server.ProcessRequestBody(MakeRequest(Json(nullptr), "ping").dump());
    EXPECT_TRUE(framed.Body.is_null());
    EXPECT_EQ(framed.Status, 202);
    EXPECT_TRUE(framed.SessionId.empty());
}

// ---- framing: the initialize session-id side effect ------------------------

TEST_F(McpDispatchTest, FramingInitializeMintsSessionId)
{
    const McpServer::FramedResponse framed =
        m_Server.ProcessRequestBody(MakeRequest(1, "initialize", Json{ { "protocolVersion", kLatestProtocol } }).dump());

    ASSERT_TRUE(framed.Body.contains("result"));
    EXPECT_FALSE(framed.SessionId.empty());
    EXPECT_EQ(framed.SessionId.size(), 32u); // 16 random bytes -> 32 hex chars
}

TEST_F(McpDispatchTest, FramingNonInitializeMintsNoSessionId)
{
    const McpServer::FramedResponse framed = m_Server.ProcessRequestBody(MakeRequest(1, "ping").dump());
    EXPECT_TRUE(framed.SessionId.empty());
}

TEST_F(McpDispatchTest, FramingInitializeNotificationMintsNoSessionId)
{
    // An initialize *notification* (no id) yields no response and no session.
    const McpServer::FramedResponse framed = m_Server.ProcessRequestBody(
        MakeRequest(Json(nullptr), "initialize", Json{ { "protocolVersion", kLatestProtocol } }).dump());
    EXPECT_TRUE(framed.Body.is_null());
    EXPECT_EQ(framed.Status, 202);
    EXPECT_TRUE(framed.SessionId.empty());
}

// ---- security: bearer auth (pure helper) -----------------------------------

TEST(McpDispatchSecurity, BearerAuthAcceptsExactToken)
{
    EXPECT_TRUE(McpServer::CheckBearerAuth("Bearer abc123def456", "abc123def456"));
}

TEST(McpDispatchSecurity, BearerAuthRejectsWrongToken)
{
    EXPECT_FALSE(McpServer::CheckBearerAuth("Bearer abc123def457", "abc123def456"));
}

TEST(McpDispatchSecurity, BearerAuthRejectsTokenPrefix)
{
    // Length is folded into the constant-time compare, so a proper prefix of the
    // real token must not authenticate.
    EXPECT_FALSE(McpServer::CheckBearerAuth("Bearer abc123", "abc123def456"));
}

TEST(McpDispatchSecurity, BearerAuthRejectsMissingOrWrongScheme)
{
    EXPECT_FALSE(McpServer::CheckBearerAuth("abc123def456", "abc123def456"));       // no scheme
    EXPECT_FALSE(McpServer::CheckBearerAuth("Basic abc123def456", "abc123def456")); // wrong scheme
    EXPECT_FALSE(McpServer::CheckBearerAuth("Bearer", "abc123def456"));             // no token
    EXPECT_FALSE(McpServer::CheckBearerAuth("Bearer ", "abc123def456"));            // empty token
    EXPECT_FALSE(McpServer::CheckBearerAuth("", "abc123def456"));                   // empty header
}

TEST(McpDispatchSecurity, BearerAuthRejectsEverythingWhenServerHasNoToken)
{
    // An empty expected token means the server isn't running; nothing authenticates.
    EXPECT_FALSE(McpServer::CheckBearerAuth("Bearer anything", ""));
    EXPECT_FALSE(McpServer::CheckBearerAuth("Bearer ", ""));
    EXPECT_FALSE(McpServer::CheckBearerAuth("", ""));
}

// ---- security: origin / DNS-rebinding defence (pure helper) -----------------

TEST(McpDispatchSecurity, OriginAllowsAbsentOrNull)
{
    EXPECT_TRUE(McpServer::IsOriginAllowed(""));     // non-browser agent, no Origin
    EXPECT_TRUE(McpServer::IsOriginAllowed("null")); // file:// / sandboxed origin
}

TEST(McpDispatchSecurity, OriginAllowsLoopbackHosts)
{
    EXPECT_TRUE(McpServer::IsOriginAllowed("http://127.0.0.1"));
    EXPECT_TRUE(McpServer::IsOriginAllowed("http://127.0.0.1:7345"));
    EXPECT_TRUE(McpServer::IsOriginAllowed("http://localhost"));
    EXPECT_TRUE(McpServer::IsOriginAllowed("http://localhost:1234/mcp"));
    // Bracketed IPv6 loopback — the ':' separators inside the address must not
    // truncate the host (a port and/or path may follow the closing ']').
    EXPECT_TRUE(McpServer::IsOriginAllowed("http://[::1]"));
    EXPECT_TRUE(McpServer::IsOriginAllowed("http://[::1]:7345"));
    EXPECT_TRUE(McpServer::IsOriginAllowed("http://[::1]:7345/mcp"));
}

TEST(McpDispatchSecurity, OriginRejectsRemoteHosts)
{
    EXPECT_FALSE(McpServer::IsOriginAllowed("http://evil.com"));
    EXPECT_FALSE(McpServer::IsOriginAllowed("https://example.org:443/mcp"));
    // A loopback-looking label that is really a subdomain of an attacker host.
    EXPECT_FALSE(McpServer::IsOriginAllowed("http://127.0.0.1.evil.com"));
}

TEST(McpDispatchSecurity, OriginRejectsMalformed)
{
    EXPECT_FALSE(McpServer::IsOriginAllowed("not-a-url"));
}

// ---- discovery-file path resolution (per-worktree isolation, issue #316) ----

namespace
{
    // Cross-platform set ("" unsets) / unset of an environment variable, mirroring
    // the _putenv_s / setenv split used in OloEngineTest.cpp's main().
    void SetEnvVar(const char* name, const char* value)
    {
#if defined(_WIN32)
        _putenv_s(name, value); // empty value removes the variable on the MSVC CRT
#else
        if (value != nullptr && *value != '\0')
            ::setenv(name, value, 1);
        else
            ::unsetenv(name);
#endif
    }

    // RAII: force OLO_MCP_DISCOVERY_FILE to `value` (pass "" to unset it) for the
    // scope, then restore whatever was there before so tests don't leak into each
    // other or the surrounding process.
    class ScopedDiscoveryOverride
    {
      public:
        explicit ScopedDiscoveryOverride(const char* value)
        {
            if (const char* prev = std::getenv(kName); prev != nullptr)
            {
                m_HadPrev = true;
                m_Prev = prev;
            }
            SetEnvVar(kName, value);
        }
        ~ScopedDiscoveryOverride()
        {
            SetEnvVar(kName, m_HadPrev ? m_Prev.c_str() : "");
        }
        ScopedDiscoveryOverride(const ScopedDiscoveryOverride&) = delete;
        ScopedDiscoveryOverride& operator=(const ScopedDiscoveryOverride&) = delete;

      private:
        static constexpr const char* kName = "OLO_MCP_DISCOVERY_FILE";
        bool m_HadPrev = false;
        std::string m_Prev;
    };
} // namespace

TEST(McpDiscoveryFile, OverrideEnvWinsVerbatimRegardlessOfPort)
{
    const std::string custom = "C:/tmp/my-worktree/oloengine-mcp.json";
    ScopedDiscoveryOverride guard(custom.c_str());

    // The override is returned exactly, ignoring both the default and a custom port.
    EXPECT_EQ(McpServer::DiscoveryFilePath(OloEngine::MCP::DefaultPort), custom);
    EXPECT_EQ(McpServer::DiscoveryFilePath(54321), custom);
}

TEST(McpDiscoveryFile, DefaultPortKeepsLegacyUnsuffixedName)
{
    ScopedDiscoveryOverride guard(""); // ensure no override is in effect

    const std::string path = McpServer::DiscoveryFilePath(OloEngine::MCP::DefaultPort);
    ASSERT_FALSE(path.empty());
    // Back-compat: the default port must keep the single legacy file name (no port
    // suffix) so the panel / docs / manual attach still find oloengine-mcp.json.
    EXPECT_TRUE(path.ends_with("oloengine-mcp.json"));
    EXPECT_FALSE(path.ends_with("oloengine-mcp-7345.json"));
}

TEST(McpDiscoveryFile, NonDefaultPortNamespacesByPort)
{
    ScopedDiscoveryOverride guard(""); // ensure no override is in effect

    const std::string path = McpServer::DiscoveryFilePath(54321);
    ASSERT_FALSE(path.empty());
    // Two editors on distinct ports must land on distinct files.
    EXPECT_TRUE(path.ends_with("oloengine-mcp-54321.json"));
    EXPECT_NE(path, McpServer::DiscoveryFilePath(54322));
}
