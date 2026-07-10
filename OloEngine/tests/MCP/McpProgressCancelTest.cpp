// OLO_TEST_LAYER: unit
//
// MCP progress notifications + request cancellation (issue #357 item B, spec
// 2025-06-18 / 2025-11-25 utilities):
//   * a tools/call carrying params._meta.progressToken gets
//     notifications/progress delivered through the transport-agnostic
//     notification sink (ProcessRequestBody overload) — the same seam the
//     HTTP layer's SSE-upgraded POST response uses;
//   * notifications/cancelled sets a cooperative flag the running tool
//     observes via McpServer::IsCurrentCallCancelled(); a cancelled call's
//     result is discarded (JSON-RPC error kRequestCancelled at the framing
//     level; the SSE transport suppresses the response frame entirely);
//   * the HTTP tests exercise the real wire: POST /mcp with a progressToken
//     and `Accept: text/event-stream` streams progress frames before the
//     final response frame, per the Streamable HTTP transport.
// Dispatch tests need no socket (mirrors McpDispatchTest); the HTTP tests
// bind 127.0.0.1 on a per-process derived port (mirrors McpHeadlessHost).
#include "OloEnginePCH.h"
#include <httplib.h>

#include <gtest/gtest.h>

#include "MCP/McpServer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::kRequestCancelledCode;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    // Build a tools/call request body; `progressToken` null => no _meta.
    Json MakeCallRequest(const Json& id, const std::string& tool, const Json& progressToken)
    {
        Json params = { { "name", tool }, { "arguments", Json::object() } };
        if (!progressToken.is_null())
            params["_meta"] = Json{ { "progressToken", progressToken } };
        return Json{ { "jsonrpc", "2.0" }, { "id", id }, { "method", "tools/call" }, { "params", params } };
    }

    Json MakeCancelNotification(const Json& requestId)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "method", "notifications/cancelled" },
                     { "params", { { "requestId", requestId }, { "reason", "test cancels it" } } } };
    }

    class McpProgressCancelTest : public ::testing::Test
    {
      protected:
        McpProgressCancelTest()
            : m_Server(EditorMcpContext{})
        {
            // A "slow" tool: ticks up to `steps` (default 5), emitting progress
            // each step, honouring cancellation between steps — the shape every
            // long-running frame-ticking tool follows.
            ToolDef slow;
            slow.Name = "fake_slow";
            slow.Description = "Ticks N steps with progress; honours cancellation.";
            slow.Handler = [this](McpServer& server, const Json& args) -> ToolResult
            {
                const int steps = args.value("steps", 5);
                const int stepMs = args.value("stepMs", 20);
                for (int i = 1; i <= steps; ++i)
                {
                    if (server.IsCurrentCallCancelled())
                    {
                        m_ObservedCancel.store(true);
                        return ToolResult::Error("cancelled mid-flight");
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
                    server.EmitProgress(static_cast<double>(i), static_cast<double>(steps),
                                        "step " + std::to_string(i));
                    m_StepsCompleted.store(i);
                }
                return ToolResult::Text("done");
            };
            m_Server.RegisterTool(std::move(slow));
        }

        McpServer m_Server;
        std::atomic<bool> m_ObservedCancel{ false };
        std::atomic<int> m_StepsCompleted{ 0 };
    };

    // ---- progress at the framing seam -------------------------------------------

    TEST_F(McpProgressCancelTest, ProgressNotificationsReachTheSinkWithTheToken)
    {
        std::vector<Json> notifications;
        const auto framed = m_Server.ProcessRequestBody(
            MakeCallRequest(1, "fake_slow", "tok-abc").dump(),
            [&notifications](const Json& n)
            { notifications.push_back(n); });

        ASSERT_TRUE(framed.Body.contains("result")) << framed.Body.dump(2);
        EXPECT_EQ(framed.Body["result"]["isError"], false);

        ASSERT_EQ(notifications.size(), 5u);
        double lastProgress = 0.0;
        for (const Json& n : notifications)
        {
            EXPECT_EQ(n["jsonrpc"], "2.0");
            EXPECT_EQ(n["method"], "notifications/progress");
            EXPECT_FALSE(n.contains("id")) << "progress is a notification";
            EXPECT_EQ(n["params"]["progressToken"], "tok-abc");
            const double progress = n["params"]["progress"].get<double>();
            EXPECT_GT(progress, lastProgress) << "progress must increase";
            lastProgress = progress;
            EXPECT_EQ(n["params"]["total"], 5.0);
            EXPECT_TRUE(n["params"].contains("message"));
        }
    }

    TEST_F(McpProgressCancelTest, IntegerProgressTokenIsEchoedVerbatim)
    {
        std::vector<Json> notifications;
        const auto framed = m_Server.ProcessRequestBody(
            MakeCallRequest(7, "fake_slow", 42).dump(),
            [&notifications](const Json& n)
            { notifications.push_back(n); });

        ASSERT_TRUE(framed.Body.contains("result"));
        ASSERT_FALSE(notifications.empty());
        EXPECT_TRUE(notifications.front()["params"]["progressToken"].is_number_integer());
        EXPECT_EQ(notifications.front()["params"]["progressToken"], 42);
    }

    TEST_F(McpProgressCancelTest, NoTokenMeansNoNotificationsAndEmitIsANoOp)
    {
        std::vector<Json> notifications;
        const auto framed = m_Server.ProcessRequestBody(
            MakeCallRequest(2, "fake_slow", Json(nullptr)).dump(),
            [&notifications](const Json& n)
            { notifications.push_back(n); });

        ASSERT_TRUE(framed.Body.contains("result"));
        EXPECT_EQ(framed.Body["result"]["isError"], false);
        EXPECT_TRUE(notifications.empty()) << "EmitProgress without a progressToken must be a no-op";
    }

    TEST_F(McpProgressCancelTest, EmitProgressOutsideAnyCallIsANoOp)
    {
        // No thread-local call scope on this thread — must not crash or leak.
        m_Server.EmitProgress(1.0, 2.0, "outside any call");
        EXPECT_FALSE(m_Server.IsCurrentCallCancelled());
    }

    // ---- cancellation ------------------------------------------------------------

    TEST_F(McpProgressCancelTest, CancelledMidFlightStopsTheToolAndDiscardsTheResult)
    {
        // Slow enough that the cancel lands mid-run: 50 steps x 20 ms = 1 s max.
        const Json request = Json{ { "jsonrpc", "2.0" },
                                   { "id", "req-9" },
                                   { "method", "tools/call" },
                                   { "params",
                                     { { "name", "fake_slow" },
                                       { "arguments", { { "steps", 50 }, { "stepMs", 20 } } } } } };

        Json framedBody;
        std::thread worker(
            [this, &request, &framedBody]
            {
                const auto framed = m_Server.ProcessRequestBody(request.dump(), {});
                framedBody = framed.Body;
            });

        // Let a few steps run, then cancel by the SAME id value.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const Json response = m_Server.HandleMessage(MakeCancelNotification("req-9"));
        EXPECT_TRUE(response.is_null()) << "a notification gets no response";

        worker.join();

        EXPECT_TRUE(m_ObservedCancel.load()) << "tool must observe IsCurrentCallCancelled()";
        EXPECT_LT(m_StepsCompleted.load(), 50) << "tool must stop early";
        ASSERT_TRUE(framedBody.contains("error")) << framedBody.dump(2);
        EXPECT_EQ(framedBody["error"]["code"], kRequestCancelledCode);
        EXPECT_EQ(framedBody["id"], "req-9");
    }

    TEST_F(McpProgressCancelTest, CancelForUnknownOrFinishedRequestIsIgnored)
    {
        const Json response = m_Server.HandleMessage(MakeCancelNotification("never-existed"));
        EXPECT_TRUE(response.is_null());

        // A normal call still works afterwards (registry state is clean).
        const auto framed = m_Server.ProcessRequestBody(
            MakeCallRequest(3, "fake_slow", Json(nullptr)).dump(), {});
        ASSERT_TRUE(framed.Body.contains("result"));
        EXPECT_EQ(framed.Body["result"]["isError"], false);
    }

    TEST_F(McpProgressCancelTest, MismatchedIdTypeDoesNotCancel)
    {
        // JSON-RPC ids match by value INCLUDING type: cancelling id 5 (number)
        // must not cancel a call whose id is "5" (string).
        Json framedBody;
        std::thread worker(
            [this, &framedBody]
            {
                const Json request = Json{ { "jsonrpc", "2.0" },
                                           { "id", "5" },
                                           { "method", "tools/call" },
                                           { "params",
                                             { { "name", "fake_slow" },
                                               { "arguments", { { "steps", 10 }, { "stepMs", 20 } } } } } };
                framedBody = m_Server.ProcessRequestBody(request.dump(), {}).Body;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        (void)m_Server.HandleMessage(MakeCancelNotification(5)); // number, not string
        worker.join();

        ASSERT_TRUE(framedBody.contains("result")) << framedBody.dump(2);
        EXPECT_EQ(framedBody["result"]["isError"], false) << "type-mismatched cancel must not apply";
        EXPECT_FALSE(m_ObservedCancel.load());
    }

    // ---- the HTTP wire: SSE-upgraded POST responses -------------------------------

    // Split an SSE stream into its `data:` payloads.
    std::vector<Json> ParseSseData(const std::string& body)
    {
        std::vector<Json> out;
        std::size_t pos = 0;
        while ((pos = body.find("data: ", pos)) != std::string::npos)
        {
            pos += 6;
            const std::size_t end = body.find('\n', pos);
            out.push_back(Json::parse(body.substr(pos, end - pos)));
            pos = end;
        }
        return out;
    }

    class McpProgressHttpTest : public McpProgressCancelTest
    {
      protected:
        // Derive a per-process base port (same trick as McpHeadlessHost) and
        // sweep forward until a bind succeeds.
        u16 StartServer()
        {
            static int anchor = 0;
            const auto base = static_cast<u16>(21000 + (reinterpret_cast<std::uintptr_t>(&anchor) % 30000u));
            for (u16 i = 0; i < 32; ++i)
            {
                if (m_Server.Start(static_cast<u16>(base + i)))
                    return static_cast<u16>(base + i);
            }
            return 0;
        }

        void TearDown() override
        {
            m_Server.Stop();
        }
    };

    TEST_F(McpProgressHttpTest, PostWithProgressTokenStreamsSseThenFinalResponse)
    {
        const u16 port = StartServer();
        ASSERT_NE(port, 0);

        httplib::Client client("127.0.0.1", port);
        client.set_read_timeout(30, 0);
        const httplib::Headers headers = {
            { "Authorization", "Bearer " + m_Server.GetToken() },
            { "Accept", "application/json, text/event-stream" },
        };

        std::string streamed;
        auto res = client.Post(
            "/mcp", headers, MakeCallRequest("http-1", "fake_slow", "tok-http").dump(), "application/json",
            [&streamed](const char* data, std::size_t length)
            {
                streamed.append(data, length);
                return true;
            });

        ASSERT_TRUE(res) << "POST failed: " << httplib::to_string(res.error());
        EXPECT_EQ(res->status, 200);
        EXPECT_EQ(res->get_header_value("Content-Type"), "text/event-stream");

        const std::vector<Json> frames = ParseSseData(streamed);
        ASSERT_GE(frames.size(), 2u) << streamed;
        // Progress frames first...
        for (std::size_t i = 0; i + 1 < frames.size(); ++i)
        {
            EXPECT_EQ(frames[i]["method"], "notifications/progress");
            EXPECT_EQ(frames[i]["params"]["progressToken"], "tok-http");
        }
        // ...final response frame last.
        const Json& last = frames.back();
        EXPECT_EQ(last["id"], "http-1");
        ASSERT_TRUE(last.contains("result")) << last.dump(2);
        EXPECT_EQ(last["result"]["isError"], false);
    }

    TEST_F(McpProgressHttpTest, PostWithoutTokenStaysPlainJson)
    {
        const u16 port = StartServer();
        ASSERT_NE(port, 0);

        httplib::Client client("127.0.0.1", port);
        const httplib::Headers headers = {
            { "Authorization", "Bearer " + m_Server.GetToken() },
            { "Accept", "application/json, text/event-stream" },
        };
        auto res = client.Post("/mcp", headers,
                               MakeCallRequest("http-2", "fake_slow", Json(nullptr)).dump(),
                               "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
        EXPECT_EQ(res->get_header_value("Content-Type"), "application/json");
        const Json body = Json::parse(res->body);
        EXPECT_EQ(body["id"], "http-2");
        EXPECT_TRUE(body.contains("result"));
    }

    TEST_F(McpProgressHttpTest, CancelOverHttpSuppressesTheSseResponseFrame)
    {
        const u16 port = StartServer();
        ASSERT_NE(port, 0);

        const httplib::Headers headers = {
            { "Authorization", "Bearer " + m_Server.GetToken() },
            { "Accept", "application/json, text/event-stream" },
        };

        const Json request = Json{ { "jsonrpc", "2.0" },
                                   { "id", "http-3" },
                                   { "method", "tools/call" },
                                   { "params",
                                     { { "name", "fake_slow" },
                                       { "arguments", { { "steps", 100 }, { "stepMs", 20 } } },
                                       { "_meta", { { "progressToken", "tok-cancel" } } } } } };

        std::string streamed;
        std::thread caller(
            [&]
            {
                httplib::Client client("127.0.0.1", port);
                client.set_read_timeout(30, 0);
                (void)client.Post("/mcp", headers, request.dump(), "application/json",
                                  [&streamed](const char* data, std::size_t length)
                                  {
                                      streamed.append(data, length);
                                      return true;
                                  });
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        httplib::Client canceller("127.0.0.1", port);
        auto res = canceller.Post("/mcp", headers, MakeCancelNotification("http-3").dump(),
                                  "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 202) << "a notification returns 202 Accepted with no body";

        caller.join();

        EXPECT_TRUE(m_ObservedCancel.load());
        const std::vector<Json> frames = ParseSseData(streamed);
        for (const Json& frame : frames)
        {
            EXPECT_EQ(frame.value("method", std::string{}), "notifications/progress")
                << "a cancelled call's stream must carry no response frame, got: " << frame.dump(2);
        }
    }
} // namespace
