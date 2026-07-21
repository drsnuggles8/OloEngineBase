#pragma once

// Pure stats math + JSON shaping for the olo_render_target_stats MCP tool
// (issue #607): exact float min/max/mean + a unique-value histogram over a
// rect of one render-graph target.
//
// The motivating failure: 8-bit PNG captures hide 1-ULP corruption — 1.0 and
// 0.99999994 both encode as 255 — so mapping a corrupt HZB region during the
// GTAO hunt took hundreds of single-pixel probe round-trips. One stats call
// answers "is this region EXACTLY 1.0f, and if not, what distinct values does
// it hold and how often" bit-exactly.
//
// The handler in McpToolsRender.cpp does the GL-bound work on the main thread
// (resolve the target, glGetTextureSubImage the rect at the requested mip) and
// hands the decoded channel-interleaved values here. Everything below is free
// functions over PODs with NO GL / renderer / editor dependency, so the math
// (bit-exact uniqueness, finite-only min/max/mean, truncation behavior) is
// unit-tested headlessly — the same split McpRenderProbePixel.h uses.
//
// Only Core/Base.h (the u32/f32/... typedefs) and nlohmann::json are pulled
// in; everything else is the standard library.

#include "OloEngine/Core/Base.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine::MCP::RenderTargetStats
{
    using Json = nlohmann::json;

    // Distinct-bit-pattern cap: past this many unique values the histogram
    // stops admitting NEW keys (already-tracked keys keep counting) and the
    // result is flagged truncated. 64k distinct floats is far beyond any
    // "is this buffer quantized / corrupted" question; an untruncated answer
    // for a genuinely continuous buffer would just be noise.
    inline constexpr u32 kMaxUniqueValues = 65536;

    // How many of the most frequent distinct values the reply lists.
    inline constexpr u32 kTopValueCount = 16;

    struct ValueCount
    {
        f32 Value = 0.0f; // bit-exact representative (bit_cast of the key)
        u32 Count = 0;
    };

    struct ChannelStats
    {
        u32 FiniteCount = 0;
        u32 NaNCount = 0;
        u32 InfCount = 0;
        // Finite-only extrema/mean. Meaningless when FiniteCount == 0.
        f64 Min = 0.0;
        f64 Max = 0.0;
        f64 Mean = 0.0;
        u32 UniqueValueCount = 0; // distinct bit patterns seen (incl. non-finite)
        bool UniqueTruncated = false;
        std::vector<ValueCount> TopValues; // most frequent first; ties by value
    };

    // Bit-exact stats over one channel's values. NaNs with different payloads
    // count as distinct bit patterns but are all excluded from min/max/mean.
    [[nodiscard]] inline ChannelStats ComputeChannelStats(const std::vector<f32>& values)
    {
        ChannelStats stats;
        std::unordered_map<u32, u32> counts;
        counts.reserve(std::min<sizet>(values.size(), kMaxUniqueValues));

        f64 sum = 0.0;
        for (const f32 value : values)
        {
            const u32 bits = std::bit_cast<u32>(value);
            if (const auto it = counts.find(bits); it != counts.end())
                ++it->second;
            else if (counts.size() < kMaxUniqueValues)
                counts.emplace(bits, 1u);
            else
                stats.UniqueTruncated = true;

            if (std::isnan(value))
            {
                ++stats.NaNCount;
                continue;
            }
            if (std::isinf(value))
            {
                ++stats.InfCount;
                continue;
            }
            ++stats.FiniteCount;
            const f64 v = static_cast<f64>(value);
            if (stats.FiniteCount == 1u)
            {
                stats.Min = v;
                stats.Max = v;
            }
            else
            {
                stats.Min = std::min(stats.Min, v);
                stats.Max = std::max(stats.Max, v);
            }
            sum += v;
        }
        if (stats.FiniteCount > 0)
            stats.Mean = sum / static_cast<f64>(stats.FiniteCount);

        stats.UniqueValueCount = static_cast<u32>(counts.size());

        std::vector<ValueCount> top;
        top.reserve(counts.size());
        for (const auto& [bits, count] : counts)
            top.push_back(ValueCount{ std::bit_cast<f32>(bits), count });
        std::sort(top.begin(), top.end(),
                  [](const ValueCount& a, const ValueCount& b)
                  {
                      if (a.Count != b.Count)
                          return a.Count > b.Count;
                      // Deterministic tie-break on the bit pattern (never
                      // compare NaN-containing floats with operator<).
                      return std::bit_cast<u32>(a.Value) < std::bit_cast<u32>(b.Value);
                  });
        if (top.size() > kTopValueCount)
            top.resize(kTopValueCount);
        stats.TopValues = std::move(top);
        return stats;
    }

    [[nodiscard]] inline Json ChannelStatsJson(const ChannelStats& stats, std::string_view channelName)
    {
        Json j;
        j["channel"] = std::string(channelName);
        j["finiteCount"] = stats.FiniteCount;
        if (stats.NaNCount > 0)
            j["nanCount"] = stats.NaNCount;
        if (stats.InfCount > 0)
            j["infCount"] = stats.InfCount;
        if (stats.FiniteCount > 0)
        {
            j["min"] = stats.Min;
            j["max"] = stats.Max;
            j["mean"] = stats.Mean;
        }
        j["uniqueValues"] = stats.UniqueValueCount;
        if (stats.UniqueTruncated)
        {
            j["uniqueTruncated"] = true;
            j["uniqueNote"] = "More than " + std::to_string(kMaxUniqueValues) +
                              " distinct bit patterns; uniqueValues is a lower bound and topValues only "
                              "covers the patterns seen before the cap.";
        }
        Json top = Json::array();
        for (const auto& vc : stats.TopValues)
        {
            Json entry;
            // NaN/Inf are not representable in JSON numbers — encode the
            // exact bit pattern instead, plus a readable token.
            if (std::isfinite(vc.Value))
                entry["value"] = vc.Value;
            else
                entry["value"] = std::isnan(vc.Value) ? "NaN" : (vc.Value > 0.0f ? "+Inf" : "-Inf");
            entry["bits"] = std::bit_cast<u32>(vc.Value);
            entry["count"] = vc.Count;
            top.push_back(std::move(entry));
        }
        j["topValues"] = std::move(top);
        return j;
    }

    // Channel letter for interleaved data ("r", "g", "b", "a").
    [[nodiscard]] inline const char* ChannelName(const i32 channel) noexcept
    {
        switch (channel)
        {
            case 0:
                return "r";
            case 1:
                return "g";
            case 2:
                return "b";
            case 3:
                return "a";
            default:
                return "?";
        }
    }

    // De-interleave channel `channel` out of `interleaved` (texel-major,
    // `channels` values per texel) — the shape glGetTextureSubImage returns.
    [[nodiscard]] inline std::vector<f32> ExtractChannel(const std::vector<f32>& interleaved,
                                                         const i32 channels, const i32 channel)
    {
        std::vector<f32> values;
        if (channels <= 0 || channel < 0 || channel >= channels)
            return values;
        values.reserve(interleaved.size() / static_cast<sizet>(channels));
        for (sizet i = static_cast<sizet>(channel); i < interleaved.size(); i += static_cast<sizet>(channels))
            values.push_back(interleaved[i]);
        return values;
    }

    // The whole reply body (the handler appends "meta" itself).
    [[nodiscard]] inline Json BuildStatsJson(const std::string& name, const std::string& format,
                                             u32 rectX, u32 rectY, u32 rectW, u32 rectH,
                                             u32 mip, u32 mipWidth, u32 mipHeight, u32 layer,
                                             const std::vector<f32>& interleaved, const i32 channels)
    {
        Json j;
        j["name"] = name;
        j["format"] = format;
        j["mip"] = mip;
        j["mipWidth"] = mipWidth;
        j["mipHeight"] = mipHeight;
        if (layer != 0)
            j["layer"] = layer;
        j["rect"] = Json{ { "x", rectX }, { "y", rectY }, { "w", rectW }, { "h", rectH } };
        j["origin"] = "top-left (rect addresses texels of the mip, same orientation as a capture PNG)";
        j["texelCount"] = static_cast<u64>(rectW) * static_cast<u64>(rectH);
        Json channelStats = Json::array();
        for (i32 c = 0; c < channels; ++c)
            channelStats.push_back(ChannelStatsJson(ComputeChannelStats(ExtractChannel(interleaved, channels, c)),
                                                    ChannelName(c)));
        j["channels"] = std::move(channelStats);
        return j;
    }
} // namespace OloEngine::MCP::RenderTargetStats
