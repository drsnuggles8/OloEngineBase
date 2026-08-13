// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// The outbound MCP client's protocol + lifecycle layer (#673 Tier 1, bullet 1),
// driven end-to-end through McpServer::ConnectClientWithTransport with an
// IN-MEMORY fake transport — no child process, socket, or GL: protocol-era
// negotiation (#777 — the `server/discover` probe and its fallback to the
// initialize / initialized handshake), tools/list, prefixed tool merging, the
// bridged tools/call round-trip (content + structuredContent pass-through, child
// errors, timeouts), child-death teardown (pending calls fail fast, tools
// unpublish, status flips), disconnect, duplicate aliases, and the
// polite -32601 reply to a request initiated BY the child.
//
// Both protocol eras are pinned here on purpose. Spec 2026-07-28 removed the
// handshake, both shapes coexist for a ≥12-month offramp, and the failure mode of
// getting the detection wrong is silent — so the legacy path must keep being
// asserted beside the modern one, not replaced by it.
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

      private:
        std::atomic<bool> m_Closed{ false };
    };

    // The one tools/list payload every script below serves.
    Json ToolsListResult(const Json& id)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "result",
                       Json{ { "tools",
                               Json::array({ Json{ { "name", "read_file" },
                                                   { "title", "Read file" },
                                                   { "description", "Read a file from disk." },
                                                   { "inputSchema",
                                                     Json{ { "type", "object" },
                                                           { "properties",
                                                             { { "path", { { "type", "string" } } } } } } } } }) } } } };
    }

    // The standard well-behaved LEGACY child: answers initialize and serves one
    // tool ("read_file"); tools/call behaviour comes from `onCall`.
    //
    // It answers the `server/discover` era probe (#777) with -32601, which is what
    // a real handshake-era MCP server does with an unknown method. That is both
    // realistic AND what keeps this suite fast: without a reply the probe would
    // burn its full timeout on every connect in this file.
    std::function<void(const Json&, FakeTransport&)> WellBehavedScript(
        std::function<void(const Json& request, FakeTransport&)> onCall = {})
    {
        return [onCall = std::move(onCall)](const Json& request, FakeTransport& transport)
        {
            const std::string method = request.value("method", std::string{});
            if (method == "server/discover")
            {
                transport.Reply(Json{ { "jsonrpc", "2.0" },
                                      { "id", request["id"] },
                                      { "error", Json{ { "code", -32601 },
                                                       { "message", "Method not found: server/discover" } } } });
            }
            else if (method == "initialize")
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
                transport.Reply(ToolsListResult(request["id"]));
            }
            else if (method == "tools/call" && onCall)
            {
                onCall(request, transport);
            }
        };
    }

    // A MODERN (2026-07-28 stateless core) child: it implements `server/discover`
    // and has no `initialize` at all. `discoverVersions` is what it advertises as
    // supportedVersions, so a test can drive the mutual-version outcomes.
    std::function<void(const Json&, FakeTransport&)> ModernScript(
        Json discoverVersions = Json::array({ "2026-07-28" }),
        std::function<void(const Json& request, FakeTransport&)> onCall = {})
    {
        return [discoverVersions = std::move(discoverVersions), onCall = std::move(onCall)](
                   const Json& request, FakeTransport& transport)
        {
            const std::string method = request.value("method", std::string{});
            if (method == "server/discover")
            {
                transport.Reply(Json{ { "jsonrpc", "2.0" },
                                      { "id", request["id"] },
                                      { "result",
                                        Json{ { "resultType", "complete" },
                                              { "supportedVersions", discoverVersions },
                                              { "capabilities", Json{ { "tools", Json::object() } } },
                                              { "ttlMs", 300000 },
                                              { "cacheScope", "public" } } } });
            }
            else if (method == "initialize")
            {
                // A modern-only server has no handshake; answering one would hide a
                // client bug where we send it anyway.
                transport.Reply(Json{ { "jsonrpc", "2.0" },
                                      { "id", request["id"] },
                                      { "error", Json{ { "code", -32601 },
                                                       { "message", "Method not found: initialize" } } } });
            }
            else if (method == "tools/list")
            {
                transport.Reply(ToolsListResult(request["id"]));
            }
            else if (method == "tools/call" && onCall)
            {
                onCall(request, transport);
            }
        };
    }

    // Pull the `_meta` block off a recorded request, or a null Json when absent.
    Json MetaOf(const Json& request)
    {
        if (!request.is_object() || !request.contains("params") || !request["params"].is_object())
            return Json();
        return request["params"].value("_meta", Json());
    }

    // The first recorded request with this method, or nullptr.
    const Json* FirstWritten(const std::vector<Json>& written, const std::string& method)
    {
        for (const Json& message : written)
        {
            if (message.is_object() && message.value("method", std::string{}) == method)
                return &message;
        }
        return nullptr;
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

    // Wire order against a LEGACY child: the era probe first (#777), then the
    // handshake it falls back to — initialize, the initialized notification,
    // tools/list.
    ASSERT_GE(fake->Written.size(), 4u);
    EXPECT_EQ(fake->Written[0]["method"], "server/discover");
    EXPECT_EQ(fake->Written[1]["method"], "initialize");
    EXPECT_EQ(fake->Written[2]["method"], "notifications/initialized");
    EXPECT_FALSE(fake->Written[2].contains("id")) << "initialized must be a notification";
    EXPECT_EQ(fake->Written[3]["method"], "tools/list");

    // Legacy requests carry NO per-request `_meta` — the handshake conveyed the
    // version and identity once, and stamping `_meta` on a 2025-era server would
    // be a modern-shape request it never asked for.
    EXPECT_TRUE(MetaOf(fake->Written[1]).is_null()) << fake->Written[1].dump(2);
    EXPECT_TRUE(MetaOf(fake->Written[3]).is_null()) << fake->Written[3].dump(2);
    EXPECT_EQ(fake->Written[1]["params"]["protocolVersion"], "2025-06-18");

    ASSERT_EQ(server.ClientStatuses().size(), 1u);
    EXPECT_EQ(server.ClientStatuses()[0].Era, OloEngine::MCP::McpProtocolEra::Legacy);
    EXPECT_EQ(server.ClientStatuses()[0].ProtocolVersion, "2025-06-18");

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

// ---- protocol-era negotiation (issue #777) ----------------------------------
//
// Spec 2026-07-28 removes the `initialize` handshake in favour of a stateless
// core where every request is self-describing. Both eras will coexist for the
// whole ≥12-month offramp, so this client has to detect which one a child speaks
// and can never assume either. The rule it implements is the spec's own stdio
// backward-compatibility rule: probe `server/discover`, treat a RECOGNIZED modern
// reply as modern, and treat everything else — including silence — as legacy.
//
// The load-bearing property is that the legacy path still works untouched; the
// tests above cover it, and these cover the modern side plus each way the probe
// can be answered. Getting this wrong is silent: a client that guesses "modern"
// against a legacy child sends requests it will never understand.

TEST(McpClientStdio, ModernChildSkipsTheHandshakeAndStampsMetaOnEveryRequest)
{
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, ModernScript())).empty());
    ASSERT_NE(fake, nullptr);

    // No handshake at all: `initialize` is an unknown method to a modern server,
    // and `notifications/initialized` would be a message with nowhere to land.
    EXPECT_EQ(FirstWritten(fake->Written, "initialize"), nullptr);
    EXPECT_EQ(FirstWritten(fake->Written, "notifications/initialized"), nullptr);

    ASSERT_GE(fake->Written.size(), 2u);
    EXPECT_EQ(fake->Written[0]["method"], "server/discover");
    EXPECT_EQ(fake->Written[1]["method"], "tools/list");

    // Every request carries the three reserved `_meta` keys. protocolVersion and
    // clientCapabilities are REQUIRED per-request — a modern server must not infer
    // capabilities from an earlier call, so omitting them on the second request
    // would be just as wrong as omitting them on the first.
    for (const Json& sent : fake->Written)
    {
        const Json meta = MetaOf(sent);
        ASSERT_TRUE(meta.is_object()) << sent.dump(2);
        EXPECT_EQ(meta.value("io.modelcontextprotocol/protocolVersion", std::string{}), "2026-07-28");
        EXPECT_TRUE(meta.contains("io.modelcontextprotocol/clientCapabilities")) << sent.dump(2);
        EXPECT_TRUE(meta["io.modelcontextprotocol/clientCapabilities"].is_object());
        EXPECT_EQ(meta["io.modelcontextprotocol/clientInfo"]["name"], "OloEditor");
    }

    ASSERT_EQ(server.ClientStatuses().size(), 1u);
    EXPECT_EQ(server.ClientStatuses()[0].Era, OloEngine::MCP::McpProtocolEra::Modern);
    EXPECT_EQ(server.ClientStatuses()[0].ProtocolVersion, "2026-07-28");
    // The tools still bridge — era negotiation must be invisible to the registry.
    EXPECT_NE(FindToolEntry(server.HandleMessage(MakeRequest(20, "tools/list")), "ext.files.read_file"), nullptr);
}

