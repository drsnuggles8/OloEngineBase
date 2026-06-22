#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Tier-1 of issue #357: tool behavioural annotations + `title` in tools/list, and
// tool-name validation at RegisterTool. The dispatch core (McpServer.cpp) is
// compiled into the test binary (its httplib-free seam); this header only
// forward-declares httplib, so no socket/winsock types leak in. We register fake
// tools carrying the same annotation shapes the real builders emit, then assert on
// the tools/list serialization — no live editor, GPU, or agent required. The real
// RegisterBuiltinTools (McpTools.cpp) pulls the whole renderer/physics stack, so it
// is intentionally NOT linked here; the per-tool classification is exercised live
// over MCP instead (see HANDOVER step 7).
#include "MCP/McpServer.h"

#include <string>
#include <utility>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
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

    ToolResult NoopHandler(McpServer&, const Json&)
    {
        return ToolResult::Text("ok");
    }

    // The two annotation shapes the production builders emit, mirrored here so the
    // serializer is tested against exactly what ships (McpTools.cpp's
    // ReadOnlyAnnotations / MutatingAnnotations).
    Json ReadOnly()
    {
        return Json{ { "readOnlyHint", true }, { "openWorldHint", false } };
    }
    Json MutatingIdempotent()
    {
        return Json{ { "readOnlyHint", false }, { "openWorldHint", false }, { "destructiveHint", false }, { "idempotentHint", true } };
    }

    // Register a tool with the given name/title/annotations and a no-op handler.
    void AddTool(McpServer& server, std::string name, std::string title, Json annotations)
    {
        ToolDef tool;
        tool.Name = std::move(name);
        tool.Title = std::move(title);
        tool.Annotations = std::move(annotations);
        tool.Description = "A fake tool.";
        tool.Handler = NoopHandler;
        server.RegisterTool(std::move(tool));
    }

    // Pull the named tool's entry out of a tools/list response. Returns nullptr if
    // absent. `response` must outlive the returned pointer.
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

// ---- tools/list: title + annotations serialization -------------------------

TEST(McpToolAnnotations, ToolsListEmitsTitleAndReadOnlyAnnotations)
{
    McpServer server(EditorMcpContext{});
    AddTool(server, "olo_fake_read", "Read something", ReadOnly());

    const Json resp = server.HandleMessage(MakeRequest(1, "tools/list"));
    const Json* entry = FindToolEntry(resp, "olo_fake_read");
    ASSERT_NE(entry, nullptr);

    // Top-level display title (spec 2025-06-18: precedence title > annotations.title > name).
    ASSERT_TRUE(entry->contains("title"));
    EXPECT_EQ((*entry)["title"], "Read something");

    ASSERT_TRUE(entry->contains("annotations"));
    const Json& ann = (*entry)["annotations"];
    EXPECT_EQ(ann.value("readOnlyHint", false), true);
    EXPECT_EQ(ann.value("openWorldHint", true), false);
}

TEST(McpToolAnnotations, ToolsListOmitsTitleWhenEmpty)
{
    McpServer server(EditorMcpContext{});
    // No title set, but annotations present.
    AddTool(server, "olo_fake_notitle", "", ReadOnly());

    const Json resp = server.HandleMessage(MakeRequest(2, "tools/list"));
    const Json* entry = FindToolEntry(resp, "olo_fake_notitle");
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->contains("title"));
    // Annotations are independent of title and must still be emitted.
    EXPECT_TRUE(entry->contains("annotations"));
}

TEST(McpToolAnnotations, ToolsListOmitsAnnotationsWhenEmpty)
{
    McpServer server(EditorMcpContext{});
    // Default-constructed Annotations is JSON null -> must be omitted entirely.
    ToolDef tool;
    tool.Name = "olo_fake_noann";
    tool.Title = "Has a title";
    tool.Description = "A fake tool.";
    tool.Handler = NoopHandler;
    server.RegisterTool(std::move(tool));

    const Json resp = server.HandleMessage(MakeRequest(3, "tools/list"));
    const Json* entry = FindToolEntry(resp, "olo_fake_noann");
    ASSERT_NE(entry, nullptr);
    EXPECT_TRUE(entry->contains("title"));
    EXPECT_FALSE(entry->contains("annotations"));
}

