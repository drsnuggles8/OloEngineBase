#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit

// Unit tests for the pure stats math behind olo_render_target_stats (issue
// #607): bit-exact unique-value histograms, finite-only min/max/mean, NaN/Inf
// accounting, channel de-interleave, and the reply JSON shape. The GL-bound
// rect readback lives in McpToolsRender.cpp's handler (deliberately NOT
// compiled here) — this pins the math an 8-bit PNG capture cannot express
// (1.0f vs 0.99999994f both quantize to 255).
#include "MCP/McpRenderTargetStats.h"

#include <bit>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::MCP::RenderTargetStats;
} // namespace

TEST(McpRenderTargetStats, ExactMinMaxMeanOverFiniteValues)
{
    const std::vector<f32> values{ 1.0f, 2.0f, 3.0f, 4.0f };
    const ChannelStats stats = ComputeChannelStats(values);

    EXPECT_EQ(4u, stats.FiniteCount);
    EXPECT_EQ(0u, stats.NaNCount);
    EXPECT_EQ(0u, stats.InfCount);
    EXPECT_DOUBLE_EQ(1.0, stats.Min);
    EXPECT_DOUBLE_EQ(4.0, stats.Max);
    EXPECT_DOUBLE_EQ(2.5, stats.Mean);
    EXPECT_EQ(4u, stats.UniqueValueCount);
    EXPECT_FALSE(stats.UniqueTruncated);
}

TEST(McpRenderTargetStats, OneUlpNeighboursAreDistinct)
{
    // The motivating case: 1.0f and its 1-ULP neighbour both encode to PNG
    // byte 255 — the histogram must keep them apart bit-exactly.
    const f32 one = 1.0f;
    const f32 nearlyOne = std::nextafter(1.0f, 0.0f); // 0.99999994f
    const std::vector<f32> values{ one, one, nearlyOne };
    const ChannelStats stats = ComputeChannelStats(values);

    EXPECT_EQ(2u, stats.UniqueValueCount);
    ASSERT_EQ(2u, stats.TopValues.size());
    // Most frequent first: 1.0f twice, the neighbour once.
    EXPECT_EQ(std::bit_cast<u32>(one), std::bit_cast<u32>(stats.TopValues[0].Value));
    EXPECT_EQ(2u, stats.TopValues[0].Count);
    EXPECT_EQ(std::bit_cast<u32>(nearlyOne), std::bit_cast<u32>(stats.TopValues[1].Value));
    EXPECT_EQ(1u, stats.TopValues[1].Count);
}

TEST(McpRenderTargetStats, NaNAndInfExcludedFromAggregatesButCounted)
{
    const f32 quietNaN = std::numeric_limits<f32>::quiet_NaN();
    const f32 positiveInf = std::numeric_limits<f32>::infinity();
    const std::vector<f32> values{ 0.5f, quietNaN, positiveInf, -positiveInf, 0.5f };
    const ChannelStats stats = ComputeChannelStats(values);

    EXPECT_EQ(2u, stats.FiniteCount);
    EXPECT_EQ(1u, stats.NaNCount);
    EXPECT_EQ(2u, stats.InfCount);
    EXPECT_DOUBLE_EQ(0.5, stats.Min);
    EXPECT_DOUBLE_EQ(0.5, stats.Max);
    EXPECT_DOUBLE_EQ(0.5, stats.Mean);
    // 0.5, NaN, +Inf, -Inf = four distinct bit patterns.
    EXPECT_EQ(4u, stats.UniqueValueCount);
}

TEST(McpRenderTargetStats, TrackedKeysKeepCountingPastTheCapWithoutTruncation)
{
    // Fill the map to exactly kMaxUniqueValues distinct values, then append
    // repeats of an ALREADY-TRACKED value: those must keep incrementing their
    // count and must NOT flag truncation (only a rejected NEW key does).
    std::vector<f32> values;
    values.reserve(kMaxUniqueValues + 4u);
    for (u32 i = 0; i < kMaxUniqueValues; ++i)
        values.push_back(static_cast<f32>(i));
    for (u32 i = 0; i < 4u; ++i)
        values.push_back(0.0f); // already tracked (i == 0)
    const ChannelStats stats = ComputeChannelStats(values);

    EXPECT_FALSE(stats.UniqueTruncated);
    EXPECT_EQ(kMaxUniqueValues, stats.UniqueValueCount);
    ASSERT_FALSE(stats.TopValues.empty());
    // 0.0f was seen 1 (pre-cap) + 4 (post-cap) times — the most frequent value.
    EXPECT_EQ(std::bit_cast<u32>(0.0f), std::bit_cast<u32>(stats.TopValues[0].Value));
    EXPECT_EQ(5u, stats.TopValues[0].Count);
}