TEST(McpClientStdio, SilentProbeFallsBackToTheLegacyHandshake)
{
    // A child that swallows unknown methods entirely: no reply to server/discover.
    // The spec's compatibility matrix says a probe that TIMES OUT identifies a
    // legacy server, so this must fall back rather than fail the connect — and the
    // short probe budget is what stops that fallback costing the full handshake
    // timeout.
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const auto silentDiscover = [](const Json& request, FakeTransport& transport)
    {
        const std::string method = request.value("method", std::string{});
        if (method == "server/discover")
            return; // no reply
        if (method == "initialize")
        {
            transport.Reply(Json{ { "jsonrpc", "2.0" },
                                  { "id", request["id"] },
                                  { "result", Json{ { "protocolVersion", "2025-06-18" },
                                                    { "capabilities", Json::object() } } } });
        }
        else if (method == "tools/list")
        {
            transport.Reply(ToolsListResult(request["id"]));
        }
    };

    McpClientConfig config = FilesConfig();
    config.DiscoverProbeTimeout = std::chrono::milliseconds(50);
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(server.ConnectClientWithTransport(config, FakeFactory(fake, silentDiscover)).empty());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_EQ(server.ClientStatuses().size(), 1u);
    EXPECT_EQ(server.ClientStatuses()[0].Era, OloEngine::MCP::McpProtocolEra::Legacy);
    EXPECT_NE(FirstWritten(fake->Written, "initialize"), nullptr) << "the handshake must still run";
    // The probe budget is what bounds the fallback, not HandshakeTimeout (5 s
    // here). Asserted one-sided and generously to stay off the flake line — the
    // point is "bounded by the probe timeout", not a precise duration.
    EXPECT_LT(elapsed, std::chrono::seconds(3)) << "the fallback must be bounded by DiscoverProbeTimeout";
}

