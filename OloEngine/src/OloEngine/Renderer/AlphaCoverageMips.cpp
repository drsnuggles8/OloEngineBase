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

        [[nodiscard]] u8 ScaledAlpha(u32 alphaByte, f32 scale) noexcept
        {
            const f32 scaled = std::round(static_cast<f32>(alphaByte) * scale);
            return static_cast<u8>(std::clamp(scaled, 0.0f, 255.0f));
        }

        using AlphaHistogram = std::array<u64, 256>;

        [[nodiscard]] AlphaHistogram HistogramOf(std::span<const u8> rgba) noexcept
        {
            AlphaHistogram histogram{};
            for (sizet i = 3; i < rgba.size(); i += 4)
                ++histogram[rgba[i]];
            return histogram;
        }

        [[nodiscard]] f64 CoverageAtScale(const AlphaHistogram& histogram, u64 total, f32 cutoff, f32 scale) noexcept
        {
            u64 passing = 0;
            for (u32 a = 0; a < 256u; ++a)
            {
                if (histogram[a] != 0u && Passes(ScaledAlpha(a, scale), cutoff))
                    passing += histogram[a];
            }
            return static_cast<f64>(passing) / static_cast<f64>(total);
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

        [[nodiscard]] TArray<TArray<Tap>> AxisTaps(u32 src, u32 dst)
        {
            TArray<TArray<Tap>> taps;
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

        void Downsample(std::span<const u8> src, u32 srcW, u32 srcH, std::span<u8> dst, u32 dstW, u32 dstH, bool srgb)
        {
            const auto xTaps = AxisTaps(srcW, dstW);
            const auto yTaps = AxisTaps(srcH, dstH);
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

    f32 Coverage(std::span<const u8> rgba, f32 cutoff) noexcept
    {
        const u64 total = rgba.size() / 4u;
        if (total == 0u)
            return 0.0f;
        return static_cast<f32>(CoverageAtScale(HistogramOf(rgba), total, cutoff, 1.0f));
    }

    f32 ScaleAlphaToCoverage(std::span<u8> rgba, f32 cutoff, f32 targetCoverage) noexcept
    {
        const u64 total = rgba.size() / 4u;
        if (total == 0u || !std::isfinite(targetCoverage))
            return 1.0f;

        const AlphaHistogram histogram = HistogramOf(rgba);
        const f64 target = std::clamp(static_cast<f64>(targetCoverage), 0.0, 1.0);

        // Coverage never decreases as the scale grows, so bisect for the
        // smallest scale that reaches the target, then keep whichever of it
        // and the largest scale below it lands closer. 256 is enough headroom:
        // it lifts alpha 1 to 255.
        f32 lo = 0.0f;
        f32 hi = 256.0f;
        for (u32 iteration = 0; iteration < 32u; ++iteration)
        {
            const f32 mid = 0.5f * (lo + hi);
            if (CoverageAtScale(histogram, total, cutoff, mid) >= target)
                hi = mid;
            else
                lo = mid;
        }
        const f64 above = CoverageAtScale(histogram, total, cutoff, hi);
        const f64 below = CoverageAtScale(histogram, total, cutoff, lo);
        const f32 scale = std::abs(below - target) < std::abs(above - target) ? lo : hi;

        // The level already matches as well as any scale can: leave it bit-exact.
        const f64 unscaled = CoverageAtScale(histogram, total, cutoff, 1.0f);
        if (std::abs(unscaled - target) <= std::abs(CoverageAtScale(histogram, total, cutoff, scale) - target))
            return 1.0f;

        for (sizet i = 3; i < rgba.size(); i += 4)
            rgba[i] = ScaledAlpha(rgba[i], scale);
        return scale;
    }

    Chain Build(std::span<const u8> level0, u32 width, u32 height, u32 mipLevels, bool srgb, f32 cutoff)
    {
        Chain chain;
        if (width == 0u || height == 0u || mipLevels <= 1u || level0.size() < static_cast<sizet>(width) * height * 4u)
            return chain;

        const f32 activeCutoff = SanitizeCutoff(cutoff);
        // A level 0 that passes everywhere or nowhere keeps that property under
        // any box filter, so there is nothing for the rescale to preserve.
        const bool preserve = HasPartialCoverage(level0, activeCutoff);
        const f32 baseCoverage = preserve ? Coverage(level0, activeCutoff) : 0.0f;

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

        // The previous level as the box filter produced it, before any rescale.
        TArray64<u8> previous;
        previous.Append(level0.data(), static_cast<i64>(width) * height * 4);
        u32 prevW = width;
        u32 prevH = height;
        TArray64<u8> filtered;
        for (const Level& level : chain.Levels)
        {
            filtered.SetNumZeroed(static_cast<i64>(level.Width) * level.Height * 4);
            Downsample({ previous.GetData(), static_cast<sizet>(previous.Num()) }, prevW, prevH,
                       { filtered.GetData(), static_cast<sizet>(filtered.Num()) }, level.Width, level.Height, srgb);

            const std::span<u8> out(chain.Bytes.GetData() + level.Offset, static_cast<sizet>(filtered.Num()));
            std::copy(filtered.GetData(), filtered.GetData() + filtered.Num(), out.begin());
            if (preserve)
                ScaleAlphaToCoverage(out, activeCutoff, baseCoverage);

            std::swap(previous, filtered);
            prevW = level.Width;
            prevH = level.Height;
        }
        return chain;
    }
} // namespace OloEngine::AlphaCoverageMips
