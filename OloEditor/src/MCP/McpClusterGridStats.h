#pragma once

// Pure, engine-light histogram core for the olo_cluster_grid_stats MCP tool
// (issue #607): summarise the clustered Forward+ froxel light grid.
//
// Verifying the clustered cull today means writing a one-off readback test —
// the light grid is a GPU SSBO nobody can look at from outside the renderer.
// This turns it into a question an agent can just ask: how many lights landed
// in each froxel, which z-slices are hot, how many clusters are empty, and how
// many are AT the per-cluster cap (i.e. silently dropping lights).
//
// The handler in McpToolsRender.cpp does the GL-bound work on the main thread
// (stage the DYNAMIC_COPY grid SSBO through a DYNAMIC_READ buffer and read the
// (offset, count) pairs back); the aggregation below is a free function over a
// plain u32 span with NO GL / renderer dependency, so it is unit-tested
// headlessly — the same split McpGoldenCompare.h / McpRenderProbePixel.h use.
//
// Cluster indexing mirrors LightCulling.comp exactly:
//     clusterIndex = (z * countY + y) * countX + x
// so slicing the flat array by z is a contiguous stride of countX * countY.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::MCP::ClusterGrid
{
    using Json = nlohmann::json;

    struct GridDims
    {
        u32 CountX = 0;
        u32 CountY = 0;
        u32 CountZ = 0;
        u32 MaxLightsPerCluster = 0;

        [[nodiscard]] u32 TotalClusters() const
        {
            return CountX * CountY * CountZ;
        }
    };

    // Aggregate over one z-slice (a depth band of the view frustum).
    struct SliceStats
    {
        u32 Slice = 0;
        u32 Clusters = 0;
        u32 AssignedIndices = 0; // sum of per-cluster light counts in this slice
        u32 EmptyClusters = 0;
        u32 OverflowClusters = 0; // count == MaxLightsPerCluster (lights may have been dropped)
        u32 MaxLights = 0;
        f64 MeanLights = 0.0;
    };

    struct Stats
    {
        u32 TotalClusters = 0;
        u32 EmptyClusters = 0;
        u32 OverflowClusters = 0;
        u32 MaxLightsInAnyCluster = 0;
        u32 TotalAssignedIndices = 0;
        f64 MeanLightsPerCluster = 0.0;         // over ALL clusters
        f64 MeanLightsPerNonEmptyCluster = 0.0; // over the clusters that got at least one light
        u32 BusiestClusterIndex = 0;
        u32 BusiestClusterX = 0;
        u32 BusiestClusterY = 0;
        u32 BusiestClusterZ = 0;
        std::vector<SliceStats> Slices;
        // Count-bucket histogram over every cluster. Bucket i covers the light
        // counts in [Bucket[i].Low, Bucket[i].High].
        struct Bucket
        {
            u32 Low = 0;
            u32 High = 0;
            u32 Clusters = 0;
        };
        std::vector<Bucket> Histogram;
    };

    // The bucket edges of the count histogram: exact 0, exact 1, then doubling
    // bands up to the cap. Chosen so "mostly empty", "a handful", and "at the
    // cap" are all visible at a glance instead of being averaged away.
    [[nodiscard]] inline std::vector<Stats::Bucket> MakeBuckets(u32 maxLightsPerCluster)
    {
        std::vector<Stats::Bucket> buckets;
        buckets.push_back({ 0u, 0u, 0u });
        u32 low = 1u;
        while (low <= maxLightsPerCluster)
        {
            const u32 high = std::min(low == 1u ? 1u : (low * 2u - 1u), maxLightsPerCluster);
            buckets.push_back({ low, high, 0u });
            if (high >= maxLightsPerCluster)
                break;
            low = high + 1u;
        }
        return buckets;
    }

    // `gridPairs` is the raw light-grid SSBO contents: two u32 per cluster
    // (offset, count), cluster-major in LightCulling.comp's index order. A short
    // buffer is tolerated (the missing tail is simply not counted) so a partial
    // readback degrades instead of reading out of bounds.
    [[nodiscard]] inline Stats Compute(std::span<const u32> gridPairs, const GridDims& dims)
    {
        Stats stats;
        stats.Histogram = MakeBuckets(dims.MaxLightsPerCluster);

        const u32 clustersPerSlice = dims.CountX * dims.CountY;
        const auto availableClusters = static_cast<u32>(std::min<std::size_t>(
            gridPairs.size() / 2u, static_cast<std::size_t>(dims.TotalClusters())));
        stats.TotalClusters = availableClusters;
        if (availableClusters == 0 || clustersPerSlice == 0)
            return stats;

        u64 nonEmptyClusters = 0;
        stats.Slices.reserve(dims.CountZ);
        for (u32 z = 0; z < dims.CountZ; ++z)
        {
            SliceStats slice;
            slice.Slice = z;
            for (u32 local = 0; local < clustersPerSlice; ++local)
            {
                const u32 clusterIndex = z * clustersPerSlice + local;
                if (clusterIndex >= availableClusters)
                    break;

                const u32 count = gridPairs[static_cast<std::size_t>(clusterIndex) * 2u + 1u];
                ++slice.Clusters;
                slice.AssignedIndices += count;
                slice.MaxLights = std::max(slice.MaxLights, count);
                if (count == 0)
                {
                    ++slice.EmptyClusters;
                }
                else
                {
                    ++nonEmptyClusters;
                }
                if (dims.MaxLightsPerCluster > 0 && count >= dims.MaxLightsPerCluster)
                    ++slice.OverflowClusters;

                stats.TotalAssignedIndices += count;
                if (count > stats.MaxLightsInAnyCluster)
                {
                    stats.MaxLightsInAnyCluster = count;
                    stats.BusiestClusterIndex = clusterIndex;
                    stats.BusiestClusterX = local % dims.CountX;
                    stats.BusiestClusterY = local / dims.CountX;
                    stats.BusiestClusterZ = z;
                }

                for (auto& bucket : stats.Histogram)
                {
                    if (count >= bucket.Low && count <= bucket.High)
                    {
                        ++bucket.Clusters;
                        break;
                    }
                }
            }

            if (slice.Clusters == 0)
                continue;
            slice.MeanLights = static_cast<f64>(slice.AssignedIndices) / static_cast<f64>(slice.Clusters);
            stats.EmptyClusters += slice.EmptyClusters;
            stats.OverflowClusters += slice.OverflowClusters;
            stats.Slices.push_back(slice);
        }

        stats.MeanLightsPerCluster =
            static_cast<f64>(stats.TotalAssignedIndices) / static_cast<f64>(availableClusters);
        stats.MeanLightsPerNonEmptyCluster =
            nonEmptyClusters > 0 ? static_cast<f64>(stats.TotalAssignedIndices) / static_cast<f64>(nonEmptyClusters)
                                 : 0.0;
        return stats;
    }

    // `globalIndexCount` is the light-index list's atomic append counter (how
    // many index slots the cull actually consumed this frame) and
    // `lightIndexCapacity` its allocated slot count — together they say whether
    // the index list itself is close to overflowing, which the per-cluster
    // counts alone cannot show.
    [[nodiscard]] inline Json ToJson(const Stats& stats, const GridDims& dims,
                                     u32 globalIndexCount, u32 lightIndexCapacity)
    {
        Json grid;
        grid["countX"] = dims.CountX;
        grid["countY"] = dims.CountY;
        grid["countZ"] = dims.CountZ;
        grid["totalClusters"] = dims.TotalClusters();
        grid["maxLightsPerCluster"] = dims.MaxLightsPerCluster;

        Json slices = Json::array();
        for (const SliceStats& slice : stats.Slices)
        {
            slices.push_back(Json{ { "slice", slice.Slice },
                                   { "clusters", slice.Clusters },
                                   { "assignedIndices", slice.AssignedIndices },
                                   { "emptyClusters", slice.EmptyClusters },
                                   { "overflowClusters", slice.OverflowClusters },
                                   { "maxLights", slice.MaxLights },
                                   { "meanLights", slice.MeanLights } });
        }

        Json histogram = Json::array();
        for (const Stats::Bucket& bucket : stats.Histogram)
        {
            histogram.push_back(Json{ { "low", bucket.Low },
                                      { "high", bucket.High },
                                      { "clusters", bucket.Clusters } });
        }

        Json j;
        j["grid"] = std::move(grid);
        j["clustersSampled"] = stats.TotalClusters;
        j["totalAssignedIndices"] = stats.TotalAssignedIndices;
        j["emptyClusters"] = stats.EmptyClusters;
        j["overflowClusters"] = stats.OverflowClusters;
        j["maxLightsInAnyCluster"] = stats.MaxLightsInAnyCluster;
        j["meanLightsPerCluster"] = stats.MeanLightsPerCluster;
        j["meanLightsPerNonEmptyCluster"] = stats.MeanLightsPerNonEmptyCluster;
        j["busiestCluster"] = Json{ { "index", stats.BusiestClusterIndex },
                                    { "x", stats.BusiestClusterX },
                                    { "y", stats.BusiestClusterY },
                                    { "z", stats.BusiestClusterZ },
                                    { "lights", stats.MaxLightsInAnyCluster } };
        j["perSlice"] = std::move(slices);
        j["histogram"] = std::move(histogram);
        j["lightIndexList"] = Json{ { "usedSlots", globalIndexCount },
                                    { "capacity", lightIndexCapacity },
                                    { "utilization", lightIndexCapacity > 0
                                                         ? static_cast<f64>(globalIndexCount) / static_cast<f64>(lightIndexCapacity)
                                                         : 0.0 } };
        if (stats.OverflowClusters > 0)
            j["warning"] = "One or more clusters hit the per-cluster light cap (" +
                           std::to_string(dims.MaxLightsPerCluster) +
                           "); lights beyond the cap are DROPPED by the cull and will not light those froxels.";
        return j;
    }
} // namespace OloEngine::MCP::ClusterGrid
