// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpRayTracingStats.h"

namespace
{
    using namespace OloEngine::MCP;
    namespace RT = OloEngine::RayTracing;

    [[nodiscard]] RayTracingStats::Snapshot ReadySnapshot()
    {
        RayTracingStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = true;
        snapshot.State.HasData = true;
        snapshot.State.Freshness = StatsSnapshot::FreshnessModel::PreviousFrame;
        snapshot.Capabilities.Supported = true;
        snapshot.Capabilities.Reason = RT::UnsupportedReason::None;
        snapshot.Capabilities.Properties.MinScratchOffsetAlignment = 128;
        snapshot.Capabilities.Properties.MaxInstanceCount = 16777215;
        return snapshot;
    }

    TEST(McpRayTracingStats, AReadySampleCarriesEveryCounterTheIssueAsksFor)
    {
        RayTracingStats::Snapshot snapshot = ReadySnapshot();
        snapshot.Stats.Resident.BlasByClass[static_cast<sizet>(RT::GeometryClass::Static)] = 12u;
        snapshot.Stats.Resident.BlasByClass[static_cast<sizet>(RT::GeometryClass::Masked)] = 3u;
        snapshot.Stats.Resident.UnsupportedInstances = 5u;
        snapshot.Stats.Resident.TlasInstances = 15u;
        snapshot.Stats.Resident.AccelerationStructureBytes = 4096u;
        snapshot.Stats.Resident.ScratchBytes = 1024u;
        snapshot.Stats.Resident.CompactionSavedBytes = 512u;
        snapshot.Stats.Frame.BlasBuilds = 2u;
        snapshot.Stats.Frame.BlasRefits = 1u;
        snapshot.Stats.Frame.BlasCompactions = 4u;
        snapshot.Stats.Frame.BlasRetired = 1u;
        snapshot.Stats.Frame.InstancesSkipped = 5u;
        snapshot.Stats.LastTlasReason = RT::TlasBuildReason::TopologyChanged;

        const auto report = RayTracingStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "ready");
        EXPECT_EQ(report.at("freshness").at("model"), "previousFrame");
        // Every acceptance-criterion counter: memory, build time, compaction,
        // refit/rebuild, unsupported geometry.
        EXPECT_EQ(report.at("resident").at("accelerationStructureBytes").get<u64>(), 4096u);
        EXPECT_EQ(report.at("resident").at("scratchBytes").get<u64>(), 1024u);
        EXPECT_EQ(report.at("resident").at("compactionSavedBytes").get<u64>(), 512u);
        EXPECT_EQ(report.at("resident").at("unsupportedInstances").get<u32>(), 5u);
        EXPECT_EQ(report.at("resident").at("blasByClass").at("masked").get<u32>(), 3u);
        // Distinct values per key on purpose: "static" and "masked" carry 12 and
        // 3, so a mapping that crossed the two class slots reads wrong here
        // rather than passing on a coincidence.
        EXPECT_EQ(report.at("resident").at("blasByClass").at("static").get<u32>(), 12u);
        EXPECT_EQ(report.at("resident").at("tlasInstances").get<u32>(), 15u);
        EXPECT_EQ(report.at("frame").at("blasBuilds").get<u32>(), 2u);
        EXPECT_EQ(report.at("frame").at("blasRefits").get<u32>(), 1u);
        EXPECT_EQ(report.at("frame").at("blasCompactions").get<u32>(), 4u);
        EXPECT_EQ(report.at("frame").at("blasRetired").get<u32>(), 1u);
        EXPECT_EQ(report.at("frame").at("instancesSkipped").get<u32>(), 5u);
        EXPECT_EQ(report.at("lastTlasReason"), "TopologyChanged");
    }

    TEST(McpRayTracingStats, AnUnsupportedDeviceStillReportsTheReason)
    {
        // The single most useful thing this tool can say is WHY ray tracing is
        // unavailable, and that is exactly the case where the counters are
        // absent — so the capability block must survive a non-ready status.
        RayTracingStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = false;
        snapshot.State.HasData = false;
        snapshot.Capabilities.Supported = false;
        snapshot.Capabilities.Reason = RT::UnsupportedReason::ExtensionMissing;

        const auto report = RayTracingStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "disabled");
        EXPECT_FALSE(report.at("capability").at("supported").get<bool>());
        EXPECT_FALSE(report.at("capability").at("reason").get<std::string>().empty());
        EXPECT_NE(report.at("capability").at("reason").get<std::string>(), "supported");
        // No half-reported device properties behind an unsupported capability.
        EXPECT_FALSE(report.at("capability").contains("properties"));
        EXPECT_FALSE(report.contains("resident"));
        EXPECT_FALSE(report.contains("frame"));
    }

    TEST(McpRayTracingStats, AnEmptySceneIsNoDataNotAnAllZeroReadySample)
    {
        // An unsupported GPU and a scene with no traceable geometry produce
        // the same zeros. The status is the only thing that tells them apart,
        // and conflating them is how a stats channel starts lying.
        RayTracingStats::Snapshot snapshot = ReadySnapshot();
        snapshot.State.HasData = false;

        const auto report = RayTracingStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "noData");
        EXPECT_TRUE(report.at("capability").at("supported").get<bool>())
            << "noData must still say ray tracing IS available";
        EXPECT_FALSE(report.contains("resident"));
    }

    TEST(McpRayTracingStats, AMissingRendererIsUnavailable)
    {
        RayTracingStats::Snapshot snapshot;
        snapshot.State.Available = false;

        const auto report = RayTracingStats::BuildReport(snapshot);

        EXPECT_EQ(report.at("availability").at("status"), "unavailable");
        EXPECT_FALSE(report.contains("resident"));
        EXPECT_FALSE(report.contains("frame"));
    }
} // namespace
