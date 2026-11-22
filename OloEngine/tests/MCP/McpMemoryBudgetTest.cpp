// OLO_TEST_LAYER: unit
//
// The argument parsing and reply shape behind olo_memory_budget_set (#1343), the
// tool that injects a device memory budget into the renderer's admission. The
// tool takes exactly one of `megabytes` and `clear`, and a budget it accepts must
// be a real positive byte count: a NaN, a zero or an absurd value reaching
// Renderer3D::SetMemoryBudgetOverride would read as a device with no memory, or
// with no limit, and degrade (or never degrade) the frame for no reason a reader
// could find.

#include "OloEnginePCH.h"
#include "MCP/McpMemoryBudget.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <string>

namespace
{
    namespace MB = OloEngine::MCP::MemoryBudget;
    using Json = nlohmann::json;

    constexpr u64 kMiB = 1024ull * 1024ull;

    // A sentinel no accepted call produces, so "ParseArgs left `out` alone on an
    // error" is observable.
    constexpr u64 kUntouched = 7;

    [[nodiscard]] std::optional<std::string> Parse(const Json& args, MB::Args& out)
    {
        out.OverrideBytes = kUntouched;
        return MB::ParseArgs(args, out);
    }

    void ExpectRejected(const Json& args)
    {
        MB::Args out;
        const auto error = Parse(args, out);
        ASSERT_TRUE(error.has_value()) << args.dump();
        EXPECT_FALSE(error->empty()) << args.dump();
        ASSERT_TRUE(out.OverrideBytes.has_value()) << args.dump();
        EXPECT_EQ(*out.OverrideBytes, kUntouched) << "a rejected call must not half-fill the result: " << args.dump();
    }
} // namespace

TEST(McpMemoryBudget, MegabytesBecomeAnOverrideInBytes)
{
    MB::Args out;
    ASSERT_FALSE(Parse(Json{ { "megabytes", 512 } }, out).has_value());
    ASSERT_TRUE(out.OverrideBytes.has_value());
    EXPECT_EQ(*out.OverrideBytes, 512ull * kMiB);

    // A fractional budget is legal; `clear: false` beside it is the same as no `clear`.
    ASSERT_FALSE(Parse(Json{ { "megabytes", 1.5 }, { "clear", false } }, out).has_value());
    ASSERT_TRUE(out.OverrideBytes.has_value());
    EXPECT_EQ(*out.OverrideBytes, kMiB + kMiB / 2);

    // The ceiling itself is accepted, and is exact.
    ASSERT_FALSE(Parse(Json{ { "megabytes", MB::kMaxMegabytes } }, out).has_value());
    ASSERT_TRUE(out.OverrideBytes.has_value());
    EXPECT_EQ(*out.OverrideBytes, static_cast<u64>(MB::kMaxMegabytes) * kMiB);
}

TEST(McpMemoryBudget, APositiveBudgetBelowOneByteIsOneByteNotZero)
{
    MB::Args out;
    ASSERT_FALSE(Parse(Json{ { "megabytes", 1e-12 } }, out).has_value());
    ASSERT_TRUE(out.OverrideBytes.has_value());
    EXPECT_EQ(*out.OverrideBytes, 1u) << "a zero-byte override would read as 'no budget'";
}

TEST(McpMemoryBudget, ClearGivesTheDeviceBudgetBack)
{
    MB::Args out;
    ASSERT_FALSE(Parse(Json{ { "clear", true } }, out).has_value());
    EXPECT_FALSE(out.OverrideBytes.has_value());

    // A null `megabytes` is an absent one.
    ASSERT_FALSE(Parse(Json{ { "clear", true }, { "megabytes", nullptr } }, out).has_value());
    EXPECT_FALSE(out.OverrideBytes.has_value());
}

TEST(McpMemoryBudget, ExactlyOneOfMegabytesAndClearIsRequired)
{
    ExpectRejected(Json{ { "megabytes", 256 }, { "clear", true } });
    ExpectRejected(Json::object());
    ExpectRejected(Json{ { "clear", false } });
    ExpectRejected(Json{ { "megabytes", nullptr } });
    ExpectRejected(Json::array());

    // The two errors say different things: which of the two mistakes was made.
    MB::Args out;
    const auto both = Parse(Json{ { "megabytes", 256 }, { "clear", true } }, out);
    const auto neither = Parse(Json::object(), out);
    ASSERT_TRUE(both.has_value());
    ASSERT_TRUE(neither.has_value());
    EXPECT_NE(*both, *neither);
    EXPECT_NE(both->find("not both"), std::string::npos) << *both;
}

