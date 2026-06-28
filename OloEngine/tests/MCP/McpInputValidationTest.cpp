// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Server-side `inputSchema` enforcement for tools/call (issue #357 conformance /
// #306 item D hardening). Two surfaces are exercised, both httplib-free:
//   * McpServer::ValidateArguments(schema, args) — the pure validator, driven with
//     schemas built by the real schema-builder DSL (McpSchemaBuilder.h), so the
//     tests assert against exactly what the live tools emit.
//   * HandleToolsCall (via the dispatch seam) — that a malformed call now fails
//     with a field-naming kInvalidParams BEFORE the handler runs, and a valid call
//     still reaches the handler.
// No live OloEditor, GPU, or agent is required (McpServer.cpp is compiled in; the
// DSL header is header-only).
#include "MCP/McpServer.h"
#include "MCP/McpSchemaBuilder.h"

#include <optional>
#include <string>
#include <utility>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    using Json = OloEngine::MCP::Json;
    namespace Schema = OloEngine::MCP::Schema;

    // True if `result` (an std::optional<std::string> from ValidateArguments) holds
    // an error whose text contains `needle`. Fails the calling EXPECT otherwise.
    bool ErrorContains(const std::optional<std::string>& result, std::string_view needle)
    {
        return result.has_value() && result->find(needle) != std::string::npos;
    }

    Json MakeRequest(const Json& id, const std::string& method, const Json& params = Json::object())
    {
        Json req = { { "jsonrpc", "2.0" }, { "method", method } };
        if (!id.is_null())
            req["id"] = id;
        if (!params.is_null())
            req["params"] = params;
        return req;
    }

    // The canonical "one int + one string, int required, closed" schema — the shape
    // most real tool inputs reduce to. Built with the DSL so it is byte-identical to
    // what McpTools.cpp emits.
    Json CountAndTagSchema()
    {
        return Schema::Object()
            .Prop("count", Schema::Int().Min(1).Max(200).Desc("How many."))
            .Prop("tag", Schema::String().Desc("Optional filter."))
            .Required({ "count" })
            .NoAdditional();
    }
} // namespace

// ---- pure validator: happy path -------------------------------------------

TEST(McpInputValidation, HappyPathReturnsNoError)
{
    const Json schema = CountAndTagSchema();
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "count", 50 }, { "tag", "ui" } }).has_value());
    // The optional `tag` may be omitted.
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "count", 1 } }).has_value());
    // Bounds are inclusive at both ends.
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "count", 200 } }).has_value());
}

// ---- pure validator: each vocabulary keyword ------------------------------

TEST(McpInputValidation, MissingRequiredPropertyNamesTheField)
{
    const auto err = McpServer::ValidateArguments(CountAndTagSchema(), Json{ { "tag", "ui" } });
    EXPECT_TRUE(ErrorContains(err, "missing required property 'count'"));
}

TEST(McpInputValidation, WrongTypeNamesFieldAndExpectedType)
{
    const auto err = McpServer::ValidateArguments(CountAndTagSchema(), Json{ { "count", "fifty" } });
    EXPECT_TRUE(ErrorContains(err, "'count'"));
    EXPECT_TRUE(ErrorContains(err, "must be an integer"));
}

TEST(McpInputValidation, IntegerIsStrictAboutWholeValuedFloats)
{
    // A 5.0-style JSON float is NOT a JSON integer — deliberate hardening, and it
    // keeps the validator free of any float-equality test.
    const auto err = McpServer::ValidateArguments(CountAndTagSchema(), Json{ { "count", 5.0 } });
    EXPECT_TRUE(ErrorContains(err, "must be an integer"));
}

TEST(McpInputValidation, IntegerOutOfRangeReportsTheBound)
{
    EXPECT_TRUE(ErrorContains(McpServer::ValidateArguments(CountAndTagSchema(), Json{ { "count", 0 } }),
                              "'count' must be >= 1"));
    EXPECT_TRUE(ErrorContains(McpServer::ValidateArguments(CountAndTagSchema(), Json{ { "count", 201 } }),
                              "'count' must be <= 200"));
}

