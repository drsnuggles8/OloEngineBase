// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Pure JSON shaping behind olo_streaming_stats (issue #1365): an unknown size is
// reported as unknown (bytes: null, unknownCount > 0, complete: false), never as
// zero, and a disabled budget is null rather than a 0 a reader could compare with.

#include "MCP/McpStreamingStats.h"

using namespace OloEngine;      // NOLINT(google-build-using-namespace)
using namespace OloEngine::MCP; // NOLINT(google-build-using-namespace)

TEST(McpStreamingStats, AnUnknownSizeIsNullNotZero)
{
    const auto unknown = StreamingStats::ByteSizeToJson(FAssetByteSize::Unknown());
    EXPECT_EQ(unknown["source"], "unknown");
    EXPECT_TRUE(unknown["bytes"].is_null());

    const auto estimate = StreamingStats::ByteSizeToJson(FAssetByteSize::Estimate(0));
    EXPECT_EQ(estimate["source"], "estimate");
    EXPECT_EQ(estimate["bytes"], 0) << "a known zero stays a number";

    EXPECT_EQ(StreamingStats::ByteSizeToJson(FAssetByteSize::Actual(42))["source"], "actual");
}

TEST(McpStreamingStats, SceneStreamerBlockCarriesUnknownsBudgetsAndReasons)
{
    FSceneStreamingStats stats;
    stats.LoadedRegions = 2;
    stats.ResidentBytes.Add(FAssetByteSize::Estimate(1000));
    stats.ResidentBytes.Add(FAssetByteSize::Unknown());
    stats.MaxResidentBytes = 0;
    stats.MaxAdmittedBytesPerFrame = 4096;
    stats.DeferredRegions = 1;
    stats.DeferredRequests = 7;
    stats.AbandonedInFlight = 3;
    stats.EvictedForBytes = 2;

    std::vector<StreamingStats::RegionRow> rows;
    rows.push_back({ 99, "north", "Unloaded", FAssetByteSize::Estimate(512), EStreamingAdmissionStatus::Deferred,
                     EStreamingAdmissionReason::ResidentBudget });

    const auto j = StreamingStats::SceneStreamerToJson(stats, rows);
    EXPECT_TRUE(j["available"]);
    EXPECT_EQ(j["residentBytes"]["knownBytes"], 1000);
    EXPECT_EQ(j["residentBytes"]["unknownCount"], 1);
    EXPECT_FALSE(j["residentBytes"]["complete"]);
    EXPECT_TRUE(j["maxResidentBytes"].is_null()) << "0 means no budget";
    EXPECT_EQ(j["maxAdmittedBytesPerFrame"], 4096);
    EXPECT_EQ(j["admission"]["deferredRegions"], 1);
    EXPECT_EQ(j["admission"]["deferredRequests"], 7);
    EXPECT_EQ(j["cancellation"]["abandonedInFlight"], 3);
    EXPECT_EQ(j["eviction"]["forBytes"], 2);

    ASSERT_EQ(j["regions"].size(), 1u);
    EXPECT_EQ(j["regions"][0]["id"], "99");
    EXPECT_EQ(j["regions"][0]["admissionStatus"], "Deferred");
    EXPECT_EQ(j["regions"][0]["admissionReason"], "ResidentBudget");
    EXPECT_EQ(j["regions"][0]["estimatedSize"]["bytes"], 512);
}

TEST(McpStreamingStats, RuntimeAssetsBlockAndUnavailable)
{
    FRuntimeAssetStreamingReport report;
    report.Loads.PendingCount = 1;
    report.Loads.PendingBytes.Add(FAssetByteSize::Unknown());
    report.Loads.CancelledBeforeStart = 4;
    report.Resident.Add(FAssetByteSize::Actual(64));

    const auto j = StreamingStats::RuntimeAssetsToJson(report);
    EXPECT_TRUE(j["available"]);
    EXPECT_EQ(j["pendingBytes"]["unknownCount"], 1);
    EXPECT_EQ(j["residentBytes"]["actualBytes"], 64);
    EXPECT_EQ(j["cancellation"]["cancelledBeforeStart"], 4);

    const auto off = StreamingStats::Unavailable("why");
    EXPECT_FALSE(off["available"]);
    EXPECT_EQ(off["reason"], "why");
}