TEST(McpMemoryBudget, ABudgetThatIsNotAPositiveFiniteSizeIsRejected)
{
    ExpectRejected(Json{ { "megabytes", std::numeric_limits<f64>::quiet_NaN() } });
    ExpectRejected(Json{ { "megabytes", std::numeric_limits<f64>::infinity() } });
    ExpectRejected(Json{ { "megabytes", -std::numeric_limits<f64>::infinity() } });
    ExpectRejected(Json{ { "megabytes", 0 } });
    ExpectRejected(Json{ { "megabytes", 0.0 } });
    ExpectRejected(Json{ { "megabytes", -1 } });
    ExpectRejected(Json{ { "megabytes", -0.5 } });
    ExpectRejected(Json{ { "megabytes", MB::kMaxMegabytes + 1 } });
    ExpectRejected(Json{ { "megabytes", 1e30 } });
    ExpectRejected(Json{ { "megabytes", "512" } });
    ExpectRejected(Json{ { "megabytes", true } });
    ExpectRejected(Json{ { "clear", "yes" } });
    ExpectRejected(Json{ { "clear", 1 } });
}

TEST(McpMemoryBudget, TheSchemaStatesTheSameBoundsTheParserEnforces)
{
    const Json schema = MB::InputSchema();
    EXPECT_EQ(schema["type"], "object");
    EXPECT_EQ(schema["additionalProperties"], false);
    EXPECT_FALSE(schema.contains("required")) << "either argument alone is a valid call";

    const Json& megabytes = schema["properties"]["megabytes"];
    EXPECT_EQ(megabytes["type"], "number");
    EXPECT_EQ(megabytes["exclusiveMinimum"], 0);
    EXPECT_EQ(megabytes["maximum"], MB::kMaxMegabytes);
    // Bounds stay JSON integers (notes-mcp-tool-authoring.md rule 10).
    EXPECT_TRUE(megabytes["exclusiveMinimum"].is_number_integer());
    EXPECT_TRUE(megabytes["maximum"].is_number_integer());
    EXPECT_EQ(schema["properties"]["clear"]["type"], "boolean");
}

TEST(McpMemoryBudget, TheReplyCarriesTheOverrideTheAdmissionAndTheOneFrameCaveat)
{
    const Json admission{ { "reduced", true }, { "reason", "deviceBudget" }, { "level", 3 } };

    const Json injected = MB::BuildResponse(std::optional<u64>(256ull * kMiB), admission);
    EXPECT_EQ(injected["overrideBytes"], 256ull * kMiB);
    EXPECT_EQ(injected["admission"], admission);
    EXPECT_EQ(injected["note"], "the decision is taken at the next frame");

    // No override and no admitting renderer are both null, never 0 or an empty object.
    const Json cleared = MB::BuildResponse(std::nullopt, nullptr);
    EXPECT_TRUE(cleared["overrideBytes"].is_null());
    EXPECT_TRUE(cleared["admission"].is_null());
    EXPECT_EQ(cleared["note"], injected["note"]);

    // Every required output field is one the reply really carries.
    const Json output = MB::OutputSchema();
    EXPECT_FALSE(output.contains("additionalProperties"));
    for (const Json& name : output["required"])
    {
        EXPECT_TRUE(output["properties"].contains(name.get<std::string>())) << name;
        EXPECT_TRUE(cleared.contains(name.get<std::string>())) << name;
    }
}

// Registration only: no handler runs, so this is safe with no renderer.
TEST(McpMemoryBudget, TheToolIsARuntimeOverrideNotAConsentedProjectWrite)
{
    OloEngine::MCP::McpServer server{ OloEngine::MCP::EditorMcpContext{} };
    OloEngine::MCP::RegisterBuiltinTools(server);
    const auto tools = server.ToolsSnapshot();
    const OloEngine::MCP::ToolDef* budget = nullptr;
    for (const auto& tool : *tools)
    {
        if (tool.Name == "olo_memory_budget_set")
            budget = &tool;
    }
    ASSERT_NE(budget, nullptr);
    EXPECT_EQ(budget->Toolset, "perf");
    // The override is session state in the renderer: never serialized, so not gated
    // behind write consent. It still mutates, and says so.
    EXPECT_FALSE(budget->ProjectWrite);
    EXPECT_FALSE(budget->Annotations.value("readOnlyHint", true));
    EXPECT_FALSE(budget->Annotations.value("destructiveHint", true));
    EXPECT_TRUE(budget->Annotations.value("idempotentHint", false));
    // Renderer3D state is written on the main thread, and the reversal mechanism
    // is declared (an undeclared one is refused from every transaction).
    EXPECT_TRUE(budget->MainMarshaled);
    EXPECT_EQ(budget->Undo, OloEngine::Automation::AutomationUndo::None);
    EXPECT_EQ(budget->InputSchema, MB::InputSchema());
    EXPECT_EQ(budget->OutputSchema, MB::OutputSchema());
}
