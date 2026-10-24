// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "MCP/McpGpuSceneStats.h"

#include <array>
#include <string>

namespace
{
    using namespace OloEngine::MCP;

    [[nodiscard]] GpuSceneStats::Snapshot ReadySnapshot()
    {
        GpuSceneStats::Snapshot snapshot;
        snapshot.State.Available = true;
        snapshot.State.Enabled = true;
        snapshot.State.HasData = true;
        snapshot.State.SampleAgeFrames = 1u;
        snapshot.RenderFrameNumber = 420u;
        return snapshot;
    }

    TEST(McpGpuSceneStats, AReadySampleReportsEachKindsSlotAccounting)
    {
        GpuSceneStats::Snapshot snapshot = ReadySnapshot();
        auto& lights = snapshot.Stats.m_Lights;
        lights.m_Live = 5u;
        lights.m_SlotCount = 9u;
        lights.m_BufferCapacity = 16u;
        lights.m_FreeSlots = 2u;
        lights.m_RetiredSlots = 1u;
        lights.m_UploadBytes = 96u;
        snapshot.Stats.m_UploadBytes = 96u;
        snapshot.Stats.m_BufferGrowthEvents = 3u;

        GpuSceneStats::RecordStrides strides;
        strides.Light = 96u;
        const auto report = GpuSceneStats::BuildReport(snapshot, strides);

        EXPECT_EQ(report.at("availability").at("status"), "ready");
        EXPECT_EQ(report.at("renderFrameNumber").get<u32>(), 420u);
        const auto& kind = report.at("kinds").at("lights");
        EXPECT_EQ(kind.at("live").get<u32>(), 5u);
        EXPECT_EQ(kind.at("slots").get<u32>(), 9u);
        EXPECT_EQ(kind.at("bufferCapacity").get<u32>(), 16u);
        EXPECT_EQ(kind.at("freeSlots").get<u32>(), 2u);
        EXPECT_EQ(kind.at("retiringSlots").get<u32>(), 1u);
        // 9 slots - (5 live + 2 free + 1 retiring) = 1 tombstoned for good.
        EXPECT_EQ(kind.at("exhaustedSlots").get<u32>(), 1u);
        EXPECT_EQ(kind.at("capacityBytes").get<u64>(), 16u * 96u);
        EXPECT_EQ(kind.at("liveBytes").get<u64>(), 5u * 96u);
        EXPECT_DOUBLE_EQ(kind.at("occupancy").get<f64>(), 5.0 / 16.0);
        EXPECT_EQ(report.at("frame").at("bufferGrowthEvents").get<u32>(), 3u);
        EXPECT_TRUE(report.at("invariantViolations").empty());
    }

    TEST(McpGpuSceneStats, TheRecentWindowCountsEachEditAsOneUploadFrame)
    {
        // Idle, two light edits, idle. A single-frame sample taken after the
        // edits reads zero; the window still has them. The edits are shaped
        // like Vulkan's: each republishes the whole 4-light table, so the
        // records figure reads 8 while the frames figure reads the 2 edits.
        OloEngine::GPUSceneFrameStats idle{};
        OloEngine::GPUSceneFrameStats edit{};
        edit.m_Lights.m_UploadBytes = 4u * 80u;
        edit.m_UploadBytes = 4u * 80u;
        edit.m_BufferGrowthEvents = 1u;

        GpuSceneStats::Snapshot snapshot = ReadySnapshot();
        for (const auto& frame : std::array{ idle, edit, edit, idle })
            snapshot.Recent.Add(frame);
        GpuSceneStats::RecordStrides strides;
        strides.Light = 80u;
        const auto report = GpuSceneStats::BuildReport(snapshot, strides);

        const auto& recent = report.at("recent");
        EXPECT_EQ(recent.at("frames").get<u32>(), 4u);
        EXPECT_EQ(recent.at("framesWithUploads").get<u32>(), 2u);
        EXPECT_EQ(recent.at("bufferGrowthEvents").get<u32>(), 2u);
        EXPECT_EQ(recent.at("uploadFrames").at("lights").get<u32>(), 2u);
        EXPECT_EQ(recent.at("uploadFrames").at("instances").get<u32>(), 0u);
        EXPECT_EQ(recent.at("uploadBytes").at("lights").get<u64>(), 640u);
        EXPECT_EQ(recent.at("uploadRecords").at("lights").get<u64>(), 8u);
    }