TEST(McpClientStdio, ModernChildOfferingOnlyLegacyVersionsFallsBackToTheHandshake)
{
    // A dual-era child: it answers server/discover (so it IS modern-aware) but
    // lists only handshake-era revisions. That is a fallback, not a failure.
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const auto script = [](const Json& request, FakeTransport& transport)
    {
        const std::string method = request.value("method", std::string{});
        if (method == "server/discover")
        {
            transport.Reply(Json{ { "jsonrpc", "2.0" },
                                  { "id", request["id"] },
                                  { "result", Json{ { "resultType", "complete" },
                                                    { "supportedVersions", Json::array({ "2025-11-25" }) },
                                                    { "capabilities", Json::object() },
                                                    { "ttlMs", 0 },
                                                    { "cacheScope", "public" } } } });
        }
        else if (method == "initialize")
        {
            transport.Reply(Json{ { "jsonrpc", "2.0" },
                                  { "id", request["id"] },
                                  { "result", Json{ { "protocolVersion", "2025-11-25" },
                                                    { "capabilities", Json::object() } } } });
        }
        else if (method == "tools/list")
        {
            transport.Reply(ToolsListResult(request["id"]));
        }
    };
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, script)).empty());

    ASSERT_EQ(server.ClientStatuses().size(), 1u);
    EXPECT_EQ(server.ClientStatuses()[0].Era, OloEngine::MCP::McpProtocolEra::Legacy);
    EXPECT_NE(FirstWritten(fake->Written, "initialize"), nullptr);
    // The handshake NEGOTIATES — we asked for 2025-06-18 and the child answered
    // 2025-11-25. Reporting our request rather than the agreed revision would put
    // a version nobody speaks in the panel and the log, which is the first thing
    // anyone debugging an era problem reads.
    EXPECT_EQ(server.ClientStatuses()[0].ProtocolVersion, "2025-11-25");
}

