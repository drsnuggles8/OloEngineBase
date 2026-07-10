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
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpScriptTools.h"
#include "MCP/McpServer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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

        [[nodiscard]] McpScriptToolsReport Load(std::chrono::milliseconds budget = std::chrono::milliseconds(10000))
        {
            return LoadScriptTools(m_Server, m_Dir, budget);
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
        EXPECT_TRUE(server.Tools().empty());
    }
} // namespace
