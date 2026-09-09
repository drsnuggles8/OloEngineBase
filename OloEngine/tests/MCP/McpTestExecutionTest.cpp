// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure core of olo_tests_list / olo_tests_run (issue #1130):
// gtest report parsing, OLO_TEST_LAYER classification, and the reconciliation
// that makes a partial run an error instead of a small green number.
//
// The handler that spawns OloEngine-Tests is in the editor target and needs a
// real child process; everything it decides WITH is here, in free functions
// over JSON, so the invariant this issue exists for is pinned in CI rather than
// only observed live. There is a mild irony in the test binary carrying the
// tests for the thing that runs the test binary — the split is what keeps that
// from being circular: nothing below launches anything.
#include "MCP/McpTestExecution.h"

#include <set>
#include <string>
#include <vector>

namespace
{
    using OloEngine::MCP::TestExecution::CaseResult;
    using OloEngine::MCP::TestExecution::CaseStatus;
    using OloEngine::MCP::TestExecution::Clamp;
    using OloEngine::MCP::TestExecution::CountCases;
    using OloEngine::MCP::TestExecution::CountStartedCases;
    using OloEngine::MCP::TestExecution::ExtractLayerMarker;
    using OloEngine::MCP::TestExecution::IsDisabledName;
    using OloEngine::MCP::TestExecution::IsFilterSafeExpression;
    using OloEngine::MCP::TestExecution::IsFilterSafeName;
    using OloEngine::MCP::TestExecution::JoinFilter;
    using OloEngine::MCP::TestExecution::Json;
    using OloEngine::MCP::TestExecution::KnownLayerIds;
    using OloEngine::MCP::TestExecution::LayerCountsJson;
    using OloEngine::MCP::TestExecution::LayerMarker;
    using OloEngine::MCP::TestExecution::NormalizeTestFile;
    using OloEngine::MCP::TestExecution::ParseGTestDuration;
    using OloEngine::MCP::TestExecution::ParseListReport;
    using OloEngine::MCP::TestExecution::ParseRunReport;
    using OloEngine::MCP::TestExecution::Reconcile;
    using OloEngine::MCP::TestExecution::ResolveLayer;
    using OloEngine::MCP::TestExecution::TailOf;
    using OloEngine::MCP::TestExecution::TestCase;

    // A `--gtest_list_tests --gtest_output=json:` document, trimmed to the
    // fields the parser reads. Captured from the real binary: the `file` value
    // is a build-directory-relative __FILE__ with Windows separators, which is
    // the shape the normalizer has to cope with.
    Json ListDocument()
    {
        return Json::parse(R"({
          "tests": 7616,
          "name": "AllTests",
          "testsuites": [
            {
              "name": "EmptySuite",
              "tests": 0,
              "testsuite": []
            },
            {
              "name": "AccessibilitySettingsTest",
              "tests": 2,
              "testsuite": [
                { "name": "DefaultsAreOffAndNeutral",
                  "file": "..\\OloEngine\\tests\\Accessibility\\AccessibilitySettingsTest.cpp", "line": 64 },
                { "name": "DISABLED_NotYet",
                  "file": "..\\OloEngine\\tests\\Accessibility\\AccessibilitySettingsTest.cpp", "line": 90 }
              ]
            }
          ]
        })");
    }

    CaseResult MakeCase(std::string suite, std::string name, CaseStatus status)
    {
        CaseResult result;
        result.Suite = std::move(suite);
        result.Name = std::move(name);
        result.Status = status;
        return result;
    }

    TestCase MakeSelected(std::string suite, std::string name)
    {
        TestCase testCase;
        testCase.Suite = std::move(suite);
        testCase.Name = std::move(name);
        return testCase;
    }
} // namespace

// ---- listing -------------------------------------------------------------

