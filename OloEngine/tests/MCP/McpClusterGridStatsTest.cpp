// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// Unit tests for the pure aggregation core behind olo_cluster_grid_stats
// (issue #607). The tool exists so an agent can see the clustered Forward+ light
// cull without writing a one-off GPU readback test — which means the aggregation
// itself must be trustworthy, and the two numbers that actually matter are the
// ones most easily got wrong:
//
//   * OVERFLOW clusters — a cluster at the per-cluster cap is silently DROPPING
//     lights. An off-by-one in the >= test (counting only > cap) would report
//     zero overflow on exactly the scene that is broken.
//   * The z-slice split — clusterIndex = (z * countY + y) * countX + x. Slicing
//     it wrong would attribute a hot depth band to the wrong slice, which is a
//     confidently wrong answer.
//
// The core is header-only and GL-free precisely so this runs headlessly against
// a synthetic grid buffer; the live tool is verified over the MCP attach loop.
#include "MCP/McpClusterGridStats.h"

#include <span>
#include <vector>

namespace
{
    using namespace OloEngine::MCP::ClusterGrid;

    // A flat (offset, count) light-grid buffer in LightCulling.comp's layout.
    // `counts` is indexed by clusterIndex; the offsets are irrelevant to the
    // aggregation (only counts are read) but are filled plausibly anyway.
    std::vector<u32> MakeGrid(const std::vector<u32>& counts)
    {
        std::vector<u32> pairs;
        pairs.reserve(counts.size() * 2u);
        u32 offset = 0;
        for (const u32 count : counts)
        {
            pairs.push_back(offset);
            pairs.push_back(count);
            offset += count;
        }
        return pairs;
    }

    GridDims MakeDims(u32 x, u32 y, u32 z, u32 cap)
    {
        GridDims dims;
        dims.CountX = x;
        dims.CountY = y;
        dims.CountZ = z;
        dims.MaxLightsPerCluster = cap;
        return dims;
    }

    TEST(McpClusterGridStats, EmptyGridReportsEveryClusterEmpty)
    {
        const GridDims dims = MakeDims(2, 2, 2, 8);
        const std::vector<u32> grid = MakeGrid(std::vector<u32>(8, 0u));

        const Stats stats = Compute(std::span<const u32>(grid), dims);

        EXPECT_EQ(stats.TotalClusters, 8u);
        EXPECT_EQ(stats.EmptyClusters, 8u);
        EXPECT_EQ(stats.OverflowClusters, 0u);
        EXPECT_EQ(stats.TotalAssignedIndices, 0u);
        EXPECT_EQ(stats.MaxLightsInAnyCluster, 0u);
        EXPECT_DOUBLE_EQ(stats.MeanLightsPerCluster, 0.0);
        // No non-empty clusters => the non-empty mean must be 0, not a NaN.
        EXPECT_DOUBLE_EQ(stats.MeanLightsPerNonEmptyCluster, 0.0);
    }

    TEST(McpClusterGridStats, CountsAssignedIndicesAndBothMeans)
    {
        // 4 clusters: 0, 2, 0, 6 -> 8 indices over 4 clusters (mean 2), over 2
        // non-empty clusters (mean 4). The two means differing is the whole point:
        // a mostly-empty grid with two very hot froxels averages out to "fine".
        const GridDims dims = MakeDims(2, 2, 1, 16);
        const std::vector<u32> grid = MakeGrid({ 0u, 2u, 0u, 6u });

        const Stats stats = Compute(std::span<const u32>(grid), dims);

        EXPECT_EQ(stats.TotalAssignedIndices, 8u);
        EXPECT_EQ(stats.EmptyClusters, 2u);
        EXPECT_EQ(stats.MaxLightsInAnyCluster, 6u);
        EXPECT_DOUBLE_EQ(stats.MeanLightsPerCluster, 2.0);
        EXPECT_DOUBLE_EQ(stats.MeanLightsPerNonEmptyCluster, 4.0);
        EXPECT_EQ(stats.BusiestClusterIndex, 3u);
        EXPECT_EQ(stats.BusiestClusterX, 1u);
        EXPECT_EQ(stats.BusiestClusterY, 1u);
        EXPECT_EQ(stats.BusiestClusterZ, 0u);
    }

