// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure shaping behind olo_project_validate (issue #1130,
// Epic H slice 2): turning four per-domain validator payloads into one report,
// and the rule that a validator which could not run is never a clean bill of
// health.
//
// The composition itself — invoking olo_assets_problems / olo_shader_errors /
// olo_script_get_last_errors / olo_render_validate — lives in the editor target
// and needs a live project and render graph. What is pinned here is everything
// the report DECIDES: which payload keys are problems, what `ok` and `complete`
// mean apart from each other, and that truncation is counted rather than
// silent.
#include "MCP/McpProjectValidation.h"

#include <string>
#include <vector>

namespace
{
    using OloEngine::MCP::ProjectValidation::BuildReport;
    using OloEngine::MCP::ProjectValidation::Json;
    using OloEngine::MCP::ProjectValidation::ProblemsFromAssets;
    using OloEngine::MCP::ProjectValidation::ProblemsFromRenderGraph;
    using OloEngine::MCP::ProjectValidation::ProblemsFromScripts;
    using OloEngine::MCP::ProjectValidation::ProblemsFromShaders;
    using OloEngine::MCP::ProjectValidation::RanSection;
    using OloEngine::MCP::ProjectValidation::Section;
    using OloEngine::MCP::ProjectValidation::SectionStatus;
    using OloEngine::MCP::ProjectValidation::UnavailableSection;

    Section CleanSection(const char* name)
    {
        return RanSection(name, "olo_x", "subject", Json::array());
    }
} // namespace

// ---- per-source extraction ----------------------------------------------

TEST(McpProjectValidation, ExtractsAssetProblemsAndTagsTheirKind)
{
    const Json payload = Json::parse(R"({
      "count": 1,
      "problems": [ { "handle": "12", "type": "Texture2D", "path": "a.png", "status": "Invalid" } ]
    })");
    const Json problems = ProblemsFromAssets(payload);
    ASSERT_EQ(problems.size(), 1u);
    EXPECT_EQ(problems[0]["kind"], "AssetLoadFailure");
    EXPECT_EQ(problems[0]["path"], "a.png") << "the source command's own fields ride through untouched";
}

TEST(McpProjectValidation, ExtractsShaderAndScriptErrorsFromTheirOwnArrayKey)
{
    const Json shaders = ProblemsFromShaders(Json::parse(R"({
      "count": 1, "errors": [ { "name": "PBR", "errorMessage": "syntax error" } ] })"));
    ASSERT_EQ(shaders.size(), 1u);
    EXPECT_EQ(shaders[0]["kind"], "ShaderCompileError");

    const Json scripts = ProblemsFromScripts(Json::parse(R"({
      "count": 1, "errors": [ { "language": "csharp", "message": "NullReference" } ] })"));
    ASSERT_EQ(scripts.size(), 1u);
    EXPECT_EQ(scripts[0]["kind"], "ScriptError");
}

// olo_render_validate's own `ok` rests on these three arrays, so these three are
// what the composite calls problems. The barrier/build diagnostics are
// informational there and must stay informational here — promoting them would
// make the composite disagree with the command it composes.
TEST(McpProjectValidation, FlattensTheThreeRenderGraphVerdictsAndLeavesDiagnosticsAlone)
{
    const Json payload = Json::parse(R"({
      "ok": false,
      "hazardCount": 1,
      "hazards": [ { "kind": "ReadAfterWrite", "resource": "HZB", "message": "m" } ],
      "barrierDiagnostics": [ { "kind": "MissingProducer", "message": "informational" } ],
      "buildDiagnostics": [ { "kind": "RegistrationOrderSensitivity", "message": "informational" } ],
      "resolveFailures": [ { "pass": "GTAO", "reason": "no backing", "count": 2 } ],
      "consumedButUnbacked": [ "SceneDepth" ]
    })");
    const Json problems = ProblemsFromRenderGraph(payload);
    ASSERT_EQ(problems.size(), 3u);
    EXPECT_EQ(problems[0]["kind"], "RenderGraphHazard");
    EXPECT_EQ(problems[1]["kind"], "RenderGraphResolveFailure");
    EXPECT_EQ(problems[2]["kind"], "ResourceConsumedButUnbacked");
    EXPECT_EQ(problems[2]["resource"], "SceneDepth");
}

