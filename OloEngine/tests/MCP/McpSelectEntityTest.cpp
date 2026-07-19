#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// McpSelectEntityTest — unit test (headless, no GL, no live editor, no scene).
//
// Pins the consented MCP write tool olo_editor_select_entity (issue #607): select
// (or clear) the Scene Hierarchy panel's selection so the editor's Properties
// inspector draws the requested entity's components — the write that unblocks
// screenshot verification of the whole DrawComponent<T> surface (olo_input_inject
// can't reliably land a Scene Hierarchy PANEL row click; the OS cursor reasserts
// over the synthetic position between injected frames). Same two seams as
// McpSceneControlTest.cpp:
//
//   1. The dispatch seam (McpServer.cpp, compiled into the test binary): a tool
//      flagged ToolDef::ProjectWrite is REFUSED with a clean JSON-RPC error while
//      the session "Allow writes" gate is off, and ACCEPTED once on — and the
//      selection ACTION does NOT run while the gate is off. The server's
//      inputSchema enforcement (#423) rejects a malformed call before the handler
//      runs. McpTools.cpp is deliberately NOT linked here, so the test registers a
//      fake tool wired to the SAME schema + result shaping the real handler uses.
//
//   2. The shared shaping + validation (MCP/McpSelectEntity.h, header-only): the
//      inputSchema, the pure ParseArgs (entity XOR clear) + ParseUuidValue
//      helpers, and the McpSelectEntityResult -> JSON ToJson the real handler
//      emits. The selection ACTION itself is a context hook (the EnTT registry /
//      an editor-only ImGui panel), so the tests inject fakes and never touch a
//      scene or a window — the same indirection the real handler delegates to on
//      the main thread (EditorLayer::SelectEntityInEditor).
//
// The shared header is renderer/httplib/editor-panel-free, so only McpServer.h +
// the schema DSL are pulled in — no extra editor TU.
// =============================================================================

#include "MCP/McpSelectEntity.h"
#include "MCP/McpServer.h"
#include "MCP/McpToolsCommon.h"

#include <algorithm>
#include <optional>
#include <string>

// OLO_TEST_LAYER: unit

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpSelectEntityResult;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;
    namespace SelectEntity = OloEngine::MCP::SelectEntity;

    constexpr int kInvalidParams = -32602;

    Json MakeCallRequest(const Json& id, const Json& arguments)
    {
        return Json{ { "jsonrpc", "2.0" },
                     { "id", id },
                     { "method", "tools/call" },
                     { "params", { { "name", "olo_editor_select_entity" }, { "arguments", arguments } } } };
    }

    // Fixture: an McpServer whose only tool is a fake olo_editor_select_entity,
    // wired through the SAME schema + ParseArgs + ToJson the real handler uses,
    // but with the selection ACTION replaced by a test-owned closure that records
    // its invocations and returns a canned result — so the dispatch gate, schema
    // enforcement, and result shaping are exercised without a live scene / editor
    // window (the real handler marshals onto the main thread; here it runs
    // synchronously). The handler mirrors the real one: parse via
    // SelectEntity::ParseArgs, then invoke the "hook".
    class McpSelectEntityTest : public ::testing::Test
    {
      protected:
        McpSelectEntityTest()
            : m_Server(EditorMcpContext{})
        {
            ToolDef tool;
            tool.Name = "olo_editor_select_entity";
            tool.Description = "Select/clear entity selection (fake; test wiring).";
            tool.ProjectWrite = true;
            tool.InputSchema = SelectEntity::InputSchema();
            tool.Handler = [this](McpServer&, const Json& args) -> ToolResult
            {
                SelectEntity::Request request;
                if (const auto error = SelectEntity::ParseArgs(args, request))
                    return ToolResult::Error(*error);
                ++m_CallCount;
                m_LastRequest = request;
                return ToolResult::Text(SelectEntity::ToJson(m_FakeResult).dump());
            };
            m_Server.RegisterTool(std::move(tool));
        }

        McpSelectEntityResult m_FakeResult;
        int m_CallCount = 0;
        std::optional<SelectEntity::Request> m_LastRequest;
        McpServer m_Server; // declared last → destroyed first (its handler refs members)
    };
} // namespace

// ---- the session write gate (dispatch seam) --------------------------------

