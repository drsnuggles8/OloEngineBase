// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// The outbound MCP client's protocol + lifecycle layer (#673 Tier 1, bullet 1),
// driven end-to-end through McpServer::ConnectClientWithTransport with an
// IN-MEMORY fake transport — no child process, socket, or GL: the initialize /
// initialized / tools/list handshake, prefixed tool merging, the bridged
// tools/call round-trip (content + structuredContent pass-through, child
// errors, timeouts), child-death teardown (pending calls fail fast, tools
// unpublish, status flips), disconnect, duplicate aliases, and the
// polite -32601 reply to a request initiated BY the child.
//
// The registry-side trust seam (forced authority posture, namespace
// validation) is McpClientToolsTest's job; this file assumes it and focuses on
// the connection.
#include "MCP/McpClient.h"
#include "MCP/McpServer.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::IMcpClientTransport;
    using OloEngine::MCP::McpClientConfig;
    using OloEngine::MCP::McpClientStatus;
    using OloEngine::MCP::McpClientTransportFactory;
    using OloEngine::MCP::McpServer;
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

    // An in-memory transport: replies synchronously inside WriteLine via a
    // caller-supplied script. Close() invokes onClosed inline exactly once —
    // satisfying the "no callbacks after Close returns" contract trivially.
    class FakeTransport final : public IMcpClientTransport
    {
      public:
        std::function<void(const Json& request, FakeTransport&)> Script;
        std::vector<Json> Written;
        std::function<void(std::string)> OnLineSink;
        std::function<void()> OnClosedSink;

        [[nodiscard]] bool WriteLine(const std::string& payload) override
        {
            const Json message = Json::parse(payload, nullptr, false);
            Written.push_back(message);
            if (Script)
                Script(message, *this);
            return true;
        }

        void Reply(const Json& message)
        {
            if (OnLineSink)
                OnLineSink(message.dump());
        }

        void Close() override
        {
            if (!m_Closed.exchange(true) && OnClosedSink)
                OnClosedSink();
        }

        [[nodiscard]] bool WroteMethod(const std::string& method) const
        {
            for (const Json& message : Written)
            {
                if (message.is_object() && message.value("method", std::string{}) == method)
                    return true;
            }
            return false;
        }

      private:
        std::atomic<bool> m_Closed{ false };
    };

    // The standard well-behaved child: answers initialize and serves one tool
    // ("read_file"); tools/call behaviour comes from `onCall`.
    std::function<void(const Json&, FakeTransport&)> WellBehavedScript(
        std::function<void(const Json& request, FakeTransport&)> onCall = {})
    {
        return [onCall = std::move(onCall)](const Json& request, FakeTransport& transport)
        {
            const std::string method = request.value("method", std::string{});
            if (method == "initialize")
            {
                transport.Reply(Json{
                    { "jsonrpc", "2.0" },
                    { "id", request["id"] },
                    { "result",
                      Json{ { "protocolVersion", "2025-06-18" },
                            { "capabilities", Json::object() },
                            { "serverInfo", Json{ { "name", "fake-child" }, { "version", "1.0" } } } } } });
            }
            else if (method == "tools/list")
            {
                transport.Reply(Json{
                    { "jsonrpc", "2.0" },
                    { "id", request["id"] },
                    { "result",
                      Json{ { "tools",
                              Json::array(
                                  { Json{ { "name", "read_file" },
                                          { "title", "Read file" },
                                          { "description", "Read a file from disk." },
                                          { "inputSchema",
                                            Json{ { "type", "object" },
                                                  { "properties",
                                                    { { "path", { { "type", "string" } } } } } } } } }) } } } });
            }
            else if (method == "tools/call" && onCall)
            {
                onCall(request, transport);
            }
        };
    }

    // Factory exposing the created fake so the test can poke it post-connect.
    McpClientTransportFactory FakeFactory(FakeTransport*& outTransport,
                                          std::function<void(const Json&, FakeTransport&)> script)
    {
        return [&outTransport, script = std::move(script)](
                   const std::string& /*command*/, std::function<void(std::string)> onLine,
                   std::function<void()> onClosed, std::string& /*outError*/)
                   -> std::unique_ptr<IMcpClientTransport>
        {
            auto transport = std::make_unique<FakeTransport>();
            transport->Script = script;
            transport->OnLineSink = std::move(onLine);
            transport->OnClosedSink = std::move(onClosed);
            outTransport = transport.get();
            return transport;
        };
    }

    McpClientConfig FilesConfig(std::chrono::milliseconds callTimeout = std::chrono::milliseconds(5000))
    {
        McpClientConfig config;
        config.Alias = "files";
        config.Command = "fake-child.exe";
        config.HandshakeTimeout = std::chrono::milliseconds(5000);
        config.CallTimeout = callTimeout;
        return config;
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
} // namespace