    TEST(McpClusterGridStats, AClusterAtTheCapCountsAsOverflow)
    {
        // AT the cap means lights were dropped by the cull — the failure this tool
        // exists to surface. A `> cap` test would report zero overflow here.
        const GridDims dims = MakeDims(2, 1, 1, 4);
        const std::vector<u32> grid = MakeGrid({ 3u, 4u });

        const Stats stats = Compute(std::span<const u32>(grid), dims);

        EXPECT_EQ(stats.OverflowClusters, 1u);
        EXPECT_EQ(stats.MaxLightsInAnyCluster, 4u);

        const Json j = ToJson(stats, dims, /*globalIndexCount*/ 7, /*lightIndexCapacity*/ 64);
        ASSERT_TRUE(j.contains("warning"));
        EXPECT_NE(j.at("warning").get<std::string>().find("DROPPED"), std::string::npos);
    }

    TEST(McpClusterGridStats, SlicesFollowTheShaderClusterIndexOrder)
    {
        // clusterIndex = (z * countY + y) * countX + x, so with a 2x2 tile grid the
        // first 4 clusters are z=0 and the next 4 are z=1. Put all the lights in
        // z=1 and check they are attributed there, not smeared across both.
        const GridDims dims = MakeDims(2, 2, 2, 32);
        const std::vector<u32> grid = MakeGrid({ 0u, 0u, 0u, 0u, 5u, 5u, 5u, 5u });

        const Stats stats = Compute(std::span<const u32>(grid), dims);

        ASSERT_EQ(stats.Slices.size(), 2u);
        EXPECT_EQ(stats.Slices[0].Slice, 0u);
        EXPECT_EQ(stats.Slices[0].AssignedIndices, 0u);
        EXPECT_EQ(stats.Slices[0].EmptyClusters, 4u);
        EXPECT_EQ(stats.Slices[0].MaxLights, 0u);

        EXPECT_EQ(stats.Slices[1].Slice, 1u);
        EXPECT_EQ(stats.Slices[1].AssignedIndices, 20u);
        EXPECT_EQ(stats.Slices[1].EmptyClusters, 0u);
        EXPECT_EQ(stats.Slices[1].MaxLights, 5u);
        EXPECT_DOUBLE_EQ(stats.Slices[1].MeanLights, 5.0);

        EXPECT_EQ(stats.BusiestClusterZ, 1u);
    }

    TEST(McpClusterGridStats, HistogramBucketsEveryClusterExactlyOnce)
    {
        const GridDims dims = MakeDims(4, 2, 1, 16);
        const std::vector<u32> grid = MakeGrid({ 0u, 0u, 1u, 2u, 3u, 7u, 9u, 16u });

        const Stats stats = Compute(std::span<const u32>(grid), dims);

        u32 bucketed = 0;
        for (const Stats::Bucket& bucket : stats.Histogram)
            bucketed += bucket.Clusters;
        EXPECT_EQ(bucketed, 8u) << "every cluster must land in exactly one bucket";

        // The zero bucket is exact-zero, never "0..1" — "how much of the grid is
        // empty" is the first question asked of this histogram.
        ASSERT_FALSE(stats.Histogram.empty());
        EXPECT_EQ(stats.Histogram[0].Low, 0u);
        EXPECT_EQ(stats.Histogram[0].High, 0u);
        EXPECT_EQ(stats.Histogram[0].Clusters, 2u);

        // The top bucket must reach the cap so an at-cap cluster is representable.
        EXPECT_EQ(stats.Histogram.back().High, dims.MaxLightsPerCluster);
        EXPECT_EQ(stats.Histogram.back().Clusters, 1u);
    }

    TEST(McpClusterGridStats, AShortReadbackDegradesInsteadOfReadingOutOfBounds)
    {
        // The grid claims 8 clusters but only 3 came back. Reading the missing tail
        // would be UB; the core must simply report what it got.
        const GridDims dims = MakeDims(2, 2, 2, 8);
        const std::vector<u32> grid = MakeGrid({ 1u, 2u, 3u });

        const Stats stats = Compute(std::span<const u32>(grid), dims);

        EXPECT_EQ(stats.TotalClusters, 3u);
        EXPECT_EQ(stats.TotalAssignedIndices, 6u);
    }

