// OLO_TEST_LAYER: unit
//
// Pure JSON shaping behind `olo_frame_graph_declaration_stats` (issue #607,
// gap logged from #1333). The handler needs MarshalRead and a live
// RenderPipeline; the decisions an agent acts on live in the header, so they
// are pinned here:
//
//   * every counter and the last compile cause reach the response;
//   * a stale-cache detection is visible at the top level;
//   * with verify mode off, zero detections must not read as a clean cache;
//   * a mean over nothing is null, not 0.

#include "OloEnginePCH.h"

#include "MCP/McpFrameGraphDeclarationStats.h"

#include <gtest/gtest.h>

namespace Report = OloEngine::MCP::FrameGraphDeclaration;
using OloEngine::FrameGraphDeclarationStats;

namespace
{
    FrameGraphDeclarationStats SteadyStats()
    {
        FrameGraphDeclarationStats stats;
        stats.Frames = 120;
        stats.Compiles = 4;
        stats.CacheHits = 116;
        stats.RedundantCompiles = 1;
        stats.CompileMicrosTotal = 800.0;
        stats.CacheHitMicrosTotal = 232.0;
        stats.LastCompileMicros = 190.0;
        stats.LastCacheHitMicros = 2.0;
        stats.DigestMicrosTotal = 40.0;
        stats.LastCompileCause = "RenderPath";
        return stats;
    }
} // namespace

TEST(McpFrameGraphDeclarationStats, EveryCounterReachesTheReport)
{
    const auto report = Report::BuildStatsReport(SteadyStats(), /*verifyMode=*/true);

    EXPECT_EQ(report.at("frames").get<u64>(), 120u);
    EXPECT_EQ(report.at("compiles").get<u64>(), 4u);
    EXPECT_EQ(report.at("cacheHits").get<u64>(), 116u);
    EXPECT_EQ(report.at("redundantCompiles").get<u64>(), 1u);
    EXPECT_EQ(report.at("verifiedHits").get<u64>(), 0u);
    EXPECT_EQ(report.at("staleCacheDetections").get<u64>(), 0u);
    EXPECT_EQ(report.at("lastCompileCause").get<std::string>(), "RenderPath");
    EXPECT_TRUE(report.at("lastStaleCacheDetail").get<std::string>().empty());

    const auto& timing = report.at("timing");
    EXPECT_DOUBLE_EQ(timing.at("compileMicrosTotal").get<f64>(), 800.0);
    EXPECT_DOUBLE_EQ(timing.at("cacheHitMicrosTotal").get<f64>(), 232.0);
    EXPECT_DOUBLE_EQ(timing.at("digestMicrosTotal").get<f64>(), 40.0);
    EXPECT_DOUBLE_EQ(timing.at("lastCompileMicros").get<f64>(), 190.0);
    EXPECT_DOUBLE_EQ(timing.at("lastCacheHitMicros").get<f64>(), 2.0);
    EXPECT_DOUBLE_EQ(timing.at("meanCompileMicros").get<f64>(), 200.0);
    EXPECT_DOUBLE_EQ(timing.at("meanCacheHitMicros").get<f64>(), 2.0);
    EXPECT_NEAR(report.at("cacheHitRate").get<f64>(), 116.0 / 120.0, 1e-12);

    EXPECT_FALSE(report.at("anyStale").get<bool>());
    EXPECT_FALSE(report.contains("note")) << "a verified, clean cache needs no explanation";
}

// THE ONE THAT MATTERS: a stale plan is the defect this instrument exists to
// find, and it must be readable without scanning the counters.
TEST(McpFrameGraphDeclarationStats, StaleDetectionIsReportedAtTheTopLevel)
{
    auto stats = SteadyStats();
    stats.VerifiedHits = 116;
    stats.StaleCacheDetections = 3;
    stats.LastStaleCacheDetail = "pass:SSGIPass (cause: PassStates)";

    const auto report = Report::BuildStatsReport(stats, /*verifyMode=*/true);

    EXPECT_TRUE(report.at("anyStale").get<bool>());
    EXPECT_EQ(report.at("staleCacheDetections").get<u64>(), 3u);
    EXPECT_EQ(report.at("lastStaleCacheDetail").get<std::string>(), "pass:SSGIPass (cause: PassStates)");
    ASSERT_TRUE(report.contains("note"));
    EXPECT_NE(report.at("note").get<std::string>().find("FrameGraphDeclarationConfig"), std::string::npos);
}

// With verify mode off the detection counter is never incremented, so a zero
// there is the absence of a check. The response has to say that rather than
// let an agent read "0 stale" as a passed check.
TEST(McpFrameGraphDeclarationStats, VerifyModeOffSaysNobodyChecked)
{
    const auto report = Report::BuildStatsReport(SteadyStats(), /*verifyMode=*/false);

    EXPECT_FALSE(report.at("verifyMode").get<bool>());
    EXPECT_FALSE(report.at("anyStale").get<bool>());
    ASSERT_TRUE(report.contains("note"));
    const auto note = report.at("note").get<std::string>();
    EXPECT_NE(note.find("nobody checked"), std::string::npos);
    EXPECT_NE(note.find("OLO_RG_VERIFY_DECLARATION_CACHE"), std::string::npos);
}

TEST(McpFrameGraphDeclarationStats, MeansOverNothingAreNull)
{
    const auto report = Report::BuildStatsReport(FrameGraphDeclarationStats{}, /*verifyMode=*/true);

    EXPECT_TRUE(report.at("timing").at("meanCompileMicros").is_null());
    EXPECT_TRUE(report.at("timing").at("meanCacheHitMicros").is_null());
    EXPECT_TRUE(report.at("cacheHitRate").is_null());
    EXPECT_EQ(report.at("frames").get<u64>(), 0u);
}
