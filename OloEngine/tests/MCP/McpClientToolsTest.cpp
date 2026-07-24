// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// The registry-merge contract for tools bridged from an OUTBOUND MCP client
// connection (#673 Tier 1, bullet 1) — the trust seam, tested independently of
// any child process (the stdio transport has its own tests): foreign tool
// definitions are runtime network data, so ReplaceClientTools must
// validate-and-reject (never assert), confine every accepted tool to the
// reserved `ext.<alias>.` namespace, FORCE the authority posture
// (ProjectWrite=true, readOnlyHint:false, openWorldHint:true) regardless of
// what the child claimed, coexist with native and ScriptOwned tools across
// swaps, and mark its consent prompts External so the panel modal never
// promises Ctrl-Z for an out-of-process effect.
#include "MCP/McpServer.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using OloEngine::MCP::ConsentDecision;
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using OloEngine::MCP::WriteConsentMode;
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

    ToolDef MakeBridgedTool(const std::string& name)
    {
        ToolDef tool;
        tool.Name = name;
        tool.Description = "Bridged from a fake external server.";
        tool.InputSchema = Json{ { "type", "object" } };
        tool.Handler = [](McpServer&, const Json&)
        { return ToolResult::Text("external ran"); };
        return tool;
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

    Json ToolsList(McpServer& server)
    {
        return server.HandleMessage(MakeRequest(1, "tools/list"));
    }
} // namespace

// ---- alias + name validation (reject, never assert) ---------------------------

TEST(McpClientTools, InvalidAliasRejectsTheWholeBatch)
{
    McpServer server{ EditorMcpContext{} };
    EXPECT_EQ(server.ReplaceClientTools("", { MakeBridgedTool("ext..x") }), 0u);
    EXPECT_EQ(server.ReplaceClientTools("Has-Upper", { MakeBridgedTool("ext.Has-Upper.x") }), 0u);
    EXPECT_EQ(server.ReplaceClientTools("-leading", { MakeBridgedTool("ext.-leading.x") }), 0u);
    EXPECT_EQ(server.ReplaceClientTools("spaced alias", {}), 0u);
    EXPECT_EQ(server.ToolCount(), 0u);
}

TEST(McpClientTools, NamesOutsideTheReservedNamespaceAreDropped)
{
    McpServer server{ EditorMcpContext{} };
    std::vector<ToolDef> batch;
    batch.push_back(MakeBridgedTool("read_file"));           // no prefix
    batch.push_back(MakeBridgedTool("olo_scene_summary"));   // native spoof attempt
    batch.push_back(MakeBridgedTool("script_sneaky"));       // script spoof attempt
    batch.push_back(MakeBridgedTool("ext.other.read_file")); // wrong alias
    batch.push_back(MakeBridgedTool("ext.files."));          // empty suffix
    batch.push_back(MakeBridgedTool("ext.files.read file")); // invalid charset
    batch.push_back(MakeBridgedTool("ext.files.read_file")); // the one valid tool
    batch.push_back(MakeBridgedTool("ext.files.read_file")); // duplicate of it

    EXPECT_EQ(server.ReplaceClientTools("files", std::move(batch)), 1u);
    const Json list = ToolsList(server);
    EXPECT_NE(FindToolEntry(list, "ext.files.read_file"), nullptr);
    EXPECT_EQ(list["result"]["tools"].size(), 1u);
}

TEST(McpClientTools, ToolWithoutAHandlerIsDropped)
{
    McpServer server{ EditorMcpContext{} };
    ToolDef noHandler = MakeBridgedTool("ext.files.broken");
    noHandler.Handler = nullptr;
    EXPECT_EQ(server.ReplaceClientTools("files", { std::move(noHandler) }), 0u);
}

// ---- forced authority posture --------------------------------------------------

TEST(McpClientTools, AuthorityPostureIsForcedRegardlessOfChildClaims)
{
    McpServer server{ EditorMcpContext{} };
    ToolDef liar = MakeBridgedTool("ext.files.read_file");
    // The child claims it is a harmless read-only local tool. Never trusted.
    liar.ProjectWrite = false;
    liar.ScriptOwned = true;
    liar.Toolset = "render";
    liar.Annotations = Json{ { "readOnlyHint", true }, { "openWorldHint", false } };
    ASSERT_EQ(server.ReplaceClientTools("files", { std::move(liar) }), 1u);

    // Bind the response before taking a pointer into it — FindToolEntry on a
    // temporary would dangle.
    const Json list = ToolsList(server);
    const Json* entry = FindToolEntry(list, "ext.files.read_file");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ((*entry)["annotations"]["readOnlyHint"], false);
    EXPECT_EQ((*entry)["annotations"]["openWorldHint"], true);
    EXPECT_FALSE((*entry)["annotations"].contains("destructiveHint"))
        << "keep the spec's conservative default (destructive) for foreign tools";
    EXPECT_EQ((*entry)["_meta"]["io.oloengine/toolset"], "ext.files");

    // ProjectWrite was forced true: with consent Disabled (the default), the
    // call is refused by the write gate before the bridge handler ever runs.
    const Json refused = server.HandleMessage(
        MakeRequest(2, "tools/call", Json{ { "name", "ext.files.read_file" } }));
    ASSERT_TRUE(refused.contains("error")) << refused.dump(2);
    EXPECT_EQ(refused["error"]["code"], -32602);

    // Under AllowSession the bridge handler runs.
    server.SetAllowWrites(true);
    const Json allowed = server.HandleMessage(
        MakeRequest(3, "tools/call", Json{ { "name", "ext.files.read_file" } }));
    ASSERT_TRUE(allowed.contains("result")) << allowed.dump(2);
    EXPECT_EQ(allowed["result"]["content"][0]["text"], "external ran");
}

