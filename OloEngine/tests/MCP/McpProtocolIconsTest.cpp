// OLO_TEST_LAYER: unit
//
// MCP protocol surface added by issue #607, at the httplib-free dispatch seam
// (OloEditor/src/MCP/McpServer.cpp — HandleMessage / ProcessRequestBody against
// an McpServer with no editor, exactly like McpDispatchTest):
//
//   * tool `icons` + `serverInfo.icons` (SEP-973, spec 2025-11-25): serialized
//     when present, and NEVER emitted as an empty key when absent;
//   * `IsValidIcons` — the shared shape check both the native RegisterTool
//     assertion and the Lua RegisterMcpTool{ icons = ... } rejection run on;
//   * `capabilities.tools.listChanged` is now TRUE, because the tool registry
//     can be republished live (script-tool reload);
//   * the copy-on-write tool snapshot itself: a swap performed WHILE a snapshot
//     is held leaves the held snapshot (and the ToolDefs in it) valid — the
//     property the lock-free dispatch read path depends on.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpServer.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;

    Json MakeIcons(const std::string& src = "https://example.invalid/icon.png")
    {
        return Json::array({ Json{ { "src", src },
                                   { "mimeType", "image/png" },
                                   { "sizes", Json::array({ "48x48", "96x96" }) } } });
    }

    ToolDef MakeTool(const std::string& name, Json icons = Json())
    {
        ToolDef tool;
        tool.Name = name;
        tool.Description = "test tool";
        tool.Icons = std::move(icons);
        tool.Handler = [](McpServer&, const Json&)
        { return ToolResult::Text("ok"); };
        return tool;
    }

    class McpProtocolIconsTest : public ::testing::Test
    {
      protected:
        McpProtocolIconsTest()
            : m_Server(EditorMcpContext{})
        {
        }

        [[nodiscard]] Json Send(const Json& request)
        {
            return m_Server.HandleMessage(request);
        }

        [[nodiscard]] Json ToolsList()
        {
            return Send({ { "jsonrpc", "2.0" }, { "id", 1 }, { "method", "tools/list" } });
        }

        [[nodiscard]] static const Json* FindEntry(const Json& list, const std::string& name)
        {
            const Json& tools = list["result"]["tools"];
            const auto it = std::find_if(tools.begin(), tools.end(),
                                         [&name](const Json& t)
                                         { return t["name"] == name; });
            return it == tools.end() ? nullptr : &(*it);
        }

        McpServer m_Server;
    };

    // ---- IsValidIcons (pure) ---------------------------------------------------

    TEST_F(McpProtocolIconsTest, IsValidIconsAcceptsTheSep973Shape)
    {
        EXPECT_TRUE(McpServer::IsValidIcons(Json())) << "absent icons are legal";
        EXPECT_TRUE(McpServer::IsValidIcons(MakeIcons()));
        // mimeType / sizes are optional.
        EXPECT_TRUE(McpServer::IsValidIcons(Json::array({ Json{ { "src", "data:image/png;base64,AA==" } } })));
    }

    TEST_F(McpProtocolIconsTest, IsValidIconsRejectsMalformedValues)
    {
        EXPECT_FALSE(McpServer::IsValidIcons(Json::array())) << "an empty array must never be emitted";
        EXPECT_FALSE(McpServer::IsValidIcons(Json{ { "src", "x" } })) << "an object is not an array";
        EXPECT_FALSE(McpServer::IsValidIcons(Json::array({ Json{ { "mimeType", "image/png" } } })))
            << "'src' is required";
        EXPECT_FALSE(McpServer::IsValidIcons(Json::array({ Json{ { "src", "" } } }))) << "'src' must be non-empty";
        EXPECT_FALSE(McpServer::IsValidIcons(Json::array({ Json{ { "src", 7 } } })));
        EXPECT_FALSE(McpServer::IsValidIcons(
            Json::array({ Json{ { "src", "x" }, { "mimeType", 1 } } })));
        EXPECT_FALSE(McpServer::IsValidIcons(
            Json::array({ Json{ { "src", "x" }, { "sizes", "48x48" } } })))
            << "'sizes' is an array of strings, not a string";
        EXPECT_FALSE(McpServer::IsValidIcons(
            Json::array({ Json{ { "src", "x" }, { "sizes", Json::array({ 48 }) } } })));
    }

    // ---- tools/list --------------------------------------------------------------

    TEST_F(McpProtocolIconsTest, ToolIconsRoundTripAndAreOmittedWhenAbsent)
    {
        m_Server.RegisterTool(MakeTool("olo_with_icons", MakeIcons()));
        m_Server.RegisterTool(MakeTool("olo_without_icons"));

        const Json list = ToolsList();

        const Json* withIcons = FindEntry(list, "olo_with_icons");
        ASSERT_NE(withIcons, nullptr) << list.dump(2);
        ASSERT_TRUE(withIcons->contains("icons"));
        EXPECT_EQ((*withIcons)["icons"], MakeIcons());

        const Json* withoutIcons = FindEntry(list, "olo_without_icons");
        ASSERT_NE(withoutIcons, nullptr);
        EXPECT_FALSE(withoutIcons->contains("icons"))
            << "an empty/absent icons value must not appear as an empty key";
    }

    TEST_F(McpProtocolIconsTest, ToolsSearchCarriesIconsToo)
    {
        m_Server.RegisterTool(MakeTool("olo_with_icons", MakeIcons()));

        const Json response = Send({ { "jsonrpc", "2.0" },
                                     { "id", 2 },
                                     { "method", "tools/search" },
                                     { "params", { { "query", "olo_with_icons" } } } });
        ASSERT_TRUE(response.contains("result")) << response.dump(2);
        const Json& tools = response["result"]["tools"];
        ASSERT_EQ(tools.size(), 1u) << response.dump(2);
        EXPECT_EQ(tools[0]["icons"], MakeIcons());
    }

    // ---- initialize ---------------------------------------------------------------

    TEST_F(McpProtocolIconsTest, ServerInfoIconsAreOmittedByDefaultAndEmittedWhenSet)
    {
        const Json request{ { "jsonrpc", "2.0" }, { "id", 3 }, { "method", "initialize" }, { "params", Json::object() } };

        const Json before = Send(request);
        ASSERT_TRUE(before.contains("result")) << before.dump(2);
        EXPECT_FALSE(before["result"]["serverInfo"].contains("icons"));

        m_Server.SetServerIcons(MakeIcons("https://example.invalid/olo.png"));

        const Json after = Send(request);
        ASSERT_TRUE(after.contains("result")) << after.dump(2);
        ASSERT_TRUE(after["result"]["serverInfo"].contains("icons"));
        EXPECT_EQ(after["result"]["serverInfo"]["icons"][0]["src"], "https://example.invalid/olo.png");
    }

    // The tool registry is republishable at runtime (script-tool live reload), so
    // the capability must say so — a client that trusts listChanged:false would
    // never re-list after a reload.
    TEST_F(McpProtocolIconsTest, InitializeAdvertisesToolsListChanged)
    {
        const Json response = Send({ { "jsonrpc", "2.0" },
                                     { "id", 4 },
                                     { "method", "initialize" },
                                     { "params", Json::object() } });
        ASSERT_TRUE(response.contains("result")) << response.dump(2);
        EXPECT_EQ(response["result"]["capabilities"]["tools"]["listChanged"], true);
        // Resources / prompts are still fixed for a server run.
        EXPECT_EQ(response["result"]["capabilities"]["resources"]["listChanged"], false);
        EXPECT_EQ(response["result"]["capabilities"]["prompts"]["listChanged"], false);
    }

    // ---- the copy-on-write snapshot ------------------------------------------------

    // The property the lock-free dispatch read path rests on: a swap does not
    // invalidate a snapshot someone already holds. HandleToolsCall pins one for the
    // whole call precisely so a concurrent script-tool reload cannot pull the
    // ToolDef (or the Lua state its handler owns) out from under a running handler.
    TEST_F(McpProtocolIconsTest, SwappingTheRegistryLeavesAHeldSnapshotValid)
    {
        ToolDef script = MakeTool("script_old");
        script.ScriptOwned = true;
        m_Server.RegisterTool(MakeTool("olo_native"));
        m_Server.RegisterTool(std::move(script));

        const McpServer::ToolSnapshot held = m_Server.ToolsSnapshot();
        ASSERT_EQ(held->size(), 2u);
        const u64 generationBefore = m_Server.ToolsGeneration();

        std::vector<ToolDef> replacement;
        ToolDef fresh = MakeTool("script_new");
        fresh.ScriptOwned = true;
        replacement.push_back(std::move(fresh));
        m_Server.ReplaceScriptTools(std::move(replacement));

        // The held snapshot still describes the OLD world, intact and callable.
        ASSERT_EQ(held->size(), 2u);
        EXPECT_EQ((*held)[1].Name, "script_old");
        EXPECT_FALSE((*held)[1].Handler(m_Server, Json::object()).IsError);

        // The published one describes the new: natives kept in order, script tools swapped.
        const McpServer::ToolSnapshot current = m_Server.ToolsSnapshot();
        ASSERT_EQ(current->size(), 2u);
        EXPECT_EQ((*current)[0].Name, "olo_native");
        EXPECT_EQ((*current)[1].Name, "script_new");
        EXPECT_GT(m_Server.ToolsGeneration(), generationBefore);
    }
} // namespace