TEST(McpInputValidation, NumberAcceptsIntegerAndFloatAndEnforcesExclusiveMin)
{
    const Json schema = Schema::Object()
                            .Prop("distance", Schema::Number().ExclusiveMin(0).Desc("World units."))
                            .NoAdditional();
    // A "number" field accepts both a JSON integer and a JSON float.
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "distance", 10 } }).has_value());
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "distance", 2.5 } }).has_value());
    // exclusiveMinimum rejects the bound itself and anything below it.
    EXPECT_TRUE(ErrorContains(McpServer::ValidateArguments(schema, Json{ { "distance", 0 } }),
                              "'distance' must be > 0"));
    EXPECT_TRUE(ErrorContains(McpServer::ValidateArguments(schema, Json{ { "distance", -3.0 } }),
                              "'distance' must be > 0"));
}

TEST(McpInputValidation, EnumRejectsValuesOutsideTheClosedSet)
{
    const Json schema = Schema::Object()
                            .Prop("mode", Schema::String().Enum({ "depth", "normals", "albedo" }))
                            .NoAdditional();
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "mode", "normals" } }).has_value());
    EXPECT_TRUE(ErrorContains(McpServer::ValidateArguments(schema, Json{ { "mode", "bogus" } }),
                              "'mode' must be one of"));
}

TEST(McpInputValidation, ClosedObjectRejectsUnexpectedProperty)
{
    const auto err = McpServer::ValidateArguments(CountAndTagSchema(), Json{ { "count", 1 }, { "extra", true } });
    EXPECT_TRUE(ErrorContains(err, "unexpected property 'extra'"));
}

TEST(McpInputValidation, OpenObjectAllowsUnknownProperties)
{
    // No NoAdditional() => additionalProperties is not closed, so unknown keys pass.
    const Json schema = Schema::Object().Prop("count", Schema::Int()).Required({ "count" });
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "count", 1 }, { "extra", true } }).has_value());
}

TEST(McpInputValidation, EmptyObjectSchemaRejectsAnyArgument)
{
    // Schema::EmptyObject() is the argument-less tool schema (closed, no properties).
    const Json schema = Schema::EmptyObject();
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json::object()).has_value());
    EXPECT_TRUE(ErrorContains(McpServer::ValidateArguments(schema, Json{ { "foo", 1 } }),
                              "unexpected property 'foo'"));
}

// ---- pure validator: nested array (Vec3) + multi-type ----------------------

TEST(McpInputValidation, NestedArrayEnforcesItemsAndLength)
{
    const Json schema = Schema::Object()
                            .Prop("position", Schema::Vec3("Camera eye [x, y, z]."))
                            .Required({ "position" })
                            .NoAdditional();
    // A well-formed 3-number position passes (integers count as numbers).
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "position", Json::array({ 1, 2, 3 }) } }).has_value());
    EXPECT_FALSE(
        McpServer::ValidateArguments(schema, Json{ { "position", Json::array({ 1.5, -2.0, 3.25 }) } }).has_value());

    // Too few / too many elements report the length bound on the named field.
    EXPECT_TRUE(ErrorContains(McpServer::ValidateArguments(schema, Json{ { "position", Json::array({ 1, 2 }) } }),
                              "'position' must have at least 3 items"));
    EXPECT_TRUE(
        ErrorContains(McpServer::ValidateArguments(schema, Json{ { "position", Json::array({ 1, 2, 3, 4 }) } }),
                      "'position' must have at most 3 items"));

    // A non-number element is reported with its index.
    EXPECT_TRUE(
        ErrorContains(McpServer::ValidateArguments(schema, Json{ { "position", Json::array({ 1, "x", 3 }) } }),
                      "'position[1]' must be a number"));
}

TEST(McpInputValidation, MultiTypeUnionAcceptsEitherTypeAndReportsBoth)
{
    // The `sinceId` shape from olo_events_tail: `{ "type": ["integer", "string"] }`.
    const Json schema = Schema::Object()
                            .Prop("sinceId", Schema::Raw(Json{ { "type", Json::array({ "integer", "string" }) } }))
                            .NoAdditional();
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "sinceId", 42 } }).has_value());
    EXPECT_FALSE(McpServer::ValidateArguments(schema, Json{ { "sinceId", "abc" } }).has_value());

    const auto err = McpServer::ValidateArguments(schema, Json{ { "sinceId", true } });
    EXPECT_TRUE(ErrorContains(err, "an integer or a string"));
}