// ---- per-alias replace semantics ----------------------------------------------

TEST(McpClientTools, ReplaceIsScopedToItsAliasAndCoexistsWithScriptTools)
{
    McpServer server{ EditorMcpContext{} };

    ToolDef native;
    native.Name = "fake_native";
    native.Description = "A native tool.";
    native.Handler = [](McpServer&, const Json&)
    { return ToolResult::Text("native"); };
    server.RegisterTool(std::move(native));

    ToolDef scripted;
    scripted.Name = "script_fake";
    scripted.Description = "A script tool.";
    scripted.ScriptOwned = true;
    scripted.Handler = [](McpServer&, const Json&)
    { return ToolResult::Text("script"); };
    server.RegisterTool(std::move(scripted));

    ASSERT_EQ(server.ReplaceClientTools("files", { MakeBridgedTool("ext.files.read_file") }), 1u);
    ASSERT_EQ(server.ReplaceClientTools("web", { MakeBridgedTool("ext.web.fetch") }), 1u);
    EXPECT_EQ(server.ToolCount(), 4u);

    // Re-merging one alias replaces only that alias's tools.
    ASSERT_EQ(server.ReplaceClientTools("files", { MakeBridgedTool("ext.files.write_file") }), 1u);
    Json list = ToolsList(server);
    EXPECT_EQ(FindToolEntry(list, "ext.files.read_file"), nullptr);
    EXPECT_NE(FindToolEntry(list, "ext.files.write_file"), nullptr);
    EXPECT_NE(FindToolEntry(list, "ext.web.fetch"), nullptr);

    // A script rescan (wholesale ScriptOwned replace) must not touch client tools.
    server.ReplaceScriptTools({});
    list = ToolsList(server);
    EXPECT_EQ(FindToolEntry(list, "script_fake"), nullptr);
    EXPECT_NE(FindToolEntry(list, "ext.files.write_file"), nullptr);
    EXPECT_NE(FindToolEntry(list, "ext.web.fetch"), nullptr);
    EXPECT_NE(FindToolEntry(list, "fake_native"), nullptr);

    // Disconnect: an empty batch removes the alias's tools and notifies.
    const u64 generationBefore = server.ToolsGeneration();
    EXPECT_EQ(server.ReplaceClientTools("web", {}), 0u);
    EXPECT_GT(server.ToolsGeneration(), generationBefore);
    EXPECT_EQ(FindToolEntry(ToolsList(server), "ext.web.fetch"), nullptr);
}

TEST(McpClientTools, MergeNotifiesToolsListChanged)
{
    McpServer server{ EditorMcpContext{} };
    std::vector<std::string> notified;
    server.AddNotificationListener([&notified](const Json& notification)
                                   { notified.push_back(notification.value("method", std::string{})); });
    ASSERT_EQ(server.ReplaceClientTools("files", { MakeBridgedTool("ext.files.read_file") }), 1u);
    ASSERT_EQ(notified.size(), 1u);
    EXPECT_EQ(notified[0], "notifications/tools/list_changed");
}

// ---- consent prompt honesty ----------------------------------------------------

TEST(McpClientTools, PromptModeMarksExternalConsentEntries)
{
    McpServer server{ EditorMcpContext{} };
    ASSERT_EQ(server.ReplaceClientTools("files", { MakeBridgedTool("ext.files.read_file") }), 1u);
    server.SetWriteConsentMode(WriteConsentMode::Prompt);
    server.SetConsentTimeout(std::chrono::milliseconds(5000));

    Json response;
    std::thread caller([&server, &response]
                       { response = server.HandleMessage(
                             MakeRequest(4, "tools/call", Json{ { "name", "ext.files.read_file" } })); });

    // Wait for the prompt to surface (the caller thread is parked on the modal).
    u64 promptId = 0;
    bool external = false;
    for (int i = 0; i < 200 && promptId == 0; ++i)
    {
        const auto pending = server.PendingConsents();
        if (!pending.empty())
        {
            promptId = pending.front().Id;
            external = pending.front().External;
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ASSERT_NE(promptId, 0u) << "consent prompt never surfaced";
    EXPECT_TRUE(external) << "a bridged tool's consent prompt must be marked External";

    server.ResolveConsent(promptId, ConsentDecision::Deny);
    caller.join();
    ASSERT_TRUE(response.contains("error")) << response.dump(2);
}
