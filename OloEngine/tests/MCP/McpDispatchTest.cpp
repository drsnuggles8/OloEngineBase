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

    // Fixture: an McpServer with a stub (empty) editor context plus a handful of
    // fake registrations, so dispatch can be exercised without the real 19 tools
    // / 2 resources / 3 prompts and without an editor backing the server.
    class McpDispatchTest : public ::testing::Test
    {
      protected:
        McpDispatchTest()
            : m_Server(EditorMcpContext{})
        {
            ToolDef echo;
            echo.Name = "fake_echo";
            echo.Description = "Echo back the 'text' argument.";
            echo.InputSchema = Json{ { "type", "object" },
                                     { "properties", { { "text", { { "type", "string" } } } } } };
            echo.Handler = [](McpServer&, const Json& args)
            { return ToolResult::Text(args.value("text", std::string{})); };
            m_Server.RegisterTool(std::move(echo));

            // No InputSchema set -> tools/list must substitute a default object schema.
            ToolDef boom;
            boom.Name = "fake_boom";
            boom.Description = "Always throws.";
            boom.Handler = [](McpServer&, const Json&) -> ToolResult
            { throw std::runtime_error("kaboom"); };
            m_Server.RegisterTool(std::move(boom));

            ToolDef toolError;
            toolError.Name = "fake_tool_error";
            toolError.Description = "Returns a tool-level error (isError=true).";
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
