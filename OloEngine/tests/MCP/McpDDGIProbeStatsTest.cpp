// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpDDGIProbeStats.h"

namespace
{
    using namespace OloEngine::MCP;

    TEST(McpDDGIProbeStats, ShapesCurrentCountersAndCascadeLattices)
    {
        DDGIProbeStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = true;
        snapshot.State.HasData = true;
        snapshot.State.Freshness = StatsSnapshot::FreshnessModel::CurrentBlockingReadback;
        snapshot.State.SampleAgeFrames = 0u;
        snapshot.TotalProbes = 8192u;
        snapshot.LiveProbes = 120u;
        snapshot.ActiveProbes = 96u;
        snapshot.RelitProbes = 80u;
        snapshot.CapturedProbes = 100u;
        snapshot.BlendedProbes = 72u;
        snapshot.UncapturedLive = 20u;
        snapshot.BounceCoverage = 0.25f;
        snapshot.Cascades = {
            DDGIProbeStats::Cascade{ .Origin = { 0.0f, 1.0f, 2.0f },
                                     .Spacing = { 1.0f, 1.0f, 1.0f },
                                     .LatticeMin = { -8, -7, -6 },
                                     .Dimensions = { 16, 16, 16 } },
            DDGIProbeStats::Cascade{ .Origin = { 0.0f, 1.0f, 2.0f },
                                     .Spacing = { 2.0f, 2.0f, 2.0f },
                                     .LatticeMin = { -4, -3, -2 },
                                     .Dimensions = { 16, 16, 16 } },
        };

        const auto report = DDGIProbeStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("freshness").at("model"), "currentBlockingReadback");
        EXPECT_EQ(report.at("totalProbes").get<u32>(), 8192u);
        EXPECT_EQ(report.at("statistics").at("uncapturedLive").get<u32>(), 20u);
        EXPECT_FLOAT_EQ(report.at("bounceCoverage").get<f32>(), 0.25f);
        ASSERT_EQ(report.at("cascades").size(), 2u);
        EXPECT_EQ(report.at("cascades")[1].at("level").get<u32>(), 1u);
        EXPECT_EQ(report.at("cascades")[1].at("spacing"), nlohmann::json({ 2.0f, 2.0f, 2.0f }));
        EXPECT_EQ(report.at("cascades")[0].at("latticeMin"), nlohmann::json({ -8, -7, -6 }));
    }

    TEST(McpDDGIProbeStats, ZeroBounceCoverageIsMeasuredWhileNoHitsIsNull)
    {
        DDGIProbeStats::Snapshot measured;
        measured.State.Available = true;
        measured.State.Enabled = true;
        measured.State.HasData = true;
        measured.BounceCoverage = 0.0f;

        const auto measuredReport = DDGIProbeStats::BuildReport(measured);
        ASSERT_TRUE(measuredReport.at("bounceCoverage").is_number());
        EXPECT_FLOAT_EQ(measuredReport.at("bounceCoverage").get<f32>(), 0.0f);

        DDGIProbeStats::Snapshot unknown = measured;
        unknown.BounceCoverage.reset();
        const auto unknownReport = DDGIProbeStats::BuildReport(unknown);
        EXPECT_TRUE(unknownReport.at("bounceCoverage").is_null());
    }

    TEST(McpDDGIProbeStats, DisabledPassDoesNotExposeAPreviousVolumesProbeTable)
    {
        DDGIProbeStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = false;
        snapshot.State.HasData = false;
        snapshot.TotalProbes = 4096u; // ignored retained pass state

        const auto report = DDGIProbeStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "disabled");
        EXPECT_FALSE(report.contains("statistics"));
        EXPECT_FALSE(report.contains("cascades"));
    }
} // namespace