// ---- pure validator: permissive schemas skip cleanly -----------------------

TEST(McpInputValidation, PermissiveSchemaValidatesNothing)
{
    // An empty object, a bare {} schema, and a non-object schema all declare no
    // constraints, so any arguments pass (the dispatcher also skips these).
    EXPECT_FALSE(McpServer::ValidateArguments(Json::object(), Json{ { "anything", 1 } }).has_value());
    EXPECT_FALSE(McpServer::ValidateArguments(Json(nullptr), Json{ { "anything", 1 } }).has_value());
    EXPECT_FALSE(McpServer::ValidateArguments(Json("not-a-schema"), Json{ { "anything", 1 } }).has_value());
}

// ---- dispatch integration: tools/call enforces the schema ------------------

namespace
{
    // Fixture registering one tool with a closed count+tag schema and one tool with
    // no declared schema, so both the enforced and the permissive paths are covered
    // end-to-end through HandleMessage -> HandleToolsCall.
    class McpInputValidationDispatch : public ::testing::Test
    {
      protected:
        McpInputValidationDispatch()
            : m_Server(EditorMcpContext{})
        {
            ToolDef validated;
            validated.Name = "fake_validated";
            validated.Description = "Validates its arguments.";
            validated.InputSchema = CountAndTagSchema();
            validated.Handler = [](McpServer&, const Json&)
            { return ToolResult::Text("ok"); };
            m_Server.RegisterTool(std::move(validated));

            ToolDef open;
            open.Name = "fake_open";
            open.Description = "Declares no schema (permissive).";
            open.Handler = [](McpServer&, const Json&)
            { return ToolResult::Text("ok"); };
            m_Server.RegisterTool(std::move(open));
        }

        Json Call(const Json& arguments)
        {
            return m_Server.HandleMessage(
                MakeRequest(1, "tools/call", Json{ { "name", "fake_validated" }, { "arguments", arguments } }));
        }

        McpServer m_Server;
    };
} // namespace

TEST_F(McpInputValidationDispatch, ValidArgumentsReachHandler)
{
    const Json resp = Call(Json{ { "count", 5 }, { "tag", "ui" } });
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp.contains("error"));
    EXPECT_FALSE(resp["result"]["isError"]);
    EXPECT_EQ(resp["result"]["content"][0]["text"], "ok");
}

TEST_F(McpInputValidationDispatch, MissingRequiredIsInvalidParamsBeforeHandler)
{
    const Json resp = Call(Json::object());
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_FALSE(resp.contains("result")); // handler never ran
    EXPECT_EQ(resp["error"]["code"], -32602);
    const std::string message = resp["error"]["message"].get<std::string>();
    EXPECT_NE(message.find("missing required property 'count'"), std::string::npos);
    EXPECT_NE(message.find("fake_validated"), std::string::npos);
}

TEST_F(McpInputValidationDispatch, WrongTypeIsInvalidParams)
{
    const Json resp = Call(Json{ { "count", "fifty" } });
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602);
    EXPECT_NE(resp["error"]["message"].get<std::string>().find("must be an integer"), std::string::npos);
}

TEST_F(McpInputValidationDispatch, OutOfRangeIsInvalidParams)
{
    const Json resp = Call(Json{ { "count", 9999 } });
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602);
    EXPECT_NE(resp["error"]["message"].get<std::string>().find("must be <= 200"), std::string::npos);
}

TEST_F(McpInputValidationDispatch, UnexpectedPropertyIsInvalidParams)
{
    const Json resp = Call(Json{ { "count", 1 }, { "bogus", true } });
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32602);
    EXPECT_NE(resp["error"]["message"].get<std::string>().find("unexpected property 'bogus'"), std::string::npos);
}

// A tool that declares no InputSchema is permissive: arbitrary arguments reach the
// handler (validation is skipped, not failed).
TEST_F(McpInputValidationDispatch, NoSchemaToolSkipsValidation)
{
    const Json resp = m_Server.HandleMessage(
        MakeRequest(2, "tools/call", Json{ { "name", "fake_open" }, { "arguments", { { "whatever", 123 } } } }));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_FALSE(resp["result"]["isError"]);
}