TEST(McpRenderTargetStats, NewKeyPastTheCapFlagsTruncation)
{
    // Fill the map to exactly kMaxUniqueValues distinct values, then append a
    // NEW distinct value: its key is rejected (truncation flagged, unique
    // count stays at the cap) but it still participates in min/max/mean.
    std::vector<f32> values;
    values.reserve(kMaxUniqueValues + 4u);
    for (u32 i = 0; i < kMaxUniqueValues; ++i)
        values.push_back(static_cast<f32>(i));
    for (u32 i = 0; i < 4u; ++i)
        values.push_back(1.0e30f); // a NEW distinct value past the cap
    const ChannelStats stats = ComputeChannelStats(values);

    EXPECT_TRUE(stats.UniqueTruncated);
    EXPECT_EQ(kMaxUniqueValues, stats.UniqueValueCount);
    EXPECT_DOUBLE_EQ(static_cast<f64>(1.0e30f), stats.Max);
}

TEST(McpRenderTargetStats, ExtractChannelDeinterleaves)
{
    // Two RG texels: (1,2), (3,4).
    const std::vector<f32> interleaved{ 1.0f, 2.0f, 3.0f, 4.0f };
    const std::vector<f32> r = ExtractChannel(interleaved, 2, 0);
    const std::vector<f32> g = ExtractChannel(interleaved, 2, 1);
    ASSERT_EQ(2u, r.size());
    EXPECT_FLOAT_EQ(1.0f, r[0]);
    EXPECT_FLOAT_EQ(3.0f, r[1]);
    ASSERT_EQ(2u, g.size());
    EXPECT_FLOAT_EQ(2.0f, g[0]);
    EXPECT_FLOAT_EQ(4.0f, g[1]);
    EXPECT_TRUE(ExtractChannel(interleaved, 2, 2).empty()); // out-of-range channel
}

TEST(McpRenderTargetStats, BuildStatsJsonShape)
{
    // A 2x1 R32F rect holding {1.0, 0.5}.
    const std::vector<f32> interleaved{ 1.0f, 0.5f };
    const Json j = BuildStatsJson("HZB", "R32F", 4, 8, 2, 1, 3, 256, 128, 0, interleaved, 1);

    EXPECT_EQ("HZB", j["name"].get<std::string>());
    EXPECT_EQ("R32F", j["format"].get<std::string>());
    EXPECT_EQ(3u, j["mip"].get<u32>());
    EXPECT_EQ(256u, j["mipWidth"].get<u32>());
    EXPECT_EQ(128u, j["mipHeight"].get<u32>());
    EXPECT_EQ(4u, j["rect"]["x"].get<u32>());
    EXPECT_EQ(8u, j["rect"]["y"].get<u32>());
    EXPECT_EQ(2u, j["rect"]["w"].get<u32>());
    EXPECT_EQ(1u, j["rect"]["h"].get<u32>());
    EXPECT_EQ(2u, j["texelCount"].get<u64>());
    EXPECT_FALSE(j.contains("layer")); // layer 0 is omitted
    ASSERT_EQ(1u, j["channels"].size());
    const Json& channel = j["channels"][0];
    EXPECT_EQ("r", channel["channel"].get<std::string>());
    EXPECT_EQ(2u, channel["finiteCount"].get<u32>());
    EXPECT_DOUBLE_EQ(0.5, channel["min"].get<f64>());
    EXPECT_DOUBLE_EQ(1.0, channel["max"].get<f64>());
    EXPECT_DOUBLE_EQ(0.75, channel["mean"].get<f64>());
    EXPECT_EQ(2u, channel["uniqueValues"].get<u32>());
}

TEST(McpRenderTargetStats, NonFiniteTopValuesEncodeAsTokensWithBits)
{
    const f32 quietNaN = std::numeric_limits<f32>::quiet_NaN();
    const std::vector<f32> interleaved{ quietNaN, quietNaN, 1.0f };
    const Json j = BuildStatsJson("Weird", "R32F", 0, 0, 3, 1, 0, 3, 1, 0, interleaved, 1);

    const Json& top = j["channels"][0]["topValues"];
    ASSERT_EQ(2u, top.size());
    // NaN twice is the most frequent; it must encode as a token + exact bits,
    // never as a JSON number (which cannot hold NaN).
    EXPECT_TRUE(top[0]["value"].is_string());
    EXPECT_EQ("NaN", top[0]["value"].get<std::string>());
    EXPECT_EQ(std::bit_cast<u32>(quietNaN), top[0]["bits"].get<u32>());
    EXPECT_EQ(2u, top[0]["count"].get<u32>());
    EXPECT_TRUE(top[1]["value"].is_number());
}
