// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpVirtualShadowMapStats.h"

namespace
{
    using namespace OloEngine::MCP;

    TEST(McpVirtualShadowMapStats, ZeroCountersRemainAValidPreviousFrameSample)
    {
        VirtualShadowMapStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = true;
        snapshot.State.HasData = true;
        snapshot.State.Freshness = StatsSnapshot::FreshnessModel::PreviousFrame;
        snapshot.State.SampleAgeFrames = 1u;
        snapshot.PhysicalResolution = 2048u;
        snapshot.PageSize = 64u;
        snapshot.PhysicalPageCount = 1024u;
        snapshot.VRAMBytes = 16u * 1024u * 1024u;

        const auto report = VirtualShadowMapStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "ready");
        EXPECT_TRUE(report.at("availability").at("hasData").get<bool>());
        EXPECT_EQ(report.at("freshness").at("model"), "previousFrame");
        EXPECT_EQ(report.at("freshness").at("sampleAgeFrames").get<u64>(), 1u);
        EXPECT_EQ(report.at("statistics").at("pagesDrawn").get<u32>(), 0u);
        EXPECT_EQ(report.at("statistics").at("pagesFailed").get<u32>(), 0u);
        EXPECT_EQ(report.at("physicalPool").at("pageCount").get<u64>(), 1024u);
    }

    TEST(McpVirtualShadowMapStats, DisabledFeatureDoesNotPublishRetainedCountersAsCurrentData)
    {
        VirtualShadowMapStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = false;
        snapshot.State.HasData = false;
        snapshot.PagesResident = 99u; // ignored retained cache

        const auto report = VirtualShadowMapStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "disabled");
        EXPECT_FALSE(report.contains("statistics"));
        EXPECT_FALSE(report.contains("physicalPool"));
        EXPECT_TRUE(report.at("freshness").at("sampleAgeFrames").is_null());
    }

    TEST(McpVirtualShadowMapStats, AvailableEnabledFeatureCanExplicitlyHaveNoReturnedSample)
    {
        VirtualShadowMapStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = true;
        snapshot.State.HasData = false;

        const auto report = VirtualShadowMapStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "noData");
        EXPECT_FALSE(report.contains("statistics"));
    }
} // namespace
