#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpReloadScriptTest — unit test (headless, no GL, no live editor, no Mono).
//
// Pins the consented MCP write tool olo_reload_script (issue #306 item C): reload
// the C# script assembly — the scripting counterpart of olo_shader_reload's inner
// loop. Two seams, the same shape as McpConsentedWriteTest / McpGenericFieldWrite:
//
//   1. The dispatch seam (McpServer.cpp, compiled into the test binary): a tool
//      flagged ToolDef::ProjectWrite is REFUSED with a clean JSON-RPC error while
//      the session "Allow writes" gate is off, and ACCEPTED once on — and the
//      reload action does NOT run while the gate is off. The server's inputSchema
//      enforcement (#423) rejects an unexpected property before the handler runs.
//      McpTools.cpp is deliberately NOT linked here, so the test registers a fake
//      tool wired to the SAME schema + result shaping the real handler uses.
//
//   2. The shared shaping (MCP/McpReloadScript.h, header-only): the empty-object
//      inputSchema + the McpScriptReloadResult -> JSON ToJson the real handler
//      emits. The reload ACTION itself is a context hook (ScriptEngine /Mono in
//      the editor), so the test injects a fake hook and never touches Mono — the
//      same indirection the real handler delegates to once on the main thread.
//
// The shared header is renderer/httplib/Mono-free, so only McpServer.h + the schema
// DSL are pulled in — no extra editor TU.
// =============================================================================

#include "MCP/McpReloadScript.h"
#include "MCP/McpServer.h"

#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpScriptReloadResult;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;
    namespace ReloadScript = OloEngine::MCP::ReloadScript;

    constexpr int kInvalidParams = -32602;

    Json MakeCallRequest(const Json& id, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "method", "tools/call" },
                     { "params", { { "name", "olo_reload_script" }, { "arguments", arguments } } } };
    }

    // Fixture: an McpServer whose only tool is a fake olo_reload_script wired through
    // the SAME schema + ToJson the real handler uses, but with the reload ACTION
    // replaced by a test-owned closure that records its invocations and returns a
    // canned McpScriptReloadResult — so the dispatch gate, schema enforcement, and
    // result shaping are exercised without Mono / a game thread (the real handler
    // marshals onto the main thread; here it runs synchronously).
    class McpReloadScriptTest : public ::testing::Test
    {
      protected:
        McpReloadScriptTest()
            : m_Server(EditorMcpContext{})
        {
            ToolDef tool;
            tool.Name = "olo_reload_script";
            tool.Description = "Reload the C# script assembly (fake; test wiring).";
            tool.ProjectWrite = true;
            tool.InputSchema = ReloadScript::InputSchema();
            tool.Handler = [this](McpServer&, const Json&) -> ToolResult
            {
                ++m_ReloadCount;
                return ToolResult::Text(ReloadScript::ToJson(m_FakeResult).dump());
            };
            m_Server.RegisterTool(std::move(tool));
        }

        McpScriptReloadResult m_FakeResult;
        int m_ReloadCount = 0;
        McpServer m_Server; // declared last → destroyed first (its handler refs members)
    };
} // namespace

// ---- the session write gate (dispatch seam) --------------------------------