TEST(McpTestExecution, ParsesEveryCaseOutOfAListReport)
{
    std::string error;
    const std::vector<TestCase> cases = ParseListReport(ListDocument(), error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(cases.size(), 2u);
    EXPECT_EQ(cases[0].FullName(), "AccessibilitySettingsTest.DefaultsAreOffAndNeutral");
    EXPECT_EQ(cases[0].Line, 64);
    EXPECT_FALSE(cases[0].Disabled);
    EXPECT_TRUE(cases[1].Disabled);
}

// gtest's list mode reports the WHOLE registered count in its top-level `tests`
// field, ignoring the filter — 7616 here for a two-case selection. Reading it as
// "how many I selected" would make every reconciliation fail by three orders of
// magnitude, so the parser counts entries and never touches that field.
TEST(McpTestExecution, IgnoresTheUnfilteredTopLevelCountInAListReport)
{
    std::string error;
    const std::vector<TestCase> cases = ParseListReport(ListDocument(), error);
    ASSERT_TRUE(error.empty());
    EXPECT_EQ(ListDocument()["tests"].get<int>(), 7616);
    EXPECT_EQ(cases.size(), 2u);
}

// A truncated report from a child that died mid-flush must not read as "the
// selection was empty" — that is the silent partial in its purest form.
TEST(McpTestExecution, AMalformedListReportIsAnErrorNotAnEmptySelection)
{
    std::string error;
    EXPECT_TRUE(ParseListReport(Json::parse(R"({"tests": 3})"), error).empty());
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_TRUE(ParseListReport(Json::parse(R"("not an object")"), error).empty());
    EXPECT_FALSE(error.empty());
}

TEST(McpTestExecution, DetectsTheDisabledPrefixOnEitherHalfOfAName)
{
    EXPECT_TRUE(IsDisabledName("Suite", "DISABLED_Case"));
    EXPECT_TRUE(IsDisabledName("DISABLED_Suite", "Case"));
    EXPECT_FALSE(IsDisabledName("Suite", "Case"));
    EXPECT_FALSE(IsDisabledName("Suite", "NotDISABLED_Case"));
}

// ---- source-path normalization -------------------------------------------

TEST(McpTestExecution, NormalizesAGTestFilePathToTheCatalogueKey)
{
    EXPECT_EQ(NormalizeTestFile("..\\OloEngine\\tests\\Rendering\\RenderGraphTest.cpp", "OloEngine/tests"),
              "OloEngine/tests/Rendering/RenderGraphTest.cpp");
    EXPECT_EQ(NormalizeTestFile("../../OloEngine/tests/Core/RefTest.cpp", "OloEngine/tests"),
              "OloEngine/tests/Core/RefTest.cpp");
    EXPECT_EQ(NormalizeTestFile("C:/repos/olo/OloEngine/tests/Core/RefTest.cpp", "OloEngine/tests"),
              "OloEngine/tests/Core/RefTest.cpp");
}

// A path that is not under the test root cannot be given a catalogue key. The
// normalizer says so with an empty string instead of guessing one, and the
// caller reports it as a classification problem.
TEST(McpTestExecution, RefusesToGuessAKeyForAPathOutsideTheTestRoot)
{
    EXPECT_TRUE(NormalizeTestFile("..\\OloEditor\\src\\Something.cpp", "OloEngine/tests").empty());
}

// ---- OLO_TEST_LAYER classification ---------------------------------------

TEST(McpTestExecution, ReadsAnInFileLayerMarker)
{
    EXPECT_EQ(ExtractLayerMarker("// OLO_TEST_LAYER: L1\n#include <x>\n").Layer, "L1");
    EXPECT_EQ(ExtractLayerMarker("\n\n\t//   OLO_TEST_LAYER:\tFunctional\n").Layer, "Functional");
    EXPECT_EQ(ExtractLayerMarker("#include <x>\n").Layer, "");
    // Duplicate identical markers are tolerated, as in generate_test_catalogue.py.
    EXPECT_EQ(ExtractLayerMarker("// OLO_TEST_LAYER: unit\n// OLO_TEST_LAYER: unit\n").Layer, "unit");
}

TEST(McpTestExecution, TwoDisagreeingMarkersClassifyNothing)
{
    const LayerMarker marker = ExtractLayerMarker("// OLO_TEST_LAYER: L1\n// OLO_TEST_LAYER: L8\n");
    EXPECT_TRUE(marker.Conflict);
    EXPECT_TRUE(marker.Layer.empty());
}

TEST(McpTestExecution, TheMarkerWinsOverTheCatalogueAndTheDuplicateIsReported)
{
    const Json map = Json::parse(R"({"OloEngine/tests/A.cpp": "L8"})");
    const std::set<std::string> known{ "L1", "L8", "unit" };

    const auto both = ResolveLayer("OloEngine/tests/A.cpp", LayerMarker{ "L1", false }, map, known);
    EXPECT_EQ(both.Layer, "L1");
    EXPECT_FALSE(both.Problem.empty()) << "a file registered twice must be reported, not silently accepted";

    const auto markerOnly = ResolveLayer("OloEngine/tests/B.cpp", LayerMarker{ "L1", false }, map, known);
    EXPECT_EQ(markerOnly.Layer, "L1");
    EXPECT_TRUE(markerOnly.Problem.empty());

    const auto mapOnly = ResolveLayer("OloEngine/tests/A.cpp", LayerMarker{}, map, known);
    EXPECT_EQ(mapOnly.Layer, "L8");
    EXPECT_TRUE(mapOnly.Problem.empty());
}

TEST(McpTestExecution, AnUnclassifiedOrUnknownLayerIsReportedNotBlanked)
{
    const Json map = Json::object();
    const std::set<std::string> known{ "L1", "unit" };

    const auto none = ResolveLayer("OloEngine/tests/C.cpp", LayerMarker{}, map, known);
    EXPECT_TRUE(none.Layer.empty());
    EXPECT_NE(none.Problem.find("unclassified"), std::string::npos);

    const auto bogus = ResolveLayer("OloEngine/tests/D.cpp", LayerMarker{ "L99", false }, map, known);
    EXPECT_TRUE(bogus.Layer.empty());
    EXPECT_NE(bogus.Problem.find("L99"), std::string::npos);

    // The duplicate-registration branch must apply the same range check, or an
    // unknown id is accepted on one path and rejected on the other.
    const Json bothMap = Json::parse(R"({"OloEngine/tests/E.cpp": "L1"})");
    const auto bothBogus = ResolveLayer("OloEngine/tests/E.cpp", LayerMarker{ "L99", false }, bothMap, known);
    EXPECT_TRUE(bothBogus.Layer.empty());
    EXPECT_NE(bothBogus.Problem.find("L99"), std::string::npos);
}

TEST(McpTestExecution, KnownLayerIdsAddsTheTwoImplicitTags)
{
    const Json catalogue = Json::parse(R"({
      "functional_tag": "Functional",
      "layers": [ { "id": "L1" }, { "id": "L8" } ]
    })");
    const std::set<std::string> ids = KnownLayerIds(catalogue);
    EXPECT_TRUE(ids.contains("L1"));
    EXPECT_TRUE(ids.contains("unit"));
    EXPECT_TRUE(ids.contains("Functional"));
    // No declared layers means no range check at all, rather than a check that
    // rejects everything.
    EXPECT_TRUE(KnownLayerIds(Json::object()).empty());
}

