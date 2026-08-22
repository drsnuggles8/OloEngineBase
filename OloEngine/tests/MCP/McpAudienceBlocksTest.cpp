// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Audience-tagged content blocks (#673; MCP spec 2025-06-18
// `Annotations.audience` / `.priority` on a ContentBlock). Three concerns, in
// increasing scope:
//
//   1. The MECHANISM, over the httplib-free dispatch seam with fake tools:
//      ToolResult::AnnotateBlock's emitted shape, ToolResult::StructuredDualAudience,
//      and the ToolDef::DualAudienceContent re-shaping in HandleToolsCall —
//      including the guards that make it idempotent and content-preserving.
//   2. The pure Markdown renderer (MCP/McpAudienceReport.h) that produces the
//      user block: tables, elision bounds, and the escaping/truncation rules
//      that keep the result valid Markdown AND valid UTF-8.
//   3. The ADOPTION RATCHET over the REAL builtin surface. Like
//      McpOutputSchemaCoverageTest this is registration-only — it drives
//      RegisterBuiltinTools and reads the ToolDefs, never issuing a tools/call —
//      so no handler runs, no game thread is pumped, and no GL context is
//      needed. That is exactly why adoption is declared on the ToolDef rather
//      than chosen inside the handler.
//
// NOTE this is a DIFFERENT spec field from ToolDef::Annotations (readOnlyHint /
// openWorldHint), which McpToolAnnotationsTest guards: those annotate the tool
// DECLARATION, these annotate content blocks inside a tool RESULT.
#include "MCP/McpAudienceReport.h"
#include "MCP/McpServer.h"
#include "MCP/McpTools.h"

#include <set>
#include <string>
#include <utility>

namespace
{
    using OloEngine::MCP::EditorMcpContext;
    using OloEngine::MCP::McpServer;
    using OloEngine::MCP::ToolDef;
    using OloEngine::MCP::ToolResult;
    namespace AudienceReport = OloEngine::MCP::AudienceReport;
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

    Json SamplePayload()
    {
        return Json{ { "drawCalls", 412 },
                     { "frameTimeMs", 17.2 },
                     { "passes", Json::array({ Json{ { "pass", "ShadowPass" }, { "gpuMs", 1.2 } },
                                               Json{ { "pass", "ScenePass" }, { "gpuMs", 6.7 } } }) } };
    }

    // A tool that declares the dual-audience opt-in and returns a plain
    // Structured() result — the shape every real adopter has.
    void AddDualAudienceTool(McpServer& server, std::string name, Json payload = SamplePayload())
    {
        ToolDef tool;
        tool.Name = std::move(name);
        tool.Title = "Fake report";
        tool.Description = "A dual-audience tool.";
        tool.DualAudienceContent = true;
        tool.Handler = [payload = std::move(payload)](McpServer&, const Json&)
        { return ToolResult::Structured(payload); };
        server.RegisterTool(std::move(tool));
    }

    Json CallTool(McpServer& server, const Json& id, const std::string& name)
    {
        return server.HandleMessage(MakeRequest(id, "tools/call", Json{ { "name", name } }));
    }

    // Every ToolDef the real builtin registration produces. The snapshot is a
    // shared_ptr<const ToolList>, so it outlives the local server — no handler is
    // ever invoked, only the declarations are read.
    const McpServer::ToolList& BuiltinTools()
    {
        static const McpServer::ToolSnapshot snapshot = []
        {
            McpServer server{ EditorMcpContext{} };
            OloEngine::MCP::RegisterBuiltinTools(server);
            return server.ToolsSnapshot();
        }();
        return *snapshot;
    }
} // namespace

// ---- AnnotateBlock: the emitted annotation shape ------------------------------

TEST(McpAudienceBlocks, AnnotateBlockEmitsAudienceArrayAndPriority)
{
    Json block{ { "type", "text" }, { "text", "hi" } };
    ToolResult::AnnotateBlock(block, ToolResult::Audience::User, 0.25);

    ASSERT_TRUE(block.contains("annotations"));
    const Json& annotations = block["annotations"];
    ASSERT_TRUE(annotations["audience"].is_array());
    ASSERT_EQ(annotations["audience"].size(), 1u);
    EXPECT_EQ(annotations["audience"][0], "user");
    EXPECT_DOUBLE_EQ(annotations["priority"].get<double>(), 0.25);
    // The block itself is untouched apart from the added key.
    EXPECT_EQ(block["text"], "hi");
}

