// OLO_TEST_LAYER: unit
//
// Pure JSON shaping behind `olo_gpu_readback_stats` (issue #721).
//
// The handler cannot be driven from the test binary (it needs MarshalRead, a
// game thread and GL), which is exactly why every decision worth testing lives
// in the header this file exercises. What is worth testing is not the field
// names — it is the two shaping decisions an agent's behaviour depends on:
//
//   * a raised overflow flag must be reachable WITHOUT reading the counters;
//   * "nothing has come back yet" and "everything counted zero" must not
//     serialise to the same thing.

#include "OloEnginePCH.h"

#include "MCP/McpGpuReadbackStats.h"

#include <gtest/gtest.h>

namespace Stats = OloEngine::MCP::GpuReadbackStats;

namespace
{
    Stats::StatsSnapshot HealthySnapshot()
    {
        Stats::StatsSnapshot snapshot;
        snapshot.Enabled = true;
        snapshot.Valid = true;
        snapshot.FrameIndex = 4242;
        snapshot.LatencyFrames = 3;
        snapshot.RingSlots = 3;
        snapshot.SlotsInFlight = 1;
        snapshot.Counters.push_back({ "InstanceCullInput", "Instances submitted", 1000u });
        snapshot.Counters.push_back({ "InstanceCullDrawn", "Instances drawn", 640u });
        return snapshot;
    }
} // namespace

TEST(McpGpuReadbackStats, CleanFrameReportsNoOverflow)
{
    const auto report = Stats::BuildStatsReport(HealthySnapshot());

    EXPECT_TRUE(report.at("enabled").get<bool>());
    EXPECT_TRUE(report.at("valid").get<bool>());
    EXPECT_FALSE(report.at("anyOverflow").get<bool>());
    EXPECT_TRUE(report.at("overflows").empty());
    EXPECT_FALSE(report.contains("note")) << "a healthy frame must not carry an explanatory note";

    // The counters' provenance travels with them. Without frameIndex an agent
    // cannot tell a counter that stopped updating from one that is genuinely
    // constant — the specific way a stats channel starts lying.
    EXPECT_EQ(report.at("frameIndex").get<u64>(), 4242u);
    EXPECT_EQ(report.at("latencyFrames").get<u32>(), 3u);
    EXPECT_EQ(report.at("counters").size(), 2u);
    EXPECT_EQ(report.at("counters")[0].at("name").get<std::string>(), "InstanceCullInput");
    EXPECT_EQ(report.at("counters")[0].at("value").get<u32>(), 1000u);
}

// THE ONE THAT MATTERS. An overflow must be visible at the top of the response.
TEST(McpGpuReadbackStats, OverflowIsReportedAtTheTopLevel)
{
    auto snapshot = HealthySnapshot();
    snapshot.Overflows.push_back({ "InstanceCullOutput", "The GPU instance cull's output buffer truncated" });

    const auto report = Stats::BuildStatsReport(snapshot);

    EXPECT_TRUE(report.at("anyOverflow").get<bool>());
    ASSERT_EQ(report.at("overflows").size(), 1u);
    EXPECT_EQ(report.at("overflows")[0].at("name").get<std::string>(), "InstanceCullOutput");
    // The description, not just the name: the name alone tells an agent WHICH
    // flag, not WHAT truncated, and the second question is the useful one.
    EXPECT_FALSE(report.at("overflows")[0].at("description").get<std::string>().empty());
}

// Only the flags that fired are listed, so "nothing is wrong" and "three things
// are wrong" have visibly different shapes rather than differing by three
// booleans buried in a fixed-length list.
TEST(McpGpuReadbackStats, OnlyFiredFlagsAreListed)
{
    auto snapshot = HealthySnapshot();
    snapshot.Overflows.push_back({ "VSMRequestRing", "The VSM page-request ring truncated" });
    snapshot.Overflows.push_back({ "VSMPhysicalPool", "The VSM physical page pool ran dry" });

    const auto report = Stats::BuildStatsReport(snapshot);
    ASSERT_EQ(report.at("overflows").size(), 2u);
    for (const auto& entry : report.at("overflows"))
    {
        EXPECT_NE(entry.at("name").get<std::string>(), "InstanceCullOutput")
            << "a flag that did not fire was listed anyway";
    }
}

// "Not back yet" is a different fact from "all zero", and an agent acts
// differently on them: one says retry, the other says the pass did nothing.
TEST(McpGpuReadbackStats, NotYetValidIsDistinguishableFromAllZero)
{
    Stats::StatsSnapshot pending;
    pending.Enabled = true;
    pending.Valid = false;
    pending.RingSlots = 3;
    pending.Counters.push_back({ "InstanceCullInput", "Instances submitted", 0u });

    const auto pendingReport = Stats::BuildStatsReport(pending);
    EXPECT_FALSE(pendingReport.at("valid").get<bool>());
    ASSERT_TRUE(pendingReport.contains("note"));
    EXPECT_NE(pendingReport.at("note").get<std::string>().find("readback ring"), std::string::npos);

    Stats::StatsSnapshot zeroed = pending;
    zeroed.Valid = true;
    const auto zeroedReport = Stats::BuildStatsReport(zeroed);
    EXPECT_TRUE(zeroedReport.at("valid").get<bool>());
    EXPECT_FALSE(zeroedReport.contains("note"));
    EXPECT_NE(pendingReport, zeroedReport) << "a pending channel and an all-zero frame serialise identically";
}

// A disabled channel says so, rather than reporting zeros that read as "the
// passes did nothing".
TEST(McpGpuReadbackStats, DisabledChannelSaysSoRatherThanReportingZeros)
{
    Stats::StatsSnapshot off;
    off.Enabled = false;
    off.Valid = false;
    off.RingSlots = 3;

    const auto report = Stats::BuildStatsReport(off);
    EXPECT_FALSE(report.at("enabled").get<bool>());
    ASSERT_TRUE(report.contains("note"));
    EXPECT_NE(report.at("note").get<std::string>().find("disabled"), std::string::npos);
}

// A saturated ring means captures are being skipped, so the counters are older
// than latencyFrames says. Named in the response rather than left as a
// two-number derivation the caller has to know to make.
TEST(McpGpuReadbackStats, RingSaturationIsNamed)
{
    auto snapshot = HealthySnapshot();
    EXPECT_FALSE(Stats::BuildStatsReport(snapshot).at("ringSaturated").get<bool>());

    snapshot.SlotsInFlight = snapshot.RingSlots;
    EXPECT_TRUE(Stats::BuildStatsReport(snapshot).at("ringSaturated").get<bool>());
}
