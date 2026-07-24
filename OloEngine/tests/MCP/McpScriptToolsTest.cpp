// OLO_TEST_LAYER: unit
//
// Script-defined MCP tools (issue #357, Lua v1) — the sandboxed registration
// + dispatch path in OloEditor/src/MCP/McpScriptTools.cpp, exercised entirely
// headlessly: a temp directory of .lua files is loaded into an in-process
// McpServer (never Start()ed — dispatch via HandleMessage, like
// McpDispatchTest), so no editor, socket, or GL is required. Pins the ADR's
// contract (docs/adr/0005-mcp-script-tools-lua-sandbox.md):
//   * a valid RegisterMcpTool{} call yields a schema-enforced, listed tool
//     with readOnlyHint:true in the reserved script_* namespace;
//   * the sandbox has no io/os/require and no file/code loaders;
//   * Lua runtime errors surface as isError results, never exceptions/crashes;
//   * the watchdog bounds a runaway handler;
//   * olo.call_tool reaches read-only tools and refuses ProjectWrite tools.
//
// Issue #607 extends the contract, and this file with it:
//   * WRITE-TIER script tools (`writes = true`) are ProjectWrite — refused when
//     the session's write consent is Disabled, allowed under AllowSession — and
//     only they may reach a ProjectWrite tool through olo.call_tool; a read-only
//     script tool cannot, not even by calling a write-tier script tool;
//   * the per-state MEMORY QUOTA refuses an allocation bomb as a clean tool error;
//   * a LIVE rescan (with the server "serving") swaps the tool list, bumps the
//     generation, and fires notifications/tools/list_changed;
//   * `icons` round-trip from RegisterMcpTool{} into tools/list.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpScriptTools.h"
#include "MCP/McpServer.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::LoadScriptTools;
    using OloEngine::MCP::McpScriptToolsReport;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    class McpScriptToolsTest : public ::testing::Test
    {
      protected:
        McpScriptToolsTest()
            : m_Server(EditorMcpContext{})
        {
            const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
            m_Dir = std::filesystem::temp_directory_path() /
                    (std::string("olo-mcp-script-tools-") + info->name());
            std::filesystem::remove_all(m_Dir);
            std::filesystem::create_directories(m_Dir);

            // A native read-only tool + a native write tool, for the bridge tests.
            ToolDef echo;
            echo.Name = "fake_echo";
            echo.Description = "Echo back the 'text' argument.";
            echo.InputSchema = Json{ { "type", "object" },
                                     { "properties", { { "text", { { "type", "string" } } } } } };
            echo.Handler = [](McpServer&, const Json& args)
            { return ToolResult::Structured(Json{ { "echoed", args.value("text", std::string{}) } }); };
            m_Server.RegisterTool(std::move(echo));

            ToolDef write;
            write.Name = "fake_write";
            write.Description = "A project-mutating tool.";
            write.ProjectWrite = true;
            write.Handler = [](McpServer&, const Json&)
            { return ToolResult::Text("wrote"); };
            m_Server.RegisterTool(std::move(write));
        }

        ~McpScriptToolsTest() override
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Dir, ec);
        }

        void WriteScript(const std::string& filename, const std::string& body)
        {
            std::ofstream out(m_Dir / filename, std::ios::binary);
            out << body;
        }

        [[nodiscard]] McpScriptToolsReport Load(std::chrono::milliseconds budget = std::chrono::milliseconds(10000),
                                                std::size_t memoryBudget = OloEngine::MCP::kDefaultScriptToolsMemoryBudget)
        {
            return LoadScriptTools(m_Server, m_Dir, budget, memoryBudget);
        }

        [[nodiscard]] Json CallTool(const std::string& name, const Json& args = Json::object())
        {
            const Json request = { { "jsonrpc", "2.0" },
                                   { "id", 1 },
                                   { "method", "tools/call" },
                                   { "params", { { "name", name }, { "arguments", args } } } };
            return m_Server.HandleMessage(request);
        }

        [[nodiscard]] Json ToolsList()
        {
            return m_Server.HandleMessage({ { "jsonrpc", "2.0" }, { "id", 2 }, { "method", "tools/list" } });
        }

        McpServer m_Server;
        std::filesystem::path m_Dir;
    };

    // ---- registration ---------------------------------------------------------

    TEST_F(McpScriptToolsTest, RegistersToolAndAppearsInToolsList)
    {
        WriteScript("digest.lua", R"lua(
RegisterMcpTool{
    name = "script_digest",
    title = "Scene digest",
    description = "A tiny scripted digest tool.",
    schema = { type = "object",
               properties = { count = { type = "integer", minimum = 1 } },
               additionalProperties = false },
    handler = function(args)
        return { doubled = (args.count or 1) * 2 }
    end,
}
)lua");
        const McpScriptToolsReport report = Load();
        EXPECT_EQ(report.ToolsRegistered, 1);
        EXPECT_EQ(report.FilesLoaded, 1);
        EXPECT_EQ(report.Failures, 0);

        const Json list = ToolsList();
        const Json& tools = list["result"]["tools"];
        const auto it = std::find_if(tools.begin(), tools.end(),
                                     [](const Json& t)
                                     { return t["name"] == "script_digest"; });
        ASSERT_NE(it, tools.end());
        EXPECT_EQ((*it)["title"], "Scene digest");
        EXPECT_EQ((*it)["annotations"]["readOnlyHint"], true);
        EXPECT_EQ((*it)["_meta"]["io.oloengine/toolset"], "script");
        EXPECT_EQ((*it)["inputSchema"]["type"], "object");
    }

    TEST_F(McpScriptToolsTest, CallReturnsStructuredResultAndEnforcesSchema)
    {
        WriteScript("digest.lua", R"lua(
RegisterMcpTool{
    name = "script_digest",
    description = "doubles count",
    schema = { type = "object",
               properties = { count = { type = "integer", minimum = 1 } },
               required = { "count" },
               additionalProperties = false },
    handler = function(args) return { doubled = args.count * 2 } end,
}
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 1);

        const Json ok = CallTool("script_digest", Json{ { "count", 21 } });
        ASSERT_TRUE(ok.contains("result")) << ok.dump(2);
        EXPECT_EQ(ok["result"]["isError"], false);
        EXPECT_EQ(ok["result"]["structuredContent"]["doubled"], 42);

        // Schema enforcement happens in dispatch BEFORE the Lua handler runs —
        // the same path native tools go through (#423). SEP-1303 (protocol
        // 2025-11-25): input-validation failures are TOOL errors so the model
        // can self-correct, not protocol errors.
        const Json bad = CallTool("script_digest", Json{ { "count", "nope" } });
        ASSERT_TRUE(bad.contains("result")) << bad.dump(2);
        EXPECT_EQ(bad["result"]["isError"], true);
    }

    TEST_F(McpScriptToolsTest, RejectsBadNamesDuplicatesAndMissingHandler)
    {
        WriteScript("bad.lua", R"lua(
RegisterMcpTool{ name = "olo_spoof", description = "wrong prefix", handler = function() return 1 end }
RegisterMcpTool{ name = "script_UPPER", description = "bad chars", handler = function() return 1 end }
RegisterMcpTool{ name = "script_ok", description = "fine", handler = function() return 1 end }
RegisterMcpTool{ name = "script_ok", description = "duplicate", handler = function() return 2 end }
RegisterMcpTool{ name = "script_nohandler", description = "no handler" }
)lua");
        const McpScriptToolsReport report = Load();
        EXPECT_EQ(report.ToolsRegistered, 1); // only script_ok
        EXPECT_EQ(report.Failures, 4);
    }

    // ---- sandbox --------------------------------------------------------------

    TEST_F(McpScriptToolsTest, SandboxHasNoIoOsRequireOrLoaders)
    {
        // Escape attempts at LOAD time: touching io/os/require/dofile/loadfile/
        // load must fail the script (they are nil), and nothing gets registered.
        WriteScript("escape_io.lua", "io.open('secrets.txt', 'r')\n");
        WriteScript("escape_os.lua", "os.execute('calc.exe')\n");
        WriteScript("escape_require.lua", "require('socket')\n");
        WriteScript("escape_load.lua", "load('return 1')()\n");
        // A registered tool whose HANDLER attempts an escape: registration
        // succeeds, the call returns isError (never a crash).
        WriteScript("escape_in_handler.lua", R"lua(
RegisterMcpTool{ name = "script_escape", description = "tries io at call time",
                 handler = function() return io.open("x") end }
)lua");
        const McpScriptToolsReport report = Load();
        EXPECT_EQ(report.ToolsRegistered, 1);
        EXPECT_EQ(report.Failures, 4) << "each escape script must fail to load";

        const Json result = CallTool("script_escape");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["isError"], true);
    }

    TEST_F(McpScriptToolsTest, LuaRuntimeErrorBecomesIsErrorResult)
    {
        WriteScript("boom.lua", R"lua(
RegisterMcpTool{ name = "script_boom", description = "always errors",
                 handler = function() error("kaboom from lua") end }
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 1);

        const Json result = CallTool("script_boom");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["isError"], true);
        const std::string text = result["result"]["content"][0]["text"].get<std::string>();
        EXPECT_NE(text.find("kaboom from lua"), std::string::npos) << text;
    }

    TEST_F(McpScriptToolsTest, WatchdogStopsRunawayHandler)
    {
        WriteScript("spin.lua", R"lua(
RegisterMcpTool{ name = "script_spin", description = "loops forever",
                 handler = function() while true do end end }
)lua");
        ASSERT_EQ(Load(std::chrono::milliseconds(150)).ToolsRegistered, 1);

        const auto start = std::chrono::steady_clock::now();
        const Json result = CallTool("script_spin");
        const auto elapsed = std::chrono::steady_clock::now() - start;

        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["isError"], true);
        const std::string text = result["result"]["content"][0]["text"].get<std::string>();
        EXPECT_NE(text.find("time budget"), std::string::npos) << text;
        EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5000)
            << "watchdog must abort a runaway loop promptly";
    }

    // ---- the olo.call_tool bridge ----------------------------------------------

    TEST_F(McpScriptToolsTest, BridgeCallsReadOnlyToolAndReturnsItsResult)
    {
        WriteScript("compose.lua", R"lua(
RegisterMcpTool{ name = "script_compose", description = "wraps fake_echo",
    handler = function(args)
        local result, err = olo.call_tool("fake_echo", { text = "hello" })
        if err then error(err) end
        return { inner = result.echoed }
    end }
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 1);

        const Json result = CallTool("script_compose");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["isError"], false) << result.dump(2);
        EXPECT_EQ(result["result"]["structuredContent"]["inner"], "hello");
    }

    TEST_F(McpScriptToolsTest, BridgeRefusesProjectWriteTools)
    {
        WriteScript("sneak.lua", R"lua(
RegisterMcpTool{ name = "script_sneak", description = "tries a write tool",
    handler = function()
        local result, err = olo.call_tool("fake_write", {})
        return { blocked = (result == nil), why = err }
    end }
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 1);

        const Json result = CallTool("script_sneak");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["isError"], false) << result.dump(2);
        EXPECT_EQ(result["result"]["structuredContent"]["blocked"], true);
        const std::string why = result["result"]["structuredContent"]["why"].get<std::string>();
        EXPECT_NE(why.find("read-only"), std::string::npos) << why;
    }

    TEST_F(McpScriptToolsTest, MissingDirectoryIsACleanNoOp)
    {
        McpServer server{ EditorMcpContext{} };
        const McpScriptToolsReport report =
            LoadScriptTools(server, m_Dir / "does-not-exist");
        EXPECT_EQ(report.ToolsRegistered, 0);
        EXPECT_EQ(report.FilesLoaded, 0);
        EXPECT_EQ(report.Failures, 0);
        EXPECT_TRUE(server.ToolsSnapshot()->empty());
    }

    // ---- write-tier script tools (issue #607) ----------------------------------

    // `writes = true` opts a script tool into the ProjectWrite class, which is the
    // ONLY thing that changes: the existing HandleToolsCall consent gate does the
    // rest. Disabled (the default) refuses it with a protocol error; AllowSession
    // lets it run — no new consent code exists for script tools.
    TEST_F(McpScriptToolsTest, WriteTierToolIsRefusedWhenWritesDisabledAndRunsUnderAllowSession)
    {
        WriteScript("write.lua", R"lua(
RegisterMcpTool{ name = "script_writer", description = "a write-tier tool", writes = true,
    handler = function()
        local result, err = olo.call_tool("fake_write", {})
        if err then error(err) end
        return { wrote = result }
    end }
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 1);

        // It advertises itself honestly: readOnlyHint:false.
        const Json list = ToolsList();
        const Json& tools = list["result"]["tools"];
        const auto it = std::find_if(tools.begin(), tools.end(),
                                     [](const Json& t)
                                     { return t["name"] == "script_writer"; });
        ASSERT_NE(it, tools.end());
        EXPECT_EQ((*it)["annotations"]["readOnlyHint"], false);

        // Default consent mode is Disabled -> the dispatch gate refuses it outright.
        ASSERT_FALSE(m_Server.AllowWrites());
        const Json refused = CallTool("script_writer");
        ASSERT_TRUE(refused.contains("error")) << refused.dump(2);
        EXPECT_NE(refused["error"]["message"].get<std::string>().find("disabled"), std::string::npos)
            << refused.dump(2);

        // Human opts in for the session -> the same call runs, and its inner
        // ProjectWrite call_tool now succeeds.
        m_Server.SetAllowWrites(true);
        const Json allowed = CallTool("script_writer");
        ASSERT_TRUE(allowed.contains("result")) << allowed.dump(2);
        EXPECT_EQ(allowed["result"]["isError"], false) << allowed.dump(2);
        EXPECT_EQ(allowed["result"]["structuredContent"]["wrote"], "wrote");
    }

    // The security rule: write authority is the EXECUTING tool's own declared tier.
    // A read-only script tool may not reach a write tool directly, NOR launder one
    // through a write-tier script tool — because a write-tier script tool is itself
    // ProjectWrite, so the very same bridge check refuses it. Writes are ON for the
    // whole test, so the only thing standing between the read-only tool and the
    // mutation is the bridge rule.
    TEST_F(McpScriptToolsTest, ReadOnlyScriptToolCannotReachAWriteToolByAnyPath)
    {
        WriteScript("tiers.lua", R"lua(
RegisterMcpTool{ name = "script_writer", description = "write tier", writes = true,
    handler = function() return { ok = olo.call_tool("fake_write", {}) } end }

RegisterMcpTool{ name = "script_reader", description = "read-only, tries both routes",
    handler = function()
        local direct, directErr = olo.call_tool("fake_write", {})
        local viaWriter, viaWriterErr = olo.call_tool("script_writer", {})
        return { directBlocked = (direct == nil), directWhy = directErr,
                 launderBlocked = (viaWriter == nil), launderWhy = viaWriterErr }
    end }
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 2);
        m_Server.SetAllowWrites(true); // the session PERMITS writes...

        const Json result = CallTool("script_reader");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        const Json& out = result["result"]["structuredContent"];

        // ...and the read-only script tool still reaches no mutation, by any path.
        EXPECT_EQ(out["directBlocked"], true) << result.dump(2);
        EXPECT_NE(out["directWhy"].get<std::string>().find("read-only"), std::string::npos);
        EXPECT_EQ(out["launderBlocked"], true) << result.dump(2);
        EXPECT_NE(out["launderWhy"].get<std::string>().find("read-only"), std::string::npos);
    }

    // Authority is not AMBIENT either: a write-tier tool calling a read-only script
    // tool does not lend it the write capability (WatchdogScope installs the inner
    // tool's own tier and restores the outer one on unwind).
    TEST_F(McpScriptToolsTest, WriteAuthorityIsNotInheritedByANestedReadOnlyScriptTool)
    {
        WriteScript("nested.lua", R"lua(
RegisterMcpTool{ name = "script_inner_reader", description = "read-only inner",
    handler = function()
        local result, err = olo.call_tool("fake_write", {})
        return { innerBlocked = (result == nil), innerWhy = err }
    end }

RegisterMcpTool{ name = "script_outer_writer", description = "write tier outer", writes = true,
    handler = function()
        local inner, innerErr = olo.call_tool("script_inner_reader", {})
        if innerErr then error(innerErr) end
        -- The outer tool's OWN authority is restored after the nested call.
        local mine, mineErr = olo.call_tool("fake_write", {})
        return { inner = inner, outerWroteAfterwards = (mine ~= nil), outerWhy = mineErr }
    end }
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 2);
        m_Server.SetAllowWrites(true);

        const Json result = CallTool("script_outer_writer");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["isError"], false) << result.dump(2);
        const Json& out = result["result"]["structuredContent"];
        EXPECT_EQ(out["inner"]["innerBlocked"], true) << result.dump(2);
        EXPECT_EQ(out["outerWroteAfterwards"], true) << result.dump(2);
    }

    // A write-tier tool's inner writes are re-checked against the LIVE consent mode,
    // not just the one that was in force when dispatch admitted the call.
    TEST_F(McpScriptToolsTest, WriteTierBridgeRefusesWhenConsentIsRevokedMidCall)
    {
        WriteScript("revoke.lua", R"lua(
RegisterMcpTool{ name = "script_revoked", description = "writes after consent is revoked", writes = true,
    handler = function()
        olo.call_tool("fake_revoke_writes", {})   -- flips the session back to Disabled
        local result, err = olo.call_tool("fake_write", {})
        return { blocked = (result == nil), why = err }
    end }
)lua");
        ToolDef revoke;
        revoke.Name = "fake_revoke_writes";
        revoke.Description = "Turn the session write consent back off (test hook).";
        revoke.Handler = [](McpServer& server, const Json&)
        {
            server.SetAllowWrites(false);
            return ToolResult::Text("revoked");
        };
        m_Server.RegisterTool(std::move(revoke));

        ASSERT_EQ(Load().ToolsRegistered, 1);
        m_Server.SetAllowWrites(true);

        const Json result = CallTool("script_revoked");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["structuredContent"]["blocked"], true) << result.dump(2);
    }

    TEST_F(McpScriptToolsTest, NonBooleanWritesFlagIsRejectedRatherThanSilentlyDowngraded)
    {
        WriteScript("typo.lua", R"lua(
RegisterMcpTool{ name = "script_typo", description = "writes is a string", writes = "yes",
                 handler = function() return 1 end }
)lua");
        const McpScriptToolsReport report = Load();
        EXPECT_EQ(report.ToolsRegistered, 0);
        EXPECT_EQ(report.Failures, 1);
    }

    // ---- memory quota (issue #607) ---------------------------------------------

    // The watchdog bounds TIME, not allocation: without a quota, this handler grows
    // the editor's heap without limit. The custom lua_Alloc refuses the allocation
    // that would cross the budget, Lua raises a normal memory error, and it surfaces
    // as a tool error — the process survives (this test running to completion IS the
    // "editor survives" assertion).
    TEST_F(McpScriptToolsTest, AllocationBombIsRefusedByTheMemoryQuota)
    {
        WriteScript("bomb.lua", R"lua(
RegisterMcpTool{ name = "script_bomb", description = "allocation bomb",
    handler = function()
        local t = {}
        while true do t[#t + 1] = string.rep("x", 1000000) end
    end }
)lua");
        // 4 MiB of headroom: a couple of 1 MB strings and the quota trips, long
        // before the (10 s) time budget could.
        ASSERT_EQ(Load(std::chrono::milliseconds(10000), 4ull * 1024 * 1024).ToolsRegistered, 1);

        const Json result = CallTool("script_bomb");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["isError"], true) << result.dump(2);
        const std::string text = result["result"]["content"][0]["text"].get<std::string>();
        EXPECT_NE(text.find("memory"), std::string::npos) << text;
    }

    // The quota is armed AFTER the sandbox is built, so a small budget bounds the
    // SCRIPTS without ever starving the interpreter's own boot: a modest tool still
    // loads and runs under a budget far below what the state itself costs.
    TEST_F(McpScriptToolsTest, SmallMemoryBudgetStillLoadsAndRunsAModestTool)
    {
        WriteScript("modest.lua", R"lua(
RegisterMcpTool{ name = "script_modest", description = "allocates a little",
    handler = function() return { text = string.rep("x", 1024) } end }
)lua");
        ASSERT_EQ(Load(std::chrono::milliseconds(10000), 1024ull * 1024).ToolsRegistered, 1);

        const Json result = CallTool("script_modest");
        ASSERT_TRUE(result.contains("result")) << result.dump(2);
        EXPECT_EQ(result["result"]["isError"], false) << result.dump(2);
        EXPECT_EQ(result["result"]["structuredContent"]["text"].get<std::string>().size(), 1024u);
    }

    // ---- live reload (issue #607) ----------------------------------------------

    // A rescan REPLACES the script tools atomically: additions appear, deletions and
    // renames disappear, the generation counter moves, and a
    // notifications/tools/list_changed goes out to every listener.
    TEST_F(McpScriptToolsTest, LiveRescanSwapsTheToolListAndFiresListChanged)
    {
        std::vector<Json> notifications;
        const u64 token = m_Server.AddNotificationListener(
            [&notifications](const Json& notification)
            { notifications.push_back(notification); });

        WriteScript("v1.lua", R"lua(
RegisterMcpTool{ name = "script_v1", description = "first version",
                 handler = function() return { version = 1 } end }
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 1);
        const u64 generationAfterFirstLoad = m_Server.ToolsGeneration();
        ASSERT_EQ(notifications.size(), 1u);
        EXPECT_EQ(notifications.front()["method"], "notifications/tools/list_changed");
        EXPECT_EQ(notifications.front()["jsonrpc"], "2.0");

        const auto hasTool = [this](const std::string& name)
        {
            const Json list = ToolsList();
            const Json& tools = list["result"]["tools"];
            return std::any_of(tools.begin(), tools.end(),
                               [&name](const Json& t)
                               { return t["name"] == name; });
        };
        EXPECT_TRUE(hasTool("script_v1"));

        // The author edits the directory: script_v1 is replaced by script_v2.
        std::filesystem::remove(m_Dir / "v1.lua");
        WriteScript("v2.lua", R"lua(
RegisterMcpTool{ name = "script_v2", description = "second version",
                 handler = function() return { version = 2 } end }
)lua");
        const McpScriptToolsReport rescan = Load();
        EXPECT_EQ(rescan.ToolsRegistered, 1);

        EXPECT_FALSE(hasTool("script_v1")) << "a deleted script's tool must disappear";
        EXPECT_TRUE(hasTool("script_v2"));
        // The native tools registered by the fixture are untouched by the swap.
        EXPECT_TRUE(hasTool("fake_echo"));
        EXPECT_TRUE(hasTool("fake_write"));

        EXPECT_GT(m_Server.ToolsGeneration(), generationAfterFirstLoad);
        ASSERT_EQ(notifications.size(), 2u);
        EXPECT_EQ(notifications.back()["method"], "notifications/tools/list_changed");

        const Json call = CallTool("script_v2");
        ASSERT_TRUE(call.contains("result")) << call.dump(2);
        EXPECT_EQ(call["result"]["structuredContent"]["version"], 2);

        m_Server.RemoveNotificationListener(token);
        const McpScriptToolsReport afterRemoval = Load();
        EXPECT_EQ(afterRemoval.ToolsRegistered, 1);
        EXPECT_EQ(notifications.size(), 2u) << "a removed listener must stop receiving";
    }

    // The olo_script_tools_reload tool an agent calls to trigger the rescan itself.
    TEST_F(McpScriptToolsTest, ReloadToolRescansTheDirectory)
    {
        OloEngine::MCP::RegisterScriptToolsReloadTool(m_Server, m_Dir);
        ASSERT_EQ(Load().ToolsRegistered, 0);

        WriteScript("late.lua", R"lua(
RegisterMcpTool{ name = "script_late", description = "added after the server came up",
                 handler = function() return { late = true } end }
)lua");

        const Json reload = CallTool("olo_script_tools_reload");
        ASSERT_TRUE(reload.contains("result")) << reload.dump(2);
        EXPECT_EQ(reload["result"]["isError"], false) << reload.dump(2);
        EXPECT_EQ(reload["result"]["structuredContent"]["toolsRegistered"], 1);
        EXPECT_EQ(reload["result"]["structuredContent"]["failures"], 0);

        const Json call = CallTool("script_late");
        ASSERT_TRUE(call.contains("result")) << call.dump(2);
        EXPECT_EQ(call["result"]["structuredContent"]["late"], true);

        // The reload tool is native, not ScriptOwned — a rescan must not delete it.
        const Json again = CallTool("olo_script_tools_reload");
        ASSERT_TRUE(again.contains("result")) << again.dump(2);
    }

    // ---- icons (SEP-973, issue #607) -------------------------------------------

    TEST_F(McpScriptToolsTest, ScriptToolIconsRoundTripThroughToolsList)
    {
        WriteScript("icon.lua", R"lua(
RegisterMcpTool{ name = "script_icon", description = "has an icon",
    icons = { { src = "data:image/svg+xml;base64,PHN2Zy8+", mimeType = "image/svg+xml", sizes = { "any" } } },
    handler = function() return { ok = true } end }
RegisterMcpTool{ name = "script_no_icon", description = "has none",
                 handler = function() return { ok = true } end }
RegisterMcpTool{ name = "script_bad_icon", description = "malformed icons",
                 icons = { { mimeType = "image/png" } },
                 handler = function() return { ok = true } end }
)lua");
        const McpScriptToolsReport report = Load();
        EXPECT_EQ(report.ToolsRegistered, 2);
        EXPECT_EQ(report.Failures, 1) << "an icon without a 'src' must be rejected";

        const Json list = ToolsList();
        const Json& tools = list["result"]["tools"];

        const auto find = [&tools](const std::string& name)
        { return std::find_if(tools.begin(), tools.end(),
                              [&name](const Json& t)
                              { return t["name"] == name; }); };

        const auto withIcon = find("script_icon");
        ASSERT_NE(withIcon, tools.end());
        ASSERT_TRUE((*withIcon).contains("icons"));
        ASSERT_TRUE((*withIcon)["icons"].is_array());
        ASSERT_EQ((*withIcon)["icons"].size(), 1u);
        EXPECT_EQ((*withIcon)["icons"][0]["src"], "data:image/svg+xml;base64,PHN2Zy8+");
        EXPECT_EQ((*withIcon)["icons"][0]["mimeType"], "image/svg+xml");
        EXPECT_EQ((*withIcon)["icons"][0]["sizes"][0], "any");

        // Never emit an empty `icons` key for a tool that declared none.
        const auto withoutIcon = find("script_no_icon");
        ASSERT_NE(withoutIcon, tools.end());
        EXPECT_FALSE((*withoutIcon).contains("icons"));
    }

    // ---- outbound-client tools are unreachable from scripts (#673 Tier 1) -----

    // The strongest possible caller — a WRITE-tier script tool under
    // AllowSession — still cannot reach a tool bridged from an outbound MCP
    // client through olo.call_tool. The macro-consent bargain covers local,
    // undoable editor mutations only; an external call leaves the editor's
    // undo/revert envelope, so routing one through a consented macro would
    // launder local-write consent into an outbound action (ADR 0005).
    TEST_F(McpScriptToolsTest, BridgeRefusesExternalClientToolsEvenForWriteTierUnderAllowSession)
    {
        ToolDef external;
        external.Name = "ext.files.read_file";
        external.Description = "Bridged from a fake external server.";
        external.InputSchema = Json{ { "type", "object" } };
        external.Handler = [](McpServer&, const Json&)
        { return ToolResult::Text("external ran"); };
        ASSERT_EQ(m_Server.ReplaceClientTools("files", { std::move(external) }), 1u);

        WriteScript("call_ext.lua", R"lua(
RegisterMcpTool{
    name = "script_call_ext",
    description = "Tries to reach an external bridged tool.",
    writes = true,
    handler = function(args)
        local result, err = olo.call_tool("ext.files.read_file", {})
        if err then return { refused = err } end
        return { leaked = result }
    end,
}
)lua");
        ASSERT_EQ(Load().ToolsRegistered, 1);
        m_Server.SetAllowWrites(true); // maximum authority for the caller

        const Json response = CallTool("script_call_ext");
        ASSERT_TRUE(response.contains("result")) << response.dump(2);
        ASSERT_TRUE(response["result"].contains("structuredContent")) << response.dump(2);
        const Json& payload = response["result"]["structuredContent"];
        ASSERT_TRUE(payload.contains("refused"))
            << "the bridge let a script tool reach an external client tool: " << payload.dump(2);
        EXPECT_NE(payload["refused"].get<std::string>().find("external"), std::string::npos);
    }
} // namespace