TEST(McpClientStdio, ConnectHandshakesAndMergesPrefixedTools)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const std::string error = server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, WellBehavedScript()));
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_NE(fake, nullptr);

    // Handshake order on the wire: initialize, then the initialized
    // notification, then tools/list.
    ASSERT_GE(fake->Written.size(), 3u);
    EXPECT_EQ(fake->Written[0]["method"], "initialize");
    EXPECT_EQ(fake->Written[1]["method"], "notifications/initialized");
    EXPECT_FALSE(fake->Written[1].contains("id")) << "initialized must be a notification";
    EXPECT_EQ(fake->Written[2]["method"], "tools/list");

    const Json list = server.HandleMessage(MakeRequest(1, "tools/list"));
    const Json* entry = FindToolEntry(list, "ext.files.read_file");
    ASSERT_NE(entry, nullptr) << list.dump(2);
    EXPECT_NE((*entry)["description"].get<std::string>().find("bridged from external MCP server 'files'"),
              std::string::npos);
    // The child's inputSchema is preserved for local pre-forward validation.
    EXPECT_EQ((*entry)["inputSchema"]["properties"].contains("path"), true);

    const std::vector<McpClientStatus> statuses = server.ClientStatuses();
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0].Alias, "files");
    EXPECT_TRUE(statuses[0].Connected);
    EXPECT_EQ(statuses[0].ToolCount, 1u);
}

TEST(McpClientStdio, BridgedCallRoundTripsContentAndStructuredContent)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    Json seenArguments;
    const auto onCall = [&seenArguments](const Json& request, FakeTransport& transport)
    {
        seenArguments = request["params"].value("arguments", Json::object());
        transport.Reply(Json{
            { "jsonrpc", "2.0" },
            { "id", request["id"] },
            { "result",
              Json{ { "content", Json::array({ Json{ { "type", "text" }, { "text", "file data" } } }) },
                    { "structuredContent", Json{ { "ok", true }, { "bytes", 9 } } },
                    { "isError", false } } } });
    };
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, WellBehavedScript(onCall))).empty());

    server.SetAllowWrites(true); // bridged tools are always ProjectWrite
    const Json response = server.HandleMessage(MakeRequest(
        2, "tools/call", Json{ { "name", "ext.files.read_file" }, { "arguments", Json{ { "path", "a.txt" } } } }));

    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    EXPECT_EQ(seenArguments["path"], "a.txt");
    EXPECT_EQ(response["result"]["isError"], false);
    EXPECT_EQ(response["result"]["content"][0]["text"], "file data");
    EXPECT_EQ(response["result"]["structuredContent"]["ok"], true);
    EXPECT_EQ(response["result"]["structuredContent"]["bytes"], 9);
}

