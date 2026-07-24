// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OutputSchema adoption guard for the REAL builtin tool surface (#673 Tier 1,
// schema-adoption sweep). Unlike McpStructuredOutputTest (which pins the
// outputSchema/structuredContent MECHANISM with fake tools), this test drives
// the actual RegisterBuiltinTools registration and inspects tools/list — a
// REGISTRATION-ONLY use: no tools/call is ever issued, so no handler runs, no
// MarshalRead needs a pumped game thread, and no GL context is required.
// Registration just builds ToolDefs (names, schemas, annotations) — pure data.
//
// Two invariants:
//   1. Every declared OutputSchema is well-formed by the house rules
//      (McpSchemaBuilder.h): an open root object (never additionalProperties
//      at the root) whose `required` names a subset of `properties`.
//   2. Schema adoption coverage: the tools expected to declare an OutputSchema
//      actually do. This is the sweep's ratchet — a new tool that returns
//      stringified JSON without declaring a schema fails here instead of
//      silently regressing the adoption.
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"

#include <set>
#include <string>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
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

    // The full builtin surface, registered once and snapshotted as the
    // tools/list result. The server is local to the lambda — only the JSON
    // survives, so no handler can ever be invoked afterwards.
    const Json& BuiltinToolsList()
    {
        static const Json response = []
        {
            McpServer server{ EditorMcpContext{} };
            OloEngine::MCP::RegisterBuiltinTools(server);
            return server.HandleMessage(MakeRequest(1, "tools/list"));
        }();
        return response;
    }

    const Json& BuiltinTools()
    {
        const Json& response = BuiltinToolsList();
        EXPECT_TRUE(response.contains("result")) << response.dump(2);
        EXPECT_TRUE(response["result"].contains("tools")) << response.dump(2);
        return response["result"]["tools"];
    }
} // namespace

// Every declared outputSchema obeys the house schema rules: the root is an
// OPEN object (output schemas never close with additionalProperties:false —
// McpSchemaBuilder.h's NoAdditional contract), and every `required` entry
// names a declared property.
TEST(McpOutputSchemaCoverage, EveryDeclaredOutputSchemaIsWellFormed)
{
    const Json& tools = BuiltinTools();
    ASSERT_TRUE(tools.is_array());
    ASSERT_FALSE(tools.empty());

    for (const Json& tool : tools)
    {
        const std::string name = tool.value("name", std::string{});
        if (!tool.contains("outputSchema"))
            continue;

        const Json& schema = tool["outputSchema"];
        ASSERT_TRUE(schema.is_object()) << name;
        EXPECT_EQ(schema.value("type", std::string{}), "object") << name;
        EXPECT_FALSE(schema.contains("additionalProperties"))
            << name << ": output schemas are left open — never close the root object.";

        if (schema.contains("required"))
        {
            ASSERT_TRUE(schema["required"].is_array()) << name;
            ASSERT_TRUE(schema.contains("properties"))
                << name << ": `required` without any declared `properties`.";
            for (const Json& requiredName : schema["required"])
            {
                ASSERT_TRUE(requiredName.is_string()) << name;
                EXPECT_TRUE(schema["properties"].contains(requiredName.get<std::string>()))
                    << name << ": required field '" << requiredName.get<std::string>()
                    << "' is not a declared property.";
            }
        }
    }
}

// Full-surface adoption (the #673 Tier 1 sweep): EVERY registered tool
// declares an OutputSchema, except the deliberate text-only exemptions below.
// This is the sweep's ratchet in both directions — a new tool returning
// stringified JSON without a schema fails here, and an exemption that quietly
// grows a schema (or a schema that disappears) fails here too.
TEST(McpOutputSchemaCoverage, EveryToolDeclaresAnOutputSchemaExceptTextOnlyExemptions)
{
    // Genuinely free-form text a JSON outputSchema cannot constrain (the
    // McpToolsRender "markdown returns free text" rule). Keep this list tiny
    // and justified per entry.
    static const std::set<std::string> kTextOnlyExempt = {
        "olo_log_tail", // raw spdlog lines, deliberately unreshaped
    };

    const Json& tools = BuiltinTools();
    ASSERT_TRUE(tools.is_array());
    // 65 = the 66-tool surface minus olo_script_tools_reload, which is
    // registered separately by the editor layer (it needs the project's script
    // directory + budgets), not by RegisterBuiltinTools.
    EXPECT_GE(tools.size(), 65u) << "the builtin surface shrank unexpectedly";

    std::set<std::string> seenExempt;
    for (const Json& tool : tools)
    {
        const std::string name = tool.value("name", std::string{});
        if (kTextOnlyExempt.contains(name))
        {
            seenExempt.insert(name);
            EXPECT_FALSE(tool.contains("outputSchema"))
                << name << " is listed text-only exempt but now declares a schema — update the exempt list.";
        }
        else
        {
            EXPECT_TRUE(tool.contains("outputSchema"))
                << name << " declares no outputSchema — migrate it (ToolResult::Structured + "
                << "Schema::Object() at registration) or add it to kTextOnlyExempt with a reason.";
        }
    }
    for (const std::string& name : kTextOnlyExempt)
        EXPECT_TRUE(seenExempt.contains(name)) << name << " is no longer registered at all.";
}