// ---- run reports ----------------------------------------------------------

TEST(McpTestExecution, ParsesEveryOutcomeOutOfARunReport)
{
    const Json doc = Json::parse(R"({
      "tests": 4, "failures": 1, "name": "AllTests",
      "testsuites": [
        { "name": "S", "tests": 4, "testsuite": [
          { "name": "Passes", "status": "RUN", "result": "COMPLETED", "time": "0.25s",
            "file": "..\\OloEngine\\tests\\S.cpp", "line": 10 },
          { "name": "Fails", "status": "RUN", "result": "COMPLETED", "time": "1.5s",
            "failures": [ { "failure": "S.cpp:12\nExpected equality\n", "type": "" } ] },
          { "name": "Skips", "status": "RUN", "result": "SKIPPED", "time": "0s",
            "skipped": [ { "message": "S.cpp:20\nNo GL 4.6 context" } ] },
          { "name": "DISABLED_Off", "status": "NOTRUN", "result": "SUPPRESSED", "time": "0s" }
        ] }
      ]
    })");

    const auto report = ParseRunReport(doc);
    ASSERT_TRUE(report.Error.empty()) << report.Error;
    ASSERT_EQ(report.Cases.size(), 4u);

    EXPECT_EQ(report.Cases[0].Status, CaseStatus::Passed);
    EXPECT_DOUBLE_EQ(report.Cases[0].Seconds, 0.25);
    EXPECT_EQ(report.Cases[0].Line, 10);

    EXPECT_EQ(report.Cases[1].Status, CaseStatus::Failed);
    ASSERT_EQ(report.Cases[1].Messages.size(), 1u);
    EXPECT_NE(report.Cases[1].Messages[0].find("Expected equality"), std::string::npos)
        << "the verbatim gtest message is the whole point — a caller must not have to scrape the console for it";

    EXPECT_EQ(report.Cases[2].Status, CaseStatus::Skipped);
    ASSERT_EQ(report.Cases[2].Messages.size(), 1u);
    EXPECT_NE(report.Cases[2].Messages[0].find("No GL 4.6 context"), std::string::npos);

    EXPECT_EQ(report.Cases[3].Status, CaseStatus::Disabled);
}