TEST(McpToolAnnotations, ToolsListOmitsAnnotationsWhenEmptyObject)
{
    McpServer server(EditorMcpContext{});
    // An explicitly empty object is also "no annotations" — must be omitted, not
    // serialized as "annotations": {}.
    AddTool(server, "olo_fake_emptyobj", "Empty obj", Json::object());

    const Json resp = server.HandleMessage(MakeRequest(4, "tools/list"));
    const Json* entry = FindToolEntry(resp, "olo_fake_emptyobj");
    ASSERT_NE(entry, nullptr);
    EXPECT_FALSE(entry->contains("annotations"));
}

// ---- read-only vs mutating classification round-trips ----------------------

TEST(McpToolAnnotations, ReadOnlyAndMutatingClassificationRoundTrip)
{
    McpServer server(EditorMcpContext{});
    AddTool(server, "olo_fake_read", "Read", ReadOnly());
    AddTool(server, "olo_fake_write", "Write", MutatingIdempotent());

    const Json resp = server.HandleMessage(MakeRequest(5, "tools/list"));

    const Json* ro = FindToolEntry(resp, "olo_fake_read");
    ASSERT_NE(ro, nullptr);
    EXPECT_EQ((*ro)["annotations"].value("readOnlyHint", false), true);

    const Json* rw = FindToolEntry(resp, "olo_fake_write");
    ASSERT_NE(rw, nullptr);
    const Json& ann = (*rw)["annotations"];
    EXPECT_EQ(ann.value("readOnlyHint", true), false);
    // destructiveHint / idempotentHint are meaningful only while readOnlyHint is
    // false, which holds for a mutating tool.
    EXPECT_EQ(ann.value("destructiveHint", true), false);
    EXPECT_EQ(ann.value("idempotentHint", false), true);
}

// ---- tool-name validation (pure helper) ------------------------------------

TEST(McpToolNameValidation, AcceptsWellFormedNames)
{
    EXPECT_TRUE(McpServer::IsValidToolName("olo_log_tail"));
    EXPECT_TRUE(McpServer::IsValidToolName("a"));                   // 1 char (lower bound)
    EXPECT_TRUE(McpServer::IsValidToolName("A.B-C_1"));             // every allowed punctuation class
    EXPECT_TRUE(McpServer::IsValidToolName("0123456789"));          // digits only
    EXPECT_TRUE(McpServer::IsValidToolName(std::string(128, 'x'))); // exactly 128 (upper bound)
}

TEST(McpToolNameValidation, RejectsEmptyAndOverLong)
{
    EXPECT_FALSE(McpServer::IsValidToolName(""));                    // empty
    EXPECT_FALSE(McpServer::IsValidToolName(std::string(129, 'x'))); // 129 chars (over the cap)
}

TEST(McpToolNameValidation, RejectsDisallowedCharacters)
{
    EXPECT_FALSE(McpServer::IsValidToolName("has space"));
    EXPECT_FALSE(McpServer::IsValidToolName("bad!"));
    EXPECT_FALSE(McpServer::IsValidToolName("with/slash"));
    EXPECT_FALSE(McpServer::IsValidToolName("with\\backslash"));
    EXPECT_FALSE(McpServer::IsValidToolName("tab\there"));
    EXPECT_FALSE(McpServer::IsValidToolName("colon:name"));
    EXPECT_FALSE(McpServer::IsValidToolName("paren(s)"));
    // A NUL byte embedded mid-string must be rejected, not silently truncated.
    EXPECT_FALSE(McpServer::IsValidToolName(std::string("nul\0byte", 8)));
}

// RegisterTool accepts a valid name and surfaces the tool in tools/list. (The
// reject path is an OLO_CORE_VERIFY debug-break by design — see IsValidToolName
// for the pure, testable rejection contract above.)
TEST(McpToolNameValidation, RegisterToolAcceptsValidNameAndListsIt)
{
    McpServer server(EditorMcpContext{});
    AddTool(server, "olo_valid.name-1", "Valid", ReadOnly());

    const Json resp = server.HandleMessage(MakeRequest(6, "tools/list"));
    EXPECT_NE(FindToolEntry(resp, "olo_valid.name-1"), nullptr);
}