TEST(McpClientStdio, ModernMarkerWithNoUsableVersionListFallsBackInsteadOfFailing)
{
    // A -32022 whose `data` is absent or carries an empty `supported` list tells us
    // nothing actionable. Hard-failing on it would strand every tool from a child
    // whose only sin is a thin error payload, so it falls back to the handshake —
    // the same rule the DiscoverResult path uses for an uninformative version list.
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const auto script = [](const Json& request, FakeTransport& transport)
    {
        const std::string method = request.value("method", std::string{});
        if (method == "server/discover")
        {
            transport.Reply(Json{ { "jsonrpc", "2.0" },
                                  { "id", request["id"] },
                                  { "error", Json{ { "code", -32022 },
                                                   { "message", "Unsupported protocol version" } } } });
        }
        else if (method == "initialize")
        {
            transport.Reply(Json{ { "jsonrpc", "2.0" },
                                  { "id", request["id"] },
                                  { "result", Json{ { "protocolVersion", "2025-06-18" },
                                                    { "capabilities", Json::object() } } } });
        }
        else if (method == "tools/list")
        {
            transport.Reply(ToolsListResult(request["id"]));
        }
    };
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, script)).empty())
        << "an uninformative modern error must not strand the connection";

    ASSERT_EQ(server.ClientStatuses().size(), 1u);
    EXPECT_EQ(server.ClientStatuses()[0].Era, OloEngine::MCP::McpProtocolEra::Legacy);
    EXPECT_NE(FindToolEntry(server.HandleMessage(MakeRequest(23, "tools/list")), "ext.files.read_file"), nullptr);
}

TEST(McpClientStdio, UnsupportedProtocolVersionErrorIsAModernMarkerNotAFallback)
{
    // -32022 is reserved by spec 2026-07-28, so only a modern server emits it:
    // seeing it must resolve the VERSION, never send us back to the handshake.
    // Here the server names 2026-07-28 in `data.supported`, which we do speak.
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const auto script = [](const Json& request, FakeTransport& transport)
    {
        const std::string method = request.value("method", std::string{});
        if (method == "server/discover")
        {
            transport.Reply(Json{
                { "jsonrpc", "2.0" },
                { "id", request["id"] },
                { "error",
                  Json{ { "code", -32022 },
                        { "message", "Unsupported protocol version" },
                        { "data", Json{ { "supported", Json::array({ "2026-07-28" }) },
                                        { "requested", "2027-01-01" } } } } } });
        }
        else if (method == "tools/list")
        {
            transport.Reply(ToolsListResult(request["id"]));
        }
    };
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, script)).empty());

    ASSERT_EQ(server.ClientStatuses().size(), 1u);
    EXPECT_EQ(server.ClientStatuses()[0].Era, OloEngine::MCP::McpProtocolEra::Modern);
    EXPECT_EQ(FirstWritten(fake->Written, "initialize"), nullptr) << "a modern marker must not trigger the handshake";
}