// gtest reports a case that skipped AND failed as SKIPPED. Taking that at face
// value would turn a red into a benign skip, which on this repo — where the GPU
// suites skip constantly — is the failure most likely to be missed.
TEST(McpTestExecution, ARecordedFailureOutranksASkippedResult)
{
    const Json doc = Json::parse(R"({
      "testsuites": [ { "name": "S", "testsuite": [
        { "name": "SkippedThenFailed", "status": "RUN", "result": "SKIPPED", "time": "0s",
          "skipped": [ { "message": "gave up" } ],
          "failures": [ { "failure": "but asserted first", "type": "" } ] }
      ] } ]
    })");
    const auto report = ParseRunReport(doc);
    ASSERT_EQ(report.Cases.size(), 1u);
    EXPECT_EQ(report.Cases[0].Status, CaseStatus::Failed);
}

// gtest reports a fatal failure in SetUpTestSuite / TearDownTestSuite / a global
// environment as a pseudo-entry with an EMPTY name — inside the owning suite, or
// under a synthetic `NonTestSuiteFailure` suite. Read as cases they become the
// full name "Suite.", which no listing ever contains, so reconciliation would
// call them `unexpected` and report a real setup failure as "the run was
// PARTIAL" — burying the cause under a plumbing complaint.
TEST(McpTestExecution, SuiteLevelFailuresAreKeptOutOfTheCaseList)
{
    const Json doc = Json::parse(R"({
      "testsuites": [
        { "name": "S", "tests": 1, "testsuite": [
          { "name": "Real", "status": "RUN", "result": "COMPLETED", "time": "0s" },
          { "name": "", "status": "RUN", "result": "COMPLETED", "time": "0s", "classname": "",
            "failures": [ { "failure": "S.cpp:5\nSetUpTestSuite blew up", "type": "" } ] }
        ] },
        { "name": "NonTestSuiteFailure", "tests": 1, "testsuite": [
          { "name": "", "status": "RUN", "result": "COMPLETED", "time": "0s", "classname": "",
            "failures": [ { "failure": "env.cpp:9\nGlobal environment SetUp failed", "type": "" } ] }
        ] }
      ]
    })");

    const auto report = ParseRunReport(doc);
    ASSERT_TRUE(report.Error.empty()) << report.Error;
    ASSERT_EQ(report.Cases.size(), 1u) << "only the named case is a case";
    EXPECT_EQ(report.Cases[0].FullName(), "S.Real");

    ASSERT_EQ(report.SuiteFailures.size(), 2u);
    EXPECT_EQ(report.SuiteFailures[0].Suite, "S");
    ASSERT_EQ(report.SuiteFailures[0].Messages.size(), 1u);
    EXPECT_NE(report.SuiteFailures[0].Messages[0].find("SetUpTestSuite blew up"), std::string::npos);
    EXPECT_EQ(report.SuiteFailures[1].Suite, "NonTestSuiteFailure");

    // And the run still reconciles, so the setup failure is reported as itself.
    const auto reconciliation = Reconcile({ MakeSelected("S", "Real") }, report.Cases);
    EXPECT_TRUE(reconciliation.Complete())
        << "a suite-level failure must not be mistaken for an unexpected case";
}