TEST(McpClientStdio, ChildErrorSurfacesAsToolError)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const auto onCall = [](const Json& request, FakeTransport& transport)
    {
        transport.Reply(Json{ { "jsonrpc", "2.0" },
                              { "id", request["id"] },
                              { "error", Json{ { "code", -32000 }, { "message", "disk on fire" } } } });
    };
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, WellBehavedScript(onCall))).empty());
    server.SetAllowWrites(true);

    const Json response =
        server.HandleMessage(MakeRequest(3, "tools/call", Json{ { "name", "ext.files.read_file" } }));
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    EXPECT_EQ(response["result"]["isError"], true);
    const std::string text = response["result"]["content"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("disk on fire"), std::string::npos);
    EXPECT_NE(text.find("'files'"), std::string::npos);
}

TEST(McpClientStdio, UnansweredCallTimesOutWithACleanError)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    // Script answers the handshake but swallows tools/call.
    ASSERT_TRUE(server
                    .ConnectClientWithTransport(FilesConfig(std::chrono::milliseconds(100)),
                                                FakeFactory(fake, WellBehavedScript()))
                    .empty());
    server.SetAllowWrites(true);

    const Json response =
        server.HandleMessage(MakeRequest(4, "tools/call", Json{ { "name", "ext.files.read_file" } }));
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    EXPECT_EQ(response["result"]["isError"], true);
    EXPECT_NE(response["result"]["content"][0]["text"].get<std::string>().find("did not respond"),
              std::string::npos);
}

TEST(McpClientStdio, ChildDeathUnpublishesToolsAndFlipsStatus)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, WellBehavedScript())).empty());
    ASSERT_NE(FindToolEntry(server.HandleMessage(MakeRequest(5, "tools/list")), "ext.files.read_file"), nullptr);

    fake->Close(); // the stream breaks — as if the child crashed

    EXPECT_EQ(FindToolEntry(server.HandleMessage(MakeRequest(6, "tools/list")), "ext.files.read_file"), nullptr)
        << "a dead child's tools must be unpublished";
    const std::vector<McpClientStatus> statuses = server.ClientStatuses();
    ASSERT_EQ(statuses.size(), 1u) << "the dead connection stays listed for the panel until disconnected";
    EXPECT_FALSE(statuses[0].Connected);

    // Reaping it frees the alias for a reconnect.
    EXPECT_TRUE(server.DisconnectClient("files"));
    EXPECT_TRUE(server.ClientStatuses().empty());
}

TEST(McpClientStdio, DisconnectRemovesToolsAndFreesTheAlias)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, WellBehavedScript())).empty());

    // A second connect on a live alias is refused.
    FakeTransport* second = nullptr;
    EXPECT_FALSE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(second, WellBehavedScript())).empty());

    EXPECT_TRUE(server.DisconnectClient("files"));
    EXPECT_EQ(FindToolEntry(server.HandleMessage(MakeRequest(7, "tools/list")), "ext.files.read_file"), nullptr);
    EXPECT_FALSE(server.DisconnectClient("files")) << "double-disconnect reports false";

    // Alias is reusable afterwards.
    FakeTransport* third = nullptr;
    EXPECT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(third, WellBehavedScript())).empty());
}

TEST(McpClientStdio, RequestFromTheChildGetsAPoliteMethodNotFound)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, WellBehavedScript())).empty());

    fake->Reply(Json{ { "jsonrpc", "2.0" }, { "id", 999 }, { "method", "sampling/createMessage" } });

    const Json* reply = nullptr;
    for (const Json& message : fake->Written)
    {
        if (message.is_object() && message.value("id", Json()) == Json(999) && message.contains("error"))
            reply = &message;
    }
    ASSERT_NE(reply, nullptr) << "the child must not be left hanging on its request";
    EXPECT_EQ((*reply)["error"]["code"], -32601);
}

TEST(McpClientStdio, StopShutsDownConnections)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, WellBehavedScript())).empty());

    server.Stop(); // never Start()ed — takes the not-running path, which must still reap clients

    EXPECT_TRUE(server.ClientStatuses().empty());
    EXPECT_EQ(FindToolEntry(server.HandleMessage(MakeRequest(8, "tools/list")), "ext.files.read_file"), nullptr);
}