// Default state: the gate is OFF, so even a well-formed selection call is
// refused with a JSON-RPC error and the selection action NEVER runs.
TEST_F(McpSelectEntityTest, GateOffRejectsAndDoesNotInvoke)
{
    ASSERT_FALSE(m_Server.AllowWrites()); // off by default

    const Json resp = m_Server.HandleMessage(MakeCallRequest(1, Json{ { "entity", "42" } }));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], kInvalidParams);
    EXPECT_EQ(m_CallCount, 0);
}

// With the gate ON, a valid uuid selects: the action runs once and the result
// carries the resolved entity through.
TEST_F(McpSelectEntityTest, GateOnValidUuidSelects)
{
    m_Server.SetAllowWrites(true);
    m_FakeResult.Available = true;
    m_FakeResult.Ok = true;
    m_FakeResult.Changed = true;
    m_FakeResult.Selected = true;
    m_FakeResult.EntityId = 12652600558176869447ULL;
    m_FakeResult.EntityName = "Cube";
    m_FakeResult.Message = "Selected 'Cube'.";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(2, Json{ { "entity", "12652600558176869447" } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(m_CallCount, 1);
    ASSERT_TRUE(m_LastRequest.has_value());
    EXPECT_FALSE(m_LastRequest->Clear);
    EXPECT_EQ(m_LastRequest->EntityUuid, 12652600558176869447ULL);

    const Json payload = Json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_TRUE(payload["changed"].get<bool>());
    EXPECT_TRUE(payload["selected"].get<bool>());
    EXPECT_EQ(payload["entity"], "12652600558176869447");
    EXPECT_EQ(payload["name"], "Cube");
}

// A JSON-Schema type check happens BEFORE the handler runs (McpServer's
// inputSchema enforcement) — this pins that a NUMERIC 'entity' is not rejected
// at that layer, since ParseArgs/ParseUuid below explicitly support one. Uses
// the real m_Server.HandleMessage dispatch path (not a direct ParseArgs call),
// so it exercises the actual schema declared by InputSchema().
TEST_F(McpSelectEntityTest, SchemaAcceptsNumericEntity)
{
    m_Server.SetAllowWrites(true);
    m_FakeResult.Available = true;
    m_FakeResult.Ok = true;
    m_FakeResult.Changed = true;
    m_FakeResult.Selected = true;
    m_FakeResult.EntityId = 42;
    m_FakeResult.EntityName = "Player";
    m_FakeResult.Message = "Selected 'Player'.";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(8, Json{ { "entity", 42 } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(m_CallCount, 1);
    ASSERT_TRUE(m_LastRequest.has_value());
    EXPECT_EQ(m_LastRequest->EntityUuid, 42ULL);
}

// An unknown uuid is a clean result (protocol-level success, ok:false) — not a
// crash and not a tool error — and does NOT report a selection.
TEST_F(McpSelectEntityTest, GateOnUnknownUuidReportsOkFalseNotSelected)
{
    m_Server.SetAllowWrites(true);
    m_FakeResult.Available = true;
    m_FakeResult.Ok = false;
    m_FakeResult.Changed = false;
    m_FakeResult.Selected = false;
    m_FakeResult.Message = "No entity with UUID 999 in the active scene.";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(3, Json{ { "entity", "999" } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]); // a clean result, not a protocol error
    EXPECT_EQ(m_CallCount, 1);

    const Json payload = Json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_FALSE(payload["ok"].get<bool>());
    EXPECT_FALSE(payload["changed"].get<bool>());
    EXPECT_FALSE(payload["selected"].get<bool>());
    EXPECT_FALSE(payload.contains("entity")); // no misleading zero/stale uuid
}

// clear:true deselects: the action runs with Clear=true and reports
// selected:false.
TEST_F(McpSelectEntityTest, GateOnClearDeselects)
{
    m_Server.SetAllowWrites(true);
    m_FakeResult.Available = true;
    m_FakeResult.Ok = true;
    m_FakeResult.Changed = true;
    m_FakeResult.Selected = false;
    m_FakeResult.Message = "Cleared the Scene Hierarchy selection.";

    const Json resp = m_Server.HandleMessage(MakeCallRequest(4, Json{ { "clear", true } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(m_CallCount, 1);
    ASSERT_TRUE(m_LastRequest.has_value());
    EXPECT_TRUE(m_LastRequest->Clear);

    const Json payload = Json::parse(resp["result"]["content"][0]["text"].get<std::string>());
    EXPECT_TRUE(payload["ok"].get<bool>());
    EXPECT_TRUE(payload["changed"].get<bool>());
    EXPECT_FALSE(payload["selected"].get<bool>());
    EXPECT_FALSE(payload.contains("entity"));
}

// ---- server-side inputSchema enforcement (#423), gate ON -------------------

// An unexpected property is rejected before the handler runs.
TEST_F(McpSelectEntityTest, SchemaRejectsUnknownProperty)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(5, Json{ { "entity", "1" }, { "speed", 2 } }));

    ASSERT_TRUE(resp.contains("result")); // SEP-1303: schema failures are tool errors
    EXPECT_EQ(resp["result"]["isError"], true);
    EXPECT_EQ(m_CallCount, 0);
}

// Giving neither 'entity' nor 'clear' passes the (permissive) JSON-Schema but is
// caught by the handler's ParseArgs, so the action never runs.
TEST_F(McpSelectEntityTest, HandlerRejectsEmptyArgs)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(6, Json::object()));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"]["isError"]);
    EXPECT_EQ(m_CallCount, 0);
}

// Giving BOTH 'entity' and 'clear':true is ambiguous and rejected.
TEST_F(McpSelectEntityTest, HandlerRejectsBothEntityAndClear)
{
    m_Server.SetAllowWrites(true);
    const Json resp = m_Server.HandleMessage(MakeCallRequest(7, Json{ { "entity", "1" }, { "clear", true } }));

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"]["isError"]);
    EXPECT_EQ(m_CallCount, 0);
}

// ---- the shared shaping + validation core (MCP/McpSelectEntity.h) ----------

TEST(McpSelectEntityShaping, ToJsonOmitsEntityWhenNotSelected)
{
    McpSelectEntityResult r;
    r.Available = true;
    r.Ok = true;
    r.Changed = true;
    r.Selected = false;
    r.Message = "Cleared the Scene Hierarchy selection.";

    const Json j = SelectEntity::ToJson(r);
    EXPECT_TRUE(j["available"].get<bool>());
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_TRUE(j["changed"].get<bool>());
    EXPECT_FALSE(j["selected"].get<bool>());
    EXPECT_FALSE(j.contains("entity"));
    EXPECT_FALSE(j.contains("name"));
}

TEST(McpSelectEntityShaping, ToJsonCarriesEntityWhenSelected)
{
    McpSelectEntityResult r;
    r.Available = true;
    r.Ok = true;
    r.Changed = true;
    r.Selected = true;
    r.EntityId = 42;
    r.EntityName = "Player";
    r.Message = "Selected 'Player'.";

    const Json j = SelectEntity::ToJson(r);
    EXPECT_TRUE(j["selected"].get<bool>());
    EXPECT_TRUE(j["changed"].get<bool>());
    EXPECT_EQ(j["entity"], "42");
    EXPECT_EQ(j["name"], "Player");
}

TEST(McpSelectEntityShaping, ToJsonReportsChangedFalseWhenAlreadyInThatState)
{
    // Re-selecting the already-selected entity (or clearing an already-empty
    // selection) is Ok:true but Changed:false — the idempotent no-op case.
    McpSelectEntityResult r;
    r.Available = true;
    r.Ok = true;
    r.Changed = false;
    r.Selected = true;
    r.EntityId = 42;
    r.EntityName = "Player";
    r.Message = "Selected 'Player'.";

    const Json j = SelectEntity::ToJson(r);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_FALSE(j["changed"].get<bool>());
}

TEST(McpSelectEntityShaping, InputSchemaHasNoRequiredFieldsAndIsClosed)
{
    const Json schema = SelectEntity::InputSchema();
    EXPECT_EQ(schema["type"], "object");
    ASSERT_TRUE(schema.contains("additionalProperties"));
    EXPECT_FALSE(schema["additionalProperties"].get<bool>());
    // 'entity' XOR 'clear' is a value-level constraint (ParseArgs), not a
    // schema-level 'required' — either may be omitted depending on which form
    // the caller uses.
    EXPECT_FALSE(schema.contains("required"));
}

// 'entity' must declare BOTH "string" and "number" as acceptable JSON types —
// ParseUuid (used by ParseArgs) accepts either, so the schema must not reject
// a numeric value before the parser ever sees it (a real gap: Schema::EntityId()
// alone only emits "string", see SchemaAcceptsNumericEntity above for the
// end-to-end dispatch-level pin).
TEST(McpSelectEntityShaping, InputSchemaEntityAcceptsStringOrNumberType)
{
    const Json schema = SelectEntity::InputSchema();
    ASSERT_TRUE(schema.contains("properties"));
    ASSERT_TRUE(schema["properties"].contains("entity"));
    const Json& entityType = schema["properties"]["entity"]["type"];
    ASSERT_TRUE(entityType.is_array());
    EXPECT_NE(std::find(entityType.begin(), entityType.end(), "string"), entityType.end());
    EXPECT_NE(std::find(entityType.begin(), entityType.end(), "number"), entityType.end());
}

// ParseArgs: entity (string or number) selects, clear:true deselects, and every
// invalid shape is rejected with a message (not a crash).
TEST(McpSelectEntityValidation, ParseArgsAcceptsEntityString)
{
    SelectEntity::Request request;
    const auto error = SelectEntity::ParseArgs(Json{ { "entity", "123" } }, request);
    EXPECT_FALSE(error.has_value());
    EXPECT_FALSE(request.Clear);
    EXPECT_EQ(request.EntityUuid, 123ULL);
}

TEST(McpSelectEntityValidation, ParseArgsAcceptsEntityNumber)
{
    SelectEntity::Request request;
    const auto error = SelectEntity::ParseArgs(Json{ { "entity", 456 } }, request);
    EXPECT_FALSE(error.has_value());
    EXPECT_FALSE(request.Clear);
    EXPECT_EQ(request.EntityUuid, 456ULL);
}

TEST(McpSelectEntityValidation, ParseArgsAcceptsClearTrue)
{
    SelectEntity::Request request;
    const auto error = SelectEntity::ParseArgs(Json{ { "clear", true } }, request);
    EXPECT_FALSE(error.has_value());
    EXPECT_TRUE(request.Clear);
    EXPECT_EQ(request.EntityUuid, 0ULL);
}

TEST(McpSelectEntityValidation, ParseArgsRejectsEmptyArgs)
{
    SelectEntity::Request request;
    EXPECT_TRUE(SelectEntity::ParseArgs(Json::object(), request).has_value());
}

TEST(McpSelectEntityValidation, ParseArgsRejectsBothEntityAndClear)
{
    SelectEntity::Request request;
    EXPECT_TRUE(SelectEntity::ParseArgs(Json{ { "entity", "1" }, { "clear", true } }, request).has_value());
}

TEST(McpSelectEntityValidation, ParseArgsRejectsClearFalseWithNoEntity)
{
    // clear:false is NOT treated as "clear requested" — it must still supply
    // 'entity', mirroring a caller who explicitly opted out of clearing.
    SelectEntity::Request request;
    EXPECT_TRUE(SelectEntity::ParseArgs(Json{ { "clear", false } }, request).has_value());
}

TEST(McpSelectEntityValidation, ParseArgsRejectsNonBooleanClear)
{
    SelectEntity::Request request;
    EXPECT_TRUE(SelectEntity::ParseArgs(Json{ { "clear", "yes" } }, request).has_value());
}

TEST(McpSelectEntityValidation, ParseArgsRejectsMalformedEntity)
{
    SelectEntity::Request request;
    EXPECT_TRUE(SelectEntity::ParseArgs(Json{ { "entity", "123junk" } }, request).has_value());
    EXPECT_TRUE(SelectEntity::ParseArgs(Json{ { "entity", "-5" } }, request).has_value());
    EXPECT_TRUE(SelectEntity::ParseArgs(Json{ { "entity", "" } }, request).has_value());
    EXPECT_TRUE(SelectEntity::ParseArgs(Json{ { "entity", true } }, request).has_value());
}

// ParseArgs delegates 'entity' parsing to the shared OloEngine::MCP::ParseUuid
// (McpToolsCommon.h) rather than a local re-implementation — pin its rejection
// of a negative JSON number directly, since (until this test) no test in the
// suite exercised ParseUuid on its own.
TEST(McpSelectEntityValidation, ParseUuidRejectsNegativeNumber)
{
    u64 out = 0;
    EXPECT_FALSE(OloEngine::MCP::ParseUuid(Json(-5), out));
}
