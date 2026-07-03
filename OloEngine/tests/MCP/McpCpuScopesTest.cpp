// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure JSON shaping behind olo_perf_cpu_scopes (issue #519,
// first slice: expose PerformanceProfiler's per-scope CPU timings over MCP).
// The shaping lives in a header over engine-free input structs
// (MCP/McpCpuScopes.h), so it is exercised here against synthetic scope data —
// the test binary deliberately does NOT compile McpTools.cpp (the
// editor-backed handler). The live tool's Application::GetProfilerPreviousFrameData
// -> JSON path is verified over the MCP attach loop; this pins the
// sort/limit/rounding/degradation shape.
#include "MCP/McpCpuScopes.h"

#include <string>
#include <vector>

namespace
{
    using OloEngine::MCP::CpuScopes::BuildCpuScopes;
    using OloEngine::MCP::CpuScopes::Round3;
    using OloEngine::MCP::CpuScopes::ScopeEntry;
    using Json = OloEngine::MCP::CpuScopes::Json;
} // namespace

TEST(McpCpuScopesTest, UnavailableReportsEmptyScopesRegardlessOfInput)
{
    // Even if the caller (erroneously) passes populated entries, a Dist build
    // must not surface them as real data — the status alone communicates
    // "can't time anything here", not "nothing happened to run".
    const std::vector<ScopeEntry> entries = { { "Scene::OnUpdateRuntime", 5.0f, 1 } };

    const Json o = BuildCpuScopes(entries, /*scopesCompiledIn*/ false, /*limit*/ 0);

    EXPECT_EQ(o["status"], "unavailable");
    EXPECT_TRUE(o["scopes"].empty());
    EXPECT_DOUBLE_EQ(o["totalTimeMs"].get<double>(), 0.0);
    EXPECT_EQ(o["scopeCount"].get<int>(), 0);
    EXPECT_NE(o["note"].get<std::string>().find("Distribution"), std::string::npos);
}

TEST(McpCpuScopesTest, EmptyScopesWhenCompiledInReportsOkNoData)
{
    const Json o = BuildCpuScopes({}, /*scopesCompiledIn*/ true, /*limit*/ 0);

    EXPECT_EQ(o["status"], "ok_no_data");
    EXPECT_TRUE(o["scopes"].empty());
    EXPECT_DOUBLE_EQ(o["totalTimeMs"].get<double>(), 0.0);
    EXPECT_EQ(o["scopeCount"].get<int>(), 0);
}

TEST(McpCpuScopesTest, SortsScopesByTimeDescending)
{
    const std::vector<ScopeEntry> entries = {
        { "Scene::UpdatePhysics", 1.5f, 1 },
        { "Scene::OnUpdateRuntime", 6.2f, 1 },
        { "Scene::UpdateScripts", 3.1f, 2 },
    };

    const Json o = BuildCpuScopes(entries, true, 0);

    EXPECT_EQ(o["status"], "ok");
    ASSERT_EQ(o["scopes"].size(), 3u);
    EXPECT_EQ(o["scopes"][0]["name"], "Scene::OnUpdateRuntime");
    EXPECT_EQ(o["scopes"][1]["name"], "Scene::UpdateScripts");
    EXPECT_EQ(o["scopes"][2]["name"], "Scene::UpdatePhysics");
    EXPECT_EQ(o["scopes"][1]["samples"].get<unsigned>(), 2u);
}

TEST(McpCpuScopesTest, LimitTruncatesButScopeCountReflectsFullSet)
{
    const std::vector<ScopeEntry> entries = {
        { "A", 3.0f, 1 },
        { "B", 2.0f, 1 },
        { "C", 1.0f, 1 },
    };

    const Json o = BuildCpuScopes(entries, true, /*limit*/ 2);

    ASSERT_EQ(o["scopes"].size(), 2u);
    EXPECT_EQ(o["scopes"][0]["name"], "A");
    EXPECT_EQ(o["scopes"][1]["name"], "B");
    EXPECT_EQ(o["scopeCount"].get<int>(), 3);
}

TEST(McpCpuScopesTest, TotalTimeMsSumsAllScopesNotJustLimited)
{
    const std::vector<ScopeEntry> entries = {
        { "A", 3.0f, 1 },
        { "B", 2.0f, 1 },
        { "C", 1.0f, 1 },
    };

    const Json o = BuildCpuScopes(entries, true, /*limit*/ 1);

    EXPECT_DOUBLE_EQ(o["totalTimeMs"].get<double>(), 6.0);
}

TEST(McpCpuScopesTest, RoundsToThreeDecimals)
{
    EXPECT_DOUBLE_EQ(Round3(0.0004999), 0.0);
    EXPECT_DOUBLE_EQ(Round3(0.0005001), 0.001);
    EXPECT_DOUBLE_EQ(Round3(1.23456), 1.235);
}

TEST(McpCpuScopesTest, ZeroLimitMeansNoTruncation)
{
    std::vector<ScopeEntry> entries;
    for (int i = 0; i < 10; ++i)
        entries.push_back({ "Scope" + std::to_string(i), static_cast<float>(i), 1 });

    const Json o = BuildCpuScopes(entries, true, /*limit*/ 0);

    EXPECT_EQ(o["scopes"].size(), 10u);
}