TEST(McpClientStdio, NoMutuallySupportedVersionFailsWithAnActionableError)
{
    // A modern server that shares no revision with us. Failing loudly here is the
    // right answer: falling back to `initialize` against a modern-only server just
    // produces a second, less informative failure.
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const auto script = [](const Json& request, FakeTransport& transport)
    {
        if (request.value("method", std::string{}) == "server/discover")
        {
            transport.Reply(Json{ { "jsonrpc", "2.0" },
                                  { "id", request["id"] },
                                  { "result", Json{ { "resultType", "complete" },
                                                    { "supportedVersions", Json::array({ "2099-01-01" }) },
                                                    { "capabilities", Json::object() },
                                                    { "ttlMs", 0 },
                                                    { "cacheScope", "public" } } } });
        }
    };
    const std::string error = server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, script));

    ASSERT_FALSE(error.empty());
    EXPECT_NE(error.find("2099-01-01"), std::string::npos)
        << "the error must name what the server does support: " << error;
    EXPECT_TRUE(server.ClientStatuses().empty());
}

TEST(McpClientStdio, InputRequiredResultIsRefusedRatherThanForwarded)
{
    // MRTR (spec 2026-07-28): a modern server may answer a tools/call with
    // resultType "input_required" and expect elicitation/sampling plus a retry. We
    // implement none of that, so the ONE thing that must not happen is handing the
    // interim result to the agent as if it were the answer — a silent half-result
    // is exactly the failure this bridge must not have.
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const auto onCall = [](const Json& request, FakeTransport& transport)
    {
        transport.Reply(Json{ { "jsonrpc", "2.0" },
                              { "id", request["id"] },
                              { "result",
                                Json{ { "resultType", "input_required" },
                                      { "inputRequests",
                                        Json{ { "ask", Json{ { "method", "elicitation/create" } } } } } } } });
    };
    ASSERT_TRUE(
        server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, ModernScript({ "2026-07-28" }, onCall)))
            .empty());
    server.SetAllowWrites(true);

    const Json response =
        server.HandleMessage(MakeRequest(21, "tools/call", Json{ { "name", "ext.files.read_file" } }));
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    EXPECT_EQ(response["result"]["isError"], true);
    const std::string text = response["result"]["content"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("input_required"), std::string::npos) << text;
}

TEST(McpClientStdio, AbsentResultTypeStillCountsAsComplete)
{
    // Backward compatibility, stated by the spec: a result from a server on an
    // earlier revision has no `resultType`, and the client MUST read that as
    // "complete". Without this, the MRTR guard above would reject every legacy
    // tool call — a regression that would look like "the bridge stopped working".
    McpServer server{ EditorMcpContext{} };
    FakeTransport* fake = nullptr;
    const auto onCall = [](const Json& request, FakeTransport& transport)
    {
        transport.Reply(Json{ { "jsonrpc", "2.0" },
                              { "id", request["id"] },
                              { "result", Json{ { "content", Json::array({ Json{ { "type", "text" },
                                                                                 { "text", "legacy payload" } } }) } } } });
    };
    ASSERT_TRUE(server.ConnectClientWithTransport(FilesConfig(), FakeFactory(fake, WellBehavedScript(onCall))).empty());
    server.SetAllowWrites(true);

    const Json response =
        server.HandleMessage(MakeRequest(22, "tools/call", Json{ { "name", "ext.files.read_file" } }));
    ASSERT_TRUE(response.contains("result")) << response.dump(2);
    EXPECT_EQ(response["result"]["isError"], false);
    EXPECT_EQ(response["result"]["content"][0]["text"], "legacy payload");
}