TEST(McpAudienceBlocks, AnnotateBlockOmitsPriorityWhenUnset)
{
    Json block{ { "type", "text" }, { "text", "hi" } };
    ToolResult::AnnotateBlock(block, ToolResult::Audience::Assistant);

    EXPECT_EQ(block["annotations"]["audience"][0], "assistant");
    EXPECT_FALSE(block["annotations"].contains("priority"))
        << "an unset priority must be omitted, never emitted as 0 (0 means 'entirely optional')";
}

TEST(McpAudienceBlocks, AnnotateBlockClampsAnOutOfRangePriority)
{
    Json high{ { "type", "text" } };
    Json low{ { "type", "text" } };
    ToolResult::AnnotateBlock(high, ToolResult::Audience::User, 42.0);
    ToolResult::AnnotateBlock(low, ToolResult::Audience::User, 0.0);
    EXPECT_DOUBLE_EQ(high["annotations"]["priority"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(low["annotations"]["priority"].get<double>(), 0.0);
}

// It composes with the other block factories rather than replacing them.
TEST(McpAudienceBlocks, AnnotateBlockComposesWithResourceLinkBlock)
{
    Json link = ToolResult::ResourceLinkBlock("olo://capture/1", "shot.png", "A capture.", "image/png", 9);
    ToolResult::AnnotateBlock(link, ToolResult::Audience::User, 0.5);
    EXPECT_EQ(link["type"], "resource_link");
    EXPECT_EQ(link["size"], 9);
    EXPECT_EQ(link["annotations"]["audience"][0], "user");
}

// ---- StructuredDualAudience: the factory --------------------------------------

TEST(McpAudienceBlocks, DualAudienceFactoryEmitsOneBlockPerAudience)
{
    const Json data = SamplePayload();
    const ToolResult r = ToolResult::StructuredDualAudience(data, "Performance snapshot");

    EXPECT_FALSE(r.IsError);
    EXPECT_EQ(r.StructuredContent, data) << "structuredContent is unchanged — only `content` differs";
    ASSERT_TRUE(r.Content.is_array());
    ASSERT_EQ(r.Content.size(), 2u);

    // Block 0 stays the machine-readable JSON mirror, so content[0] keeps meaning
    // what it always meant for a client that only reads the first block.
    EXPECT_EQ(r.Content[0]["type"], "text");
    EXPECT_EQ(r.Content[0]["annotations"]["audience"][0], "assistant");
    EXPECT_EQ(Json::parse(r.Content[0]["text"].get<std::string>()), data);
    EXPECT_EQ(r.Content[0]["text"], data.dump()) << "the machine mirror is compact, not dump(2)";

    EXPECT_EQ(r.Content[1]["type"], "text");
    EXPECT_EQ(r.Content[1]["annotations"]["audience"][0], "user");
    const std::string report = r.Content[1]["text"].get<std::string>();
    EXPECT_NE(report.find("Performance snapshot"), std::string::npos);
    EXPECT_NE(report.find("ShadowPass"), std::string::npos);

    // Priorities: the data is required, the rendering is presentational.
    EXPECT_GT(r.Content[0]["annotations"]["priority"].get<double>(),
              r.Content[1]["annotations"]["priority"].get<double>());
}

// ---- dispatch: ToolDef::DualAudienceContent -----------------------------------

TEST(McpAudienceBlocks, ToolsCallReshapesADeclaredToolIntoTwoAudienceBlocks)
{
    McpServer server{ EditorMcpContext{} };
    AddDualAudienceTool(server, "olo_fake_dual");

    const Json resp = CallTool(server, 1, "olo_fake_dual");
    ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
    const Json& content = resp["result"]["content"];
    ASSERT_EQ(content.size(), 2u);
    EXPECT_EQ(content[0]["annotations"]["audience"][0], "assistant");
    EXPECT_EQ(content[1]["annotations"]["audience"][0], "user");
    // The typed result is untouched by the re-shaping.
    EXPECT_EQ(resp["result"]["structuredContent"], SamplePayload());
    // The heading comes from ToolDef::Title.
    EXPECT_NE(content[1]["text"].get<std::string>().find("Fake report"), std::string::npos);
}

TEST(McpAudienceBlocks, ToolsCallLeavesAnUndeclaredToolAlone)
{
    McpServer server{ EditorMcpContext{} };
    ToolDef tool;
    tool.Name = "olo_fake_plain";
    tool.Description = "A plain structured tool.";
    tool.Handler = [](McpServer&, const Json&)
    { return ToolResult::Structured(SamplePayload()); };
    server.RegisterTool(std::move(tool));

    const Json resp = CallTool(server, 2, "olo_fake_plain");
    ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
    const Json& content = resp["result"]["content"];
    ASSERT_EQ(content.size(), 1u) << "opting out must leave the single-block shape byte-identical";
    EXPECT_EQ(content[0]["text"], SamplePayload().dump(2));
    EXPECT_FALSE(content[0].contains("annotations"));
}

// The guards: the re-shaping only fires on a plain single-block typed success,
// so it can never drop a block a handler deliberately added, annotate an error,
// or double-apply to a result that already carries the pair.
TEST(McpAudienceBlocks, ReshapingSkipsErrorsTextResultsAndExtraBlocks)
{
    McpServer server{ EditorMcpContext{} };

    ToolDef failing;
    failing.Name = "olo_fake_dual_error";
    failing.Description = "Fails.";
    failing.DualAudienceContent = true;
    failing.Handler = [](McpServer&, const Json&)
    { return ToolResult::Error("boom"); };
    server.RegisterTool(std::move(failing));

    ToolDef textual;
    textual.Name = "olo_fake_dual_text";
    textual.Description = "Text only.";
    textual.DualAudienceContent = true;
    textual.Handler = [](McpServer&, const Json&)
    { return ToolResult::Text("flowchart LR"); };
    server.RegisterTool(std::move(textual));

    ToolDef withLink;
    withLink.Name = "olo_fake_dual_link";
    withLink.Description = "Structured plus a resource link.";
    withLink.DualAudienceContent = true;
    withLink.Handler = [](McpServer&, const Json&)
    {
        ToolResult r = ToolResult::Structured(SamplePayload());
        r.Content.push_back(ToolResult::ResourceLinkBlock("olo://capture/1", "shot.png", "A capture.", "image/png"));
        return r;
    };
    server.RegisterTool(std::move(withLink));

    const Json errorResp = CallTool(server, 3, "olo_fake_dual_error");
    EXPECT_EQ(errorResp["result"]["isError"], true);
    ASSERT_EQ(errorResp["result"]["content"].size(), 1u);
    EXPECT_FALSE(errorResp["result"]["content"][0].contains("annotations"));

    const Json textResp = CallTool(server, 4, "olo_fake_dual_text");
    ASSERT_EQ(textResp["result"]["content"].size(), 1u)
        << "no structuredContent (e.g. the topology tool's mermaid format) => nothing to render";
    EXPECT_EQ(textResp["result"]["content"][0]["text"], "flowchart LR");

    const Json linkResp = CallTool(server, 5, "olo_fake_dual_link");
    ASSERT_EQ(linkResp["result"]["content"].size(), 2u)
        << "a handler's own extra block must survive, not be replaced by the audience pair";
    EXPECT_EQ(linkResp["result"]["content"][1]["type"], "resource_link");
}

// The human block goes out over the same wire as the JSON mirror, so it must be
// scrubbed on exactly the same terms — the re-shaping runs BEFORE redaction.
TEST(McpAudienceBlocks, RedactionScrubsBothAudienceBlocks)
{
    McpServer server{ EditorMcpContext{} };
    AddDualAudienceTool(server, "olo_fake_dual_paths",
                        Json{ { "asset", "C:\\Projects\\Game\\mesh.obj" }, { "count", 1 } });
    server.SetRedactPaths(true);

    const Json resp = CallTool(server, 6, "olo_fake_dual_paths");
    ASSERT_TRUE(resp.contains("result")) << resp.dump(2);
    const Json& content = resp["result"]["content"];
    ASSERT_EQ(content.size(), 2u);
    for (const Json& block : content)
    {
        const std::string text = block["text"].get<std::string>();
        EXPECT_EQ(text.find("C:\\"), std::string::npos) << "a raw drive path leaked in: " << text;
        EXPECT_NE(text.find("<path>"), std::string::npos) << text;
    }
}

// ---- the pure Markdown renderer ----------------------------------------------

TEST(McpAudienceReport, RendersScalarsAsAFieldTableAndObjectArraysAsTables)
{
    const std::string report = AudienceReport::Render(SamplePayload(), "Perf");

    EXPECT_NE(report.find("### Perf"), std::string::npos);
    // Scalars land in the field table...
    EXPECT_NE(report.find("| drawCalls"), std::string::npos);
    EXPECT_NE(report.find("412"), std::string::npos);
    // ...and the array of objects becomes its own table with a header row.
    EXPECT_NE(report.find("**passes** (2)"), std::string::npos);
    EXPECT_NE(report.find("| pass"), std::string::npos);
    EXPECT_NE(report.find("ScenePass"), std::string::npos);
    EXPECT_EQ(report.back(), '\n');
}

TEST(McpAudienceReport, EmptyAndNonObjectPayloadsStillProduceABlock)
{
    // A tool result is never allowed to render as an empty user block.
    EXPECT_FALSE(AudienceReport::Render(Json::object(), "Empty").empty());
    EXPECT_NE(AudienceReport::Render(Json::object(), "Empty").find("(no data)"), std::string::npos);
    EXPECT_NE(AudienceReport::Render(Json(42), "Scalar").find("42"), std::string::npos);
    // A bare array of objects renders as one table (no enclosing key).
    const Json rows = Json::array({ Json{ { "a", 1 } }, Json{ { "a", 2 } } });
    EXPECT_NE(AudienceReport::Render(rows, "Rows").find("| a"), std::string::npos);
}

TEST(McpAudienceReport, NeutralisesPipesAndNewlinesThatWouldBreakTheTable)
{
    const Json data{ { "name", "a|b" }, { "multi", "one\ntwo" } };
    const std::string report = AudienceReport::Render(data, "Escapes");
    EXPECT_NE(report.find("a\\|b"), std::string::npos) << "a literal pipe would open a phantom column";
    EXPECT_EQ(report.find("one\ntwo"), std::string::npos) << "a newline would end the row early";
    EXPECT_NE(report.find("one two"), std::string::npos);
}

// KEYS are escaped too, not just values: a payload keyed by data (a
// std::unordered_map<std::string, V> field — entity names, asset paths) renders
// its keys as column headers and section titles, where one stray '|' would
// corrupt the whole table.
TEST(McpAudienceReport, EscapesDataDerivedKeysInHeadersAndSectionTitles)
{
    const Json data{ { "a|b", 1 },                                             // field key
                     { "sec|tion", Json{ { "x", 1 } } },                       // section title
                     { "tbl|e", Json::array({ Json{ { "col|umn", 1 } } }) } }; // table title + header

    const std::string report = AudienceReport::Render(data, "Keys");
    EXPECT_NE(report.find("a\\|b"), std::string::npos) << report;
    EXPECT_NE(report.find("**sec\\|tion**"), std::string::npos) << report;
    EXPECT_NE(report.find("**tbl\\|e**"), std::string::npos) << report;
    EXPECT_NE(report.find("col\\|umn"), std::string::npos) << report;

    // An escaped column header must still resolve its cells — the lookup uses the
    // RAW key while only the printed header is escaped.
    const auto rowLine = report.find("\n| 1 ");
    EXPECT_NE(rowLine, std::string::npos) << "escaped header lost its row value:\n"
                                          << report;
}

TEST(McpAudienceReport, ElidesLongTablesAndListsRatherThanLosingThem)
{
    Json rows = Json::array();
    for (int i = 0; i < 100; ++i)
        rows.push_back(Json{ { "i", i } });
    const std::string report = AudienceReport::Render(Json{ { "rows", rows } }, "Big");

    EXPECT_NE(report.find("**rows** (100)"), std::string::npos) << "the true count is always stated";
    EXPECT_NE(report.find("more row(s) omitted"), std::string::npos);
    // The bound actually held: row 99 is not printed.
    EXPECT_EQ(report.find("| 99 "), std::string::npos);
}

// The truncation bound must never split a UTF-8 sequence: nlohmann's dump()
// throws on invalid UTF-8, so a byte-wise cut through a multi-byte codepoint
// would turn a cosmetic limit into a failed tool response.
TEST(McpAudienceReport, TruncationKeepsTheOutputValidUtf8)
{
    // A cut that lands mid-sequence must back off to the codepoint boundary.
    // "x" + 3-byte U+20AC repeated puts a continuation byte at every index
    // where (i - 1) % 3 != 0, so this exercises the backoff directly.
    std::string wide = "x";
    for (int i = 0; i < 400; ++i)
        wide += "\xE2\x82\xAC";

    for (const sizet limit : { sizet{ 60 }, sizet{ 61 }, sizet{ 62 }, sizet{ 600 } })
    {
        const std::string cut = AudienceReport::Detail::TruncateUtf8(wide, limit);
        // The proof of validity: nlohmann's dump() throws on invalid UTF-8.
        EXPECT_NO_THROW((void)Json(cut).dump()) << "cut at " << limit << " split a UTF-8 sequence";
        EXPECT_LT(cut.size(), wide.size());
    }

    // ...and end to end, through both the paragraph and the table-cell path.
    const Json data{ { "name", wide }, { "rows", Json::array({ Json{ { "cell", wide } } }) } };
    const Json block{ { "type", "text" }, { "text", AudienceReport::Render(data, "Utf8") } };
    EXPECT_NO_THROW((void)block.dump());
}

TEST(McpAudienceReport, NestedObjectsBecomeSectionsAndEmptyContainersAreLabelled)
{
    const Json data{ { "frame", Json{ { "cpuMs", 3.1 }, { "gpuMs", 9.4 } } },
                     { "warnings", Json::array() },
                     { "tags", Json::array({ "a", "b" }) } };
    const std::string report = AudienceReport::Render(data, "Nested");

    EXPECT_NE(report.find("**frame**"), std::string::npos);
    EXPECT_NE(report.find("cpuMs"), std::string::npos);
    EXPECT_NE(report.find("(none)"), std::string::npos) << "an empty array must read as empty, not vanish";
    EXPECT_NE(report.find("a, b"), std::string::npos) << "a scalar array is inlined";
}

// ---- adoption ratchet over the REAL builtin surface --------------------------

// The deliberate adopter list (#673). Each entry is multi-field or
// tabular AND is something a human stares at while debugging — see the
// inclusion rule on ToolDef::DualAudienceContent, and the per-tool reason at
// each registration site. This test is the ratchet in BOTH directions: a new
// tool that opts in without being considered fails here, and an adopter that
// silently loses the flag fails here too.
TEST(McpAudienceBlocks, BuiltinAdoptionMatchesTheDeliberateList)
{
    static const std::set<std::string> kExpectedAdopters = {
        "olo_gpu_resources",                // live GPU object table + device heap table
        "olo_memory_report",                // per-resource-type breakdown table
        "olo_perf_snapshot",                // ~18 frame counters
        "olo_perf_pass_timings",            // per-pass gpuMs/cpuMs table
        "olo_perf_cpu_scopes",              // the editor's own CPU Scopes table
        "olo_render_frame_breakdown",       // command list + per-pass breakdown
        "olo_render_graph_topology_export", // passes / edges / resources tables
        "olo_render_why_not_visible",       // explainer check list
        "olo_render_target_stats",          // per-channel min/max/mean/NaN table
        "olo_cluster_grid_stats",           // per-slice + histogram tables
        "olo_shadow_atlas_layout",          // granted vs starved casters
        "olo_physics_why_no_collision",     // explainer check list
    };

    const McpServer::ToolList& tools = BuiltinTools();
    ASSERT_FALSE(tools.empty());

    std::set<std::string> actual;
    for (const ToolDef& tool : tools)
    {
        if (tool.DualAudienceContent)
            actual.insert(tool.Name);
    }

    EXPECT_EQ(actual, kExpectedAdopters)
        << "the dual-audience adopter set drifted — re-read the inclusion rule on "
           "ToolDef::DualAudienceContent before widening it; blanket adoption is bloat, not parity.";
}

// An adopter must be able to PRODUCE the pair: the re-shaping only fires on a
// typed result, and the report is headed by Title. A tool that opts in without
// an OutputSchema/Structured result would silently render nothing new.
TEST(McpAudienceBlocks, EveryAdopterDeclaresAnOutputSchemaAndATitle)
{
    for (const ToolDef& tool : BuiltinTools())
    {
        if (!tool.DualAudienceContent)
            continue;
        EXPECT_FALSE(tool.Title.empty())
            << tool.Name << " opts into dual-audience content but has no Title to head the report.";
        EXPECT_TRUE(tool.OutputSchema.is_object() && !tool.OutputSchema.empty())
            << tool.Name
            << " opts into dual-audience content but declares no OutputSchema — the re-shaping only "
               "fires on a ToolResult::Structured result, so it would never take effect.";
    }
}