TEST(McpTestExecution, AMalformedRunReportIsAnError)
{
    EXPECT_FALSE(ParseRunReport(Json::parse(R"({"tests": 0})")).Error.empty());
}

TEST(McpTestExecution, ParsesGTestDurations)
{
    EXPECT_DOUBLE_EQ(ParseGTestDuration("17.444s"), 17.444);
    EXPECT_DOUBLE_EQ(ParseGTestDuration("0s"), 0.0);
    EXPECT_DOUBLE_EQ(ParseGTestDuration(""), 0.0);
    EXPECT_DOUBLE_EQ(ParseGTestDuration("garbage"), 0.0);
    EXPECT_DOUBLE_EQ(ParseGTestDuration("-3s"), 0.0);
}

// ---- reconciliation: the invariant ---------------------------------------

TEST(McpTestExecution, AFullyAccountedRunReconciles)
{
    const std::vector<TestCase> selected{ MakeSelected("S", "A"), MakeSelected("S", "B") };
    const std::vector<CaseResult> reported{ MakeCase("S", "A", CaseStatus::Passed),
                                            MakeCase("S", "B", CaseStatus::Failed) };
    const auto reconciliation = Reconcile(selected, reported);
    EXPECT_TRUE(reconciliation.Complete());
    EXPECT_EQ(reconciliation.Expected, 2u);
    EXPECT_EQ(reconciliation.Reported, 2u);
}

// The case this whole design exists for: the child crashed after two of three
// cases, so gtest's report is short. The counts alone would read as a perfectly
// ordinary two-case run; only the by-name diff says which case never reported.
TEST(McpTestExecution, APartialRunIsIncompleteAndNamesWhatIsMissing)
{
    const std::vector<TestCase> selected{ MakeSelected("S", "A"), MakeSelected("S", "B"), MakeSelected("S", "C") };
    const std::vector<CaseResult> reported{ MakeCase("S", "A", CaseStatus::Passed),
                                            MakeCase("S", "B", CaseStatus::Passed) };
    const auto reconciliation = Reconcile(selected, reported);
    EXPECT_FALSE(reconciliation.Complete());
    ASSERT_EQ(reconciliation.Missing.size(), 1u);
    EXPECT_EQ(reconciliation.Missing[0], "S.C");
    EXPECT_TRUE(reconciliation.Unexpected.empty());
}

// A count-only check passes this: two selected, two reported. The names do not.
TEST(McpTestExecution, ASwappedCaseIsCaughtEvenThoughTheCountsMatch)
{
    const std::vector<TestCase> selected{ MakeSelected("S", "A"), MakeSelected("S", "B") };
    const std::vector<CaseResult> reported{ MakeCase("S", "A", CaseStatus::Passed),
                                            MakeCase("S", "Z", CaseStatus::Passed) };
    const auto reconciliation = Reconcile(selected, reported);
    EXPECT_EQ(reconciliation.Expected, reconciliation.Reported);
    EXPECT_FALSE(reconciliation.Complete()) << "equal counts must not be mistaken for the same set";
    EXPECT_EQ(reconciliation.Missing, std::vector<std::string>{ "S.B" });
    EXPECT_EQ(reconciliation.Unexpected, std::vector<std::string>{ "S.Z" });
}

TEST(McpTestExecution, AnEmptyReportAgainstANonEmptySelectionIsIncomplete)
{
    const std::vector<TestCase> selected{ MakeSelected("S", "A") };
    const auto reconciliation = Reconcile(selected, {});
    EXPECT_FALSE(reconciliation.Complete());
    EXPECT_EQ(reconciliation.Missing.size(), 1u);
}

// ---- counting -------------------------------------------------------------