// Default state: the gate is OFF, so even a well-formed (argument-less) reload call
// is refused with a JSON-RPC error and the reload action NEVER runs.
TEST_F(McpReloadScriptTest, GateOffRejectsReloadAndDoesNotInvoke)
{
    ASSERT_FALSE(m_Server.AllowWrites()); // off by default

    const Json resp = m_Server.HandleMessage(MakeCallRequest(1, Json::object()));

    ASSERT_TRUE(resp.contains("error"));
    EXPECT_FALSE(resp.contains("result"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
    EXPECT_EQ(m_ReloadCount, 0); // the reload action must not have run
}

// With the gate ON the same call succeeds, the reload action runs exactly once, and
// the structured result carries the hook's outcome (available/ok/scriptClassCount).
TEST_F(McpReloadScriptTest, GateOnInvokesReloadAndReturnsResult)
{
    m_Server.SetAllowWrites(true);
    m_FakeResult.Available = true;
    m_FakeResult.Ok = true;
    m_FakeResult.ScriptClassCount = 7;
    m_FakeResult.Message = "Reloaded the C# app assembly (7 script class(es) registered).";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(2, Json::object()));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(m_ReloadCount, 1);

    // The handler returns the ToJson payload as the result text — parse + verify shape.
    const std::string text = resp["result"]["content"][0]["text"].get<std::string>();
    const Json payload = Json::parse(text);
    EXPECT_EQ(payload["language"], "csharp");
    EXPECT_TRUE(payload["available"].get<bool>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_EQ(payload["scriptClassCount"], 7);
}

// A failed reload (scripting available, but the freshly-built app assembly did not
// load) is reported as available:true, ok:false — the call SUCCEEDS at the protocol
// level (it is not a tool error), but ok distinguishes the real outcome so an agent
// isn't told a broken reload worked.
TEST_F(McpReloadScriptTest, FailedReloadReportsOkFalse)
{
    m_Server.SetAllowWrites(true);
    m_FakeResult.Available = true;
    m_FakeResult.Ok = false;
    m_FakeResult.Message = "Reload failed: the C# app assembly did not load (see the engine log).";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(9, Json::object()));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(m_ReloadCount, 1);

    const std::string text = resp["result"]["content"][0]["text"].get<std::string>();
    const Json payload = Json::parse(text);
    EXPECT_TRUE(payload["available"].get<bool>());
    EXPECT_FALSE(payload["ok"].get<bool>());
}

// When C# scripting is unavailable (disabled in the build / not initialized) the
// call still SUCCEEDS — it is a clean, honest result reporting available:false, not
// a tool error. The agent learns scripting is off rather than getting a generic fail.
TEST_F(McpReloadScriptTest, UnavailableScriptingIsCleanResult)
{
    m_Server.SetAllowWrites(true);
    m_FakeResult.Available = false;
    m_FakeResult.Ok = false;
    m_FakeResult.Message = "C# scripting is disabled in this build (Mono not available on this platform).";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(3, Json::object()));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(m_ReloadCount, 1);

    const std::string text = resp["result"]["content"][0]["text"].get<std::string>();
    const Json payload = Json::parse(text);
    EXPECT_FALSE(payload["available"].get<bool>());
    EXPECT_FALSE(payload["ok"].get<bool>());
}

// ---- server-side inputSchema enforcement (#423), gate ON -------------------

// The schema is an empty object with no additional properties allowed, so an
// unexpected argument is rejected at the schema layer before the handler runs (and
// the reload action never fires).
TEST_F(McpReloadScriptTest, SchemaRejectsUnknownProperty)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(4, Json{ { "name", "Player" } }));

    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
    EXPECT_EQ(m_ReloadCount, 0);
}

// ---- the shared shaping core (MCP/McpReloadScript.h), no server -------------

TEST(McpReloadScriptShaping, ToJsonCarriesEveryField)
{
    McpScriptReloadResult r;
    r.Language = "csharp";
    r.Available = true;
    r.Ok = true;
    r.ScriptClassCount = 3;
    r.Message = "ok";

    const Json j = ReloadScript::ToJson(r);
    EXPECT_EQ(j["language"], "csharp");
    EXPECT_TRUE(j["available"].get<bool>());
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["scriptClassCount"], 3);
    EXPECT_EQ(j["message"], "ok");
}

TEST(McpReloadScriptShaping, InputSchemaIsClosedEmptyObject)
{
    const Json schema = ReloadScript::InputSchema();
    EXPECT_EQ(schema["type"], "object");
    // No additional properties allowed — an empty, closed object schema.
    ASSERT_TRUE(schema.contains("additionalProperties"));
    EXPECT_FALSE(schema["additionalProperties"].get<bool>());
}