    TEST(McpGpuSceneStats, SlotsWithNoGpuBufferAreAViolation)
    {
        // Records that can never reach the GPU must not read as healthy.
        GpuSceneStats::Snapshot snapshot = ReadySnapshot();
        snapshot.Stats.m_Lights.m_Live = 3u;
        snapshot.Stats.m_Lights.m_SlotCount = 3u;
        const auto report = GpuSceneStats::BuildReport(snapshot);
        ASSERT_EQ(report.at("invariantViolations").size(), 1u);
        EXPECT_NE(report.at("invariantViolations")[0].get<std::string>().find("lights"), std::string::npos);
    }

    TEST(McpGpuSceneStats, ATableWithNoBufferHasNullOccupancyNotZero)
    {
        // "No table yet" and "an empty table" are different states.
        const auto report = GpuSceneStats::BuildReport(ReadySnapshot());
        EXPECT_TRUE(report.at("kinds").at("environments").at("occupancy").is_null());
    }

    TEST(McpGpuSceneStats, DrawLinksAndConsumptionComeThroughUnchanged)
    {
        GpuSceneStats::Snapshot snapshot = ReadySnapshot();
        // Distinct values so a crossed mapping fails.
        snapshot.Draws = { .Linked = 11u, .Unlinked = 2u, .Consumed = 7u, .FellBack = 4u };
        const auto report = GpuSceneStats::BuildReport(snapshot);
        EXPECT_EQ(report.at("draws").at("linked").get<u32>(), 11u);
        EXPECT_EQ(report.at("draws").at("unlinked").get<u32>(), 2u);
        EXPECT_EQ(report.at("draws").at("consumed").get<u32>(), 7u);
        EXPECT_EQ(report.at("draws").at("fellBack").get<u32>(), 4u);
    }

    TEST(McpGpuSceneStats, AccountingThatDoesNotAddUpIsListedAsAViolation)
    {
        GpuSceneStats::Snapshot snapshot = ReadySnapshot();
        auto& instances = snapshot.Stats.m_Instances;
        instances.m_Live = 8u;
        instances.m_FreeSlots = 3u;
        instances.m_SlotCount = 10u; // 8 + 3 > 10
        instances.m_BufferCapacity = 16u;
        instances.m_UploadBytes = 64u;
        snapshot.Stats.m_UploadBytes = 128u; // not the per-kind sum
        snapshot.Stats.m_Animated.m_CanonicalInstances = 4u;
        snapshot.Stats.m_Animated.m_SurfacesWithHistory = 1u;

        const auto report = GpuSceneStats::BuildReport(snapshot);
        const auto& violations = report.at("invariantViolations");
        ASSERT_EQ(violations.size(), 3u);
        EXPECT_NE(violations[0].get<std::string>().find("instances"), std::string::npos);
        EXPECT_NE(violations[1].get<std::string>().find("uploadBytes"), std::string::npos);
        EXPECT_NE(violations[2].get<std::string>().find("animated"), std::string::npos);
        // The impossible partition reports 0 exhausted rather than wrapping.
        EXPECT_EQ(report.at("kinds").at("instances").at("exhaustedSlots").get<u64>(), 0u);
    }

    TEST(McpGpuSceneStats, NoCompletedFrameIsNoDataWithoutCounters)
    {
        GpuSceneStats::Snapshot snapshot = ReadySnapshot();
        snapshot.State.HasData = false;
        const auto report = GpuSceneStats::BuildReport(snapshot);
        EXPECT_EQ(report.at("availability").at("status"), "noData");
        EXPECT_FALSE(report.contains("kinds"));
    }

    TEST(McpGpuSceneStats, NoRendererIsUnavailable)
    {
        const auto report = GpuSceneStats::BuildReport(GpuSceneStats::Snapshot{});
        EXPECT_EQ(report.at("availability").at("status"), "unavailable");
        EXPECT_FALSE(report.contains("kinds"));
    }

    TEST(McpGpuSceneStats, EveryNotStagedCategoryIsReported)
    {
        GpuSceneStats::Snapshot snapshot = ReadySnapshot();
        snapshot.Stats.m_UnsupportedCounts[static_cast<sizet>(OloEngine::GPUSceneUnsupportedCategory::Terrain)] = 6u;
        snapshot.Stats.m_UnsupportedTotal = 6u;
        const auto report = GpuSceneStats::BuildReport(snapshot);
        EXPECT_EQ(report.at("notStagedByCategory").size(), OloEngine::GPUSceneUnsupportedCategoryCount);
        EXPECT_EQ(report.at("notStagedByCategory").at("terrain").get<u32>(), 6u);
        EXPECT_EQ(report.at("notStagedTotal").get<u32>(), 6u);
    }
} // namespace