TEST(McpTestExecution, CountsEachStatusAndTheBucketsAddUp)
{
    const std::vector<CaseResult> cases{
        MakeCase("S", "A", CaseStatus::Passed),
        MakeCase("S", "B", CaseStatus::Failed),
        MakeCase("S", "C", CaseStatus::Skipped),
        MakeCase("S", "D", CaseStatus::Disabled),
        MakeCase("S", "E", CaseStatus::Passed),
    };
    const auto counts = CountCases(cases);
    EXPECT_EQ(counts.Total, 5u);
    EXPECT_EQ(counts.Passed, 2u);
    EXPECT_EQ(counts.Failed, 1u);
    EXPECT_EQ(counts.Skipped, 1u);
    EXPECT_EQ(counts.Disabled, 1u);
    EXPECT_TRUE(counts.AddsUp());
}

TEST(McpTestExecution, LayerCountsBucketTheUnclassifiedExplicitly)
{
    std::vector<TestCase> cases{ MakeSelected("S", "A"), MakeSelected("S", "B"), MakeSelected("S", "C") };
    cases[0].Layer = "L1";
    cases[1].Layer = "L1";
    // cases[2] stays unclassified.
    const Json counts = LayerCountsJson(cases);
    EXPECT_EQ(counts["L1"].get<int>(), 2);
    EXPECT_EQ(counts["unclassified"].get<int>(), 1)
        << "an unclassified case must be counted somewhere visible, not dropped";
}

// ---- filters and truncation ----------------------------------------------

TEST(McpTestExecution, AcceptsOnlyNamesAGTestFilterCanCarry)
{
    EXPECT_TRUE(IsFilterSafeName("Suite.Case"));
    EXPECT_TRUE(IsFilterSafeName("Instantiation/Suite.Case/0"));
    EXPECT_FALSE(IsFilterSafeName(""));
    EXPECT_FALSE(IsFilterSafeName("Suite.Case With Space"));
    EXPECT_FALSE(IsFilterSafeName("Suite.\"Quoted\""));
    // ':' delimits filter terms, so a name containing one cannot be expressed.
    EXPECT_FALSE(IsFilterSafeName("Suite.A:B"));
}

// The filter is the one selection input written freehand, and it is pasted into
// the child's command line. A quote in it would close the quoting and let the
// rest be read as further gtest flags — a second --gtest_output redirects the
// report and the run then fails as "exited without writing a report".
TEST(McpTestExecution, AFilterExpressionMayCarryWildcardsButNoQuoting)
{
    EXPECT_TRUE(IsFilterSafeExpression("*"));
    EXPECT_TRUE(IsFilterSafeExpression("RenderGraphTest.*"));
    EXPECT_TRUE(IsFilterSafeExpression("A.B:C.*-*Slow*"));
    EXPECT_TRUE(IsFilterSafeExpression("Instantiation/Suite.Case/0"));
    EXPECT_FALSE(IsFilterSafeExpression(""));
    EXPECT_FALSE(IsFilterSafeExpression("a\" --gtest_output=json:evil.json \""));
    EXPECT_FALSE(IsFilterSafeExpression("a b"));
    EXPECT_FALSE(IsFilterSafeExpression("a&b"));
}

TEST(McpTestExecution, JoinsNamesWithTheFilterDelimiter)
{
    EXPECT_EQ(JoinFilter({ "A.B", "C.D" }), "A.B:C.D");
    EXPECT_EQ(JoinFilter({}), "");
}

TEST(McpTestExecution, CountsRunBannersForProgress)
{
    EXPECT_EQ(CountStartedCases("[ RUN      ] S.A\n[       OK ] S.A\n[ RUN      ] S.B\n"), 2u);
    EXPECT_EQ(CountStartedCases("no banners here"), 0u);
}

TEST(McpTestExecution, TruncationIsMarkedAndCounted)
{
    const std::string long_(100, 'x');
    const std::string clamped = Clamp(long_, 10);
    EXPECT_NE(clamped.find("truncated"), std::string::npos);
    EXPECT_NE(clamped.find("90"), std::string::npos);
    EXPECT_EQ(Clamp("short", 10), "short");

    const std::string tail = TailOf(long_, 10);
    EXPECT_NE(tail.find("omitted"), std::string::npos);
    // A dead child's log is diagnosed from its END, so that is what is kept.
    EXPECT_TRUE(tail.ends_with(std::string(10, 'x')));
    EXPECT_EQ(TailOf("short", 10), "short");
}
