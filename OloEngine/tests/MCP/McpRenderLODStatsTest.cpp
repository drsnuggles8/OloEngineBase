// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpRenderLODStats.h"

namespace
{
    using namespace OloEngine::MCP;

    TEST(McpRenderLODStats, PreservesSparseLevelHistogramAndDocumentsCumulativeLifetime)
    {
        RenderLODStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = true;
        snapshot.State.HasData = true;
        snapshot.State.Freshness = StatsSnapshot::FreshnessModel::SessionCumulative;
        snapshot.LODSwitches = 7u;
        snapshot.ObjectsPerLODLevel = { 12u, 0u, 3u };

        const auto report = RenderLODStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "ready");
        EXPECT_EQ(report.at("freshness").at("model"), "sessionCumulative");
        EXPECT_EQ(report.at("lodSwitches").get<u32>(), 7u);
        EXPECT_EQ(report.at("objectsPerLODLevel"), nlohmann::json({ 12u, 0u, 3u }));
    }

    TEST(McpRenderLODStats, EmptyHistogramAndZeroSwitchesAreStillData)
    {
        RenderLODStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = true;
        snapshot.State.HasData = true;
        snapshot.State.Freshness = StatsSnapshot::FreshnessModel::SessionCumulative;

        const auto report = RenderLODStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "ready");
        EXPECT_EQ(report.at("lodSwitches").get<u32>(), 0u);
        EXPECT_TRUE(report.at("objectsPerLODLevel").empty());
    }

    TEST(McpRenderLODStats, MissingRendererIsUnavailableRatherThanAnEmptyHistogram)
    {
        RenderLODStats::Snapshot snapshot;
        snapshot.State.Available = false;

        const auto report = RenderLODStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "unavailable");
        EXPECT_FALSE(report.contains("lodSwitches"));
        EXPECT_FALSE(report.contains("objectsPerLODLevel"));
    }
} // namespace
