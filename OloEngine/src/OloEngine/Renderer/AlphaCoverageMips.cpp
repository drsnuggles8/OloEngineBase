#include "OloEnginePCH.h"
#include "OloEngine/Renderer/AlphaCoverageMips.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace OloEngine::AlphaCoverageMips
{
    namespace
    {
        // The shader's test, per UNORM byte: a texel passes iff it is NOT
        // `alpha < cutoff`.
        [[nodiscard]] bool Passes(u32 alphaByte, f32 cutoff) noexcept
        {
            return !(static_cast<f32>(alphaByte) / 255.0f < cutoff);
        }

        using AlphaHistogram = std::array<u64, 256>;

        [[nodiscard]] AlphaHistogram HistogramOf(std::span<const u8> rgba) noexcept
        {
            AlphaHistogram histogram{};
            for (sizet i = 3; i < rgba.size(); i += 4)
                ++histogram[rgba[i]];
            return histogram;
        }

        [[nodiscard]] const std::array<f32, 256>& SrgbToLinearTable() noexcept
        {
            static const std::array<f32, 256> s_Table = []
            {
                std::array<f32, 256> table{};
                for (u32 i = 0; i < 256u; ++i)
                {
                    const f32 c = static_cast<f32>(i) / 255.0f;
                    table[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                }
                return table;
            }();
            return s_Table;
        }

        [[nodiscard]] u8 LinearToSrgbByte(f32 linear) noexcept
        {
            const f32 c = std::clamp(linear, 0.0f, 1.0f);
            const f32 encoded = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            return static_cast<u8>(std::clamp(std::round(encoded * 255.0f), 0.0f, 255.0f));
        }

        [[nodiscard]] u8 UnormByte(f32 value) noexcept
        {
            return static_cast<u8>(std::clamp(std::round(value * 255.0f), 0.0f, 255.0f));
        }

        // One destination texel's footprint along one axis: the source texels
        // it overlaps and by how much. A 2:1 axis gives two taps of 0.5; an odd
        // source (7 -> 3) gives fractional edges so no source texel is dropped;
        // a 1 -> 1 axis gives one tap of 1.
        struct Tap
        {
            u32 Index = 0;
            f32 Weight = 0.0f;
        };

        using AxisFootprints = TArray<TArray<Tap>>;

        [[nodiscard]] AxisFootprints AxisTaps(u32 src, u32 dst)
        {
            AxisFootprints taps;
            taps.SetNum(static_cast<i32>(dst));
            const f64 ratio = static_cast<f64>(src) / static_cast<f64>(dst);
            for (u32 i = 0; i < dst; ++i)
            {
                const f64 begin = static_cast<f64>(i) * ratio;
                const f64 end = static_cast<f64>(i + 1u) * ratio;
                const u32 first = static_cast<u32>(std::floor(begin));
                const u32 last = std::min(src - 1u, static_cast<u32>(std::ceil(end)) - 1u);
                for (u32 s = first; s <= last; ++s)
                {
                    const f64 overlap = std::min(end, static_cast<f64>(s + 1u)) - std::max(begin, static_cast<f64>(s));
                    if (overlap > 0.0)
                        taps[static_cast<i32>(i)].Add({ s, static_cast<f32>(overlap / ratio) });
                }
            }
            return taps;
        }

        void Downsample(std::span<const u8> src, u32 srcW, std::span<u8> dst, u32 dstW, u32 dstH,
                        const AxisFootprints& xTaps, const AxisFootprints& yTaps, bool srgb)
        {
            const auto& toLinear = SrgbToLinearTable();

            for (u32 y = 0; y < dstH; ++y)
            {
                for (u32 x = 0; x < dstW; ++x)
                {
                    f32 sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                    for (const Tap& ty : yTaps[static_cast<i32>(y)])
                    {
                        for (const Tap& tx : xTaps[static_cast<i32>(x)])
                        {
                            const f32 w = tx.Weight * ty.Weight;
                            const u8* texel = &src[(static_cast<sizet>(ty.Index) * srcW + tx.Index) * 4u];
                            for (u32 c = 0; c < 3u; ++c)
                                sum[c] += w * (srgb ? toLinear[texel[c]] : static_cast<f32>(texel[c]) / 255.0f);
                            sum[3] += w * static_cast<f32>(texel[3]) / 255.0f;
                        }
                    }
                    u8* out = &dst[(static_cast<sizet>(y) * dstW + x) * 4u];
                    for (u32 c = 0; c < 3u; ++c)
                        out[c] = srgb ? LinearToSrgbByte(sum[c]) : UnormByte(sum[c]);
                    out[3] = UnormByte(sum[3]);
                }
            }
        }

        // The same footprints over an unquantised alpha plane: each level's
        // value is the exact average alpha of its footprint in level 0, which
        // is what ranks its texels for the remap. Quantising it to a byte, as
        // the stored level is, would merge footprints that differ by less than
        // 1/255 and hand the tie to texel order.
        void DownsampleAlpha(std::span<const f32> src, u32 srcW, std::span<f32> dst, u32 dstW, u32 dstH,
                             const AxisFootprints& xTaps, const AxisFootprints& yTaps)
        {
            for (u32 y = 0; y < dstH; ++y)
            {
                for (u32 x = 0; x < dstW; ++x)
                {
                    f32 sum = 0.0f;
                    for (const Tap& ty : yTaps[static_cast<i32>(y)])
                    {
                        for (const Tap& tx : xTaps[static_cast<i32>(x)])
                            sum += tx.Weight * ty.Weight * src[static_cast<sizet>(ty.Index) * srcW + tx.Index];
                    }
                    dst[static_cast<sizet>(y) * dstW + x] = sum;
                }
            }
        }

        // Rank r of N texels gets the value at rank (r + 1/2) * N0 / N of level
        // 0's N0. For every byte threshold the count of texels at or above it is
        // then level 0's scaled to N, rounded to the nearest texel, which is the
        // closest any level of N texels can come.
        [[nodiscard]] TArray64<u8> RankTargets(const AlphaHistogram& level0, u64 level0Count, u64 count)
        {
            TArray64<u8> targets;
            if (count == 0u || level0Count == 0u)
                return targets;
            targets.SetNumUninitialized(static_cast<i64>(count));
            // Walk level 0's histogram from the top: `above` counts level 0's
            // texels brighter than `value`. Ranks only grow, so the walk only
            // moves down.
            u32 value = 255u;
            u64 above = 0u;
            for (u64 rank = 0; rank < count; ++rank)
            {
                const u64 source = ((2u * rank + 1u) * level0Count) / (2u * count);
                while (value > 0u && source >= above + level0[value])
                {
                    above += level0[value];
                    --value;
                }
                targets[static_cast<i64>(rank)] = static_cast<u8>(value);
            }
            return targets;
        }

        // Replaces `rgba`'s alpha, in rank order of `footprintAlpha`, with
        // level 0's alpha values (RankTargets).
        void MatchAlphaToLevel0(std::span<u8> rgba, std::span<const f32> footprintAlpha, u32 width, u32 height,
                                const AlphaHistogram& level0, u64 level0Count)
        {
            const u64 count = static_cast<u64>(width) * height;
            if (count == 0u || level0Count == 0u)
                return;

            struct Ranked
            {
                f32 Footprint = 0.0f;
                f32 Neighbourhood = 0.0f;
                u32 Index = 0;
            };
            TArray64<Ranked> ranked;
            ranked.SetNumUninitialized(static_cast<i64>(count));
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    // Of two texels whose footprints held the same alpha, the
                    // one in the denser neighbourhood is the one that continues
                    // a shape (a blade, a leaf edge) rather than a stray texel.
                    f32 neighbourhood = 0.0f;
                    for (i32 dy = -1; dy <= 1; ++dy)
                    {
                        const u32 ny = static_cast<u32>(std::clamp(static_cast<i32>(y) + dy, 0, static_cast<i32>(height) - 1));
                        for (i32 dx = -1; dx <= 1; ++dx)
                        {
                            const u32 nx = static_cast<u32>(std::clamp(static_cast<i32>(x) + dx, 0, static_cast<i32>(width) - 1));
                            neighbourhood += footprintAlpha[static_cast<sizet>(ny) * width + nx];
                        }
                    }
                    const u32 index = y * width + x;
                    ranked[index] = { footprintAlpha[index], neighbourhood, index };
                }
            }
            // A strict ordering, not a tolerance test: two footprints tie only
            // when neither sum is larger.
            std::sort(ranked.GetData(), ranked.GetData() + ranked.Num(), [](const Ranked& a, const Ranked& b)
                      {
                          if (a.Footprint > b.Footprint)
                              return true;
                          if (a.Footprint < b.Footprint)
                              return false;
                          if (a.Neighbourhood > b.Neighbourhood)
                              return true;
                          if (a.Neighbourhood < b.Neighbourhood)
                              return false;
                          return a.Index < b.Index; });

            const TArray64<u8> targets = RankTargets(level0, level0Count, count);
            for (u64 rank = 0; rank < count; ++rank)
                rgba[static_cast<sizet>(ranked[static_cast<i64>(rank)].Index) * 4u + 3u] = targets[static_cast<i64>(rank)];
        }
    } // namespace

    f32 SanitizeCutoff(f32 cutoff) noexcept
    {
        return std::isfinite(cutoff) && cutoff > 0.0f && cutoff <= 1.0f ? cutoff : 0.0f;
    }

    u32 CappedLevelCount(u32 width, u32 height, u32 levels) noexcept
    {
        u32 kept = 1;
        u32 w = width;
        u32 h = height;
        for (u32 level = 1; level < levels; ++level)
        {
            w = NextLevelSize(w);
            h = NextLevelSize(h);
            if (std::min(w, h) < kMinCoarsestExtent)
                break;
            kept = level + 1u;
        }
        return kept;
    }

    bool HasPartialCoverage(std::span<const u8> rgba, f32 cutoff) noexcept
    {
        const f32 active = SanitizeCutoff(cutoff);
        if (!(active > 0.0f))
            return false;
        const f32 coverage = Coverage(rgba, active);
        return coverage > 0.0f && coverage < 1.0f;
    }

    bool IsCutoutAlpha(std::span<const u8> rgba) noexcept
    {
        u64 transparent = 0;
        u64 opaque = 0;
        u64 between = 0;
        for (sizet i = 3; i < rgba.size(); i += 4)
        {
            const u8 alpha = rgba[i];
            if (alpha <= 8u)
                ++transparent;
            else if (alpha >= 247u)
                ++opaque;
            else
                ++between;
        }
        const u64 total = transparent + opaque + between;
        return transparent > 0u && opaque > 0u && between * 3u <= total;
    }

    TArray64<u8> RankedTargetAlpha(std::span<const u8> level0, u64 texelCount)
    {
        return RankTargets(HistogramOf(level0), level0.size() / 4u, texelCount);
    }

    f32 WorstCoverageDifference(std::span<const u8> level, std::span<const u8> level0) noexcept
    {
        const u64 count = level.size() / 4u;
        const u64 count0 = level0.size() / 4u;
        if (count == 0u || count0 == 0u)
            return count == count0 ? 0.0f : 1.0f;
        const AlphaHistogram histogram = HistogramOf(level);
        const AlphaHistogram histogram0 = HistogramOf(level0);
        u64 atOrAbove = 0;
        u64 atOrAbove0 = 0;
        f64 worst = 0.0;
        for (u32 threshold = 255u; threshold >= 1u; --threshold)
        {
            atOrAbove += histogram[threshold];
            atOrAbove0 += histogram0[threshold];
            worst = std::max(worst, std::abs(static_cast<f64>(atOrAbove) / static_cast<f64>(count) -
                                             static_cast<f64>(atOrAbove0) / static_cast<f64>(count0)));
        }
        return static_cast<f32>(worst);
    }

    f32 Coverage(std::span<const u8> rgba, f32 cutoff) noexcept
    {
        const u64 total = rgba.size() / 4u;
        if (total == 0u)
            return 0.0f;
        const AlphaHistogram histogram = HistogramOf(rgba);
        u64 passing = 0;
        for (u32 a = 0; a < 256u; ++a)
        {
            if (Passes(a, cutoff))
                passing += histogram[a];
        }
        return static_cast<f32>(static_cast<f64>(passing) / static_cast<f64>(total));
    }

    Chain Build(std::span<const u8> level0, u32 width, u32 height, u32 mipLevels, bool srgb, bool preserveCoverage)
    {
        Chain chain;
        if (width == 0u || height == 0u || mipLevels <= 1u || level0.size() < static_cast<sizet>(width) * height * 4u)
            return chain;

        const u64 level0Count = static_cast<u64>(width) * height;
        const std::span<const u8> base = level0.first(static_cast<sizet>(level0Count) * 4u);
        const AlphaHistogram level0Histogram = preserveCoverage ? HistogramOf(base) : AlphaHistogram{};

        u64 totalBytes = 0;
        {
            u32 w = width;
            u32 h = height;
            for (u32 level = 1; level < mipLevels; ++level)
            {
                w = NextLevelSize(w);
                h = NextLevelSize(h);
                chain.Levels.Add({ w, h, totalBytes });
                totalBytes += static_cast<u64>(w) * h * 4u;
            }
        }
        chain.Bytes.SetNumZeroed(static_cast<i64>(totalBytes));

        // The previous level as the box filter produced it, before any remap,
        // so each level is filtered from real averages rather than from the
        // previous level's remapped alpha.
        TArray64<u8> previous;
        previous.Append(base.data(), static_cast<i64>(base.size()));
        TArray64<f32> previousAlpha;
        if (preserveCoverage)
        {
            previousAlpha.SetNumUninitialized(static_cast<i64>(level0Count));
            for (u64 i = 0; i < level0Count; ++i)
                previousAlpha[static_cast<i64>(i)] = static_cast<f32>(base[static_cast<sizet>(i) * 4u + 3u]) / 255.0f;
        }
        u32 prevW = width;
        u32 prevH = height;
        TArray64<u8> filtered;
        TArray64<f32> filteredAlpha;
        for (const Level& level : chain.Levels)
        {
            const AxisFootprints xTaps = AxisTaps(prevW, level.Width);
            const AxisFootprints yTaps = AxisTaps(prevH, level.Height);

            filtered.SetNumZeroed(static_cast<i64>(level.Width) * level.Height * 4);
            Downsample({ previous.GetData(), static_cast<sizet>(previous.Num()) }, prevW,
                       { filtered.GetData(), static_cast<sizet>(filtered.Num()) }, level.Width, level.Height, xTaps,
                       yTaps, srgb);

            const std::span<u8> out(chain.Bytes.GetData() + level.Offset, static_cast<sizet>(filtered.Num()));
            std::copy(filtered.GetData(), filtered.GetData() + filtered.Num(), out.begin());
            if (preserveCoverage)
            {
                filteredAlpha.SetNumZeroed(static_cast<i64>(level.Width) * level.Height);
                DownsampleAlpha({ previousAlpha.GetData(), static_cast<sizet>(previousAlpha.Num()) }, prevW,
                                { filteredAlpha.GetData(), static_cast<sizet>(filteredAlpha.Num()) }, level.Width,
                                level.Height, xTaps, yTaps);
                MatchAlphaToLevel0(out, { filteredAlpha.GetData(), static_cast<sizet>(filteredAlpha.Num()) }, level.Width,
                                   level.Height, level0Histogram, level0Count);
                std::swap(previousAlpha, filteredAlpha);
            }

            std::swap(previous, filtered);
            prevW = level.Width;
            prevH = level.Height;
        }
        return chain;
    }
} // namespace OloEngine::AlphaCoverageMips