    TEST(McpClusterGridStats, JsonCarriesTheGridShapeAndIndexListUtilization)
    {
        const GridDims dims = MakeDims(32, 18, 24, 128);
        const std::vector<u32> grid = MakeGrid(std::vector<u32>(dims.TotalClusters(), 1u));

        const Stats stats = Compute(std::span<const u32>(grid), dims);
        const Json j = ToJson(stats, dims, /*globalIndexCount*/ 4096, /*lightIndexCapacity*/ 8192);

        EXPECT_EQ(j.at("grid").at("countX").get<u32>(), 32u);
        EXPECT_EQ(j.at("grid").at("countZ").get<u32>(), 24u);
        EXPECT_EQ(j.at("grid").at("totalClusters").get<u32>(), 32u * 18u * 24u);
        EXPECT_EQ(j.at("grid").at("maxLightsPerCluster").get<u32>(), 128u);
        EXPECT_EQ(j.at("emptyClusters").get<u32>(), 0u);
        EXPECT_EQ(j.at("perSlice").size(), 24u);
        EXPECT_DOUBLE_EQ(j.at("lightIndexList").at("utilization").get<f64>(), 0.5);
        EXPECT_FALSE(j.contains("warning"));
    }

    TEST(McpClusterGridStats, CullingJsonExposesDepthAwareCompactionAndClampsReadback)
    {
        const Json depthAware = CullingToJson(true, 320u, 1000u, 73u, 73u, true);
        EXPECT_EQ(depthAware.at("mode").get<std::string>(), "depthAware2_5D");
        EXPECT_TRUE(depthAware.at("depthAware").get<bool>());
        EXPECT_EQ(depthAware.at("activeClusters").get<u32>(), 320u);
        EXPECT_EQ(depthAware.at("culledClusters").get<u32>(), 680u);
        EXPECT_DOUBLE_EQ(depthAware.at("activeFraction").get<f64>(), 0.32);
        EXPECT_EQ(depthAware.at("frameIndex").get<u64>(), 73u);
        EXPECT_EQ(depthAware.at("sampleAgeFrames").get<u64>(), 0u);
        EXPECT_FALSE(depthAware.at("stale").get<bool>());
        EXPECT_TRUE(depthAware.at("counterVerified").get<bool>());

        const Json fixedGrid = CullingToJson(false, 5000u, 1000u, 74u, 78u, false);
        EXPECT_EQ(fixedGrid.at("mode").get<std::string>(), "fixedGrid");
        EXPECT_EQ(fixedGrid.at("activeClusters").get<u32>(), 1000u);
        EXPECT_EQ(fixedGrid.at("culledClusters").get<u32>(), 0u);
        EXPECT_DOUBLE_EQ(fixedGrid.at("activeFraction").get<f64>(), 1.0);
        EXPECT_EQ(fixedGrid.at("sampleAgeFrames").get<u64>(), 4u);
        EXPECT_TRUE(fixedGrid.at("stale").get<bool>());
        EXPECT_FALSE(fixedGrid.at("counterVerified").get<bool>());

        const Json mismatch = CullingToJson(true, 320u, 1000u, 79u, 79u, false);
        EXPECT_FALSE(mismatch.at("counterVerified").get<bool>());

        const Json invalidOrder = CullingToJson(true, 320u, 1000u, 80u, 79u, true);
        EXPECT_EQ(invalidOrder.at("sampleAgeFrames").get<u64>(), 0u);
        EXPECT_TRUE(invalidOrder.at("stale").get<bool>());
    }

    TEST(McpClusterGridStats, ActiveClusterMetadataIndependentlyCrossChecksIndirectCount)
    {
        constexpr u32 metadataOffset = 8u;
        constexpr u32 tileCount = 3u;
        std::vector<u32> words(metadataOffset + tileCount * 4u, 0u);
        words[metadataOffset + 3u] = (1u << 0u) | (1u << 7u);
        words[metadataOffset + 4u + 3u] = 0u;
        words[metadataOffset + 8u + 3u] = (1u << 2u) | (1u << 9u) | (1u << 23u);

        const auto count = ActiveClusterCountFromMetadata(words, metadataOffset, tileCount);
        ASSERT_TRUE(count.has_value());
        EXPECT_EQ(*count, 5u);
        EXPECT_FALSE(ActiveClusterCountFromMetadata(
                         std::span<const u32>(words).first(words.size() - 1u), metadataOffset, tileCount)
                         .has_value());
    }
} // namespace