TEST(McpProjectValidation, AMissingOrWrongTypedArrayYieldsNoProblemsRatherThanThrowing)
{
    EXPECT_TRUE(ProblemsFromAssets(Json::object()).empty());
    EXPECT_TRUE(ProblemsFromAssets(Json::parse(R"({"problems": "not an array"})")).empty());
    EXPECT_TRUE(ProblemsFromShaders(Json::parse(R"("not an object")")).empty());
}

// ---- report assembly -----------------------------------------------------

TEST(McpProjectValidation, AllClearIsOkAndComplete)
{
    const Json report = BuildReport({ CleanSection("assets"), CleanSection("shaders") }, 50);
    EXPECT_TRUE(report["ok"].get<bool>());
    EXPECT_TRUE(report["complete"].get<bool>());
    EXPECT_EQ(report["problemCount"].get<int>(), 0);
    EXPECT_EQ(report["sectionsUnavailable"].get<int>(), 0);
    EXPECT_EQ(report["sections"][0]["status"], "ok");
}

TEST(McpProjectValidation, ProblemsMakeItNotOkButStillComplete)
{
    const Json problems = ProblemsFromShaders(Json::parse(R"({"errors": [ { "name": "PBR" } ]})"));
    const Json report = BuildReport({ CleanSection("assets"), RanSection("shaders", "olo_shader_errors", "s",
                                                                         problems) },
                                    50);
    EXPECT_FALSE(report["ok"].get<bool>());
    EXPECT_TRUE(report["complete"].get<bool>()) << "everything ran; the project simply has a problem";
    EXPECT_EQ(report["problemCount"].get<int>(), 1);
    EXPECT_EQ(report["sections"][1]["status"], "problems");
}

// The rule the whole report is shaped around. A headless session has no render
// graph; dropping that section and totalling zero problems would hand back a
// green verdict for a check that never happened.
TEST(McpProjectValidation, AnUnrunValidatorIsNeverACleanBillOfHealth)
{
    const Json report =
        BuildReport({ CleanSection("assets"),
                      UnavailableSection("renderGraph", "olo_render_validate", "s", "No active render graph.") },
                    50);
    EXPECT_FALSE(report["ok"].get<bool>()) << "ok must not survive a section that could not run";
    EXPECT_FALSE(report["complete"].get<bool>());
    EXPECT_EQ(report["problemCount"].get<int>(), 0) << "and it must not invent a problem either";
    EXPECT_EQ(report["sectionsUnavailable"].get<int>(), 1);
    EXPECT_EQ(report["sections"][1]["status"], "unavailable");
    EXPECT_EQ(report["sections"][1]["reason"], "No active render graph.")
        << "the caller has to be told WHY, or it cannot decide whether the gap matters";
}

TEST(McpProjectValidation, TruncationIsCountedAndTheTotalStillReflectsEverything)
{
    Json many = Json::array();
    for (int i = 0; i < 7; ++i)
        many.push_back(Json{ { "kind", "ShaderCompileError" }, { "name", "S" + std::to_string(i) } });

    const Json report = BuildReport({ RanSection("shaders", "olo_shader_errors", "s", many) }, 3);
    const Json& section = report["sections"][0];
    EXPECT_EQ(section["problems"].size(), 3u);
    EXPECT_EQ(section["problemCount"].get<int>(), 7) << "the count is of what was FOUND, not of what was listed";
    EXPECT_EQ(section["omitted"].get<int>(), 4);
    EXPECT_TRUE(section["truncated"].get<bool>());
    EXPECT_EQ(report["problemCount"].get<int>(), 7);
}

TEST(McpProjectValidation, AnEmptySectionListIsVacuouslyOk)
{
    const Json report = BuildReport({}, 50);
    EXPECT_TRUE(report["ok"].get<bool>());
    EXPECT_EQ(report["sectionCount"].get<int>(), 0);
}

TEST(McpProjectValidation, SectionFactoriesSetTheStatusFromWhatWasFound)
{
    EXPECT_EQ(RanSection("a", "s", "d", Json::array()).Status, SectionStatus::Ok);
    EXPECT_EQ(RanSection("a", "s", "d", Json::parse(R"([{"kind":"X"}])")).Status, SectionStatus::Problems);
    EXPECT_EQ(UnavailableSection("a", "s", "d", "why").Status, SectionStatus::Unavailable);
}
