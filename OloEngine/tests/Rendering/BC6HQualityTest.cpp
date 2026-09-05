// OLO_TEST_LAYER: L3
//
// Per-block-class quality floors for the BC6H HDR encoder (#624).
//
// TextureCompressionTest already pins the round trip on one curved gradient. That single
// aggregate number is exactly the wrong shape for encoder work: a multi-mode encoder can
// raise it while quietly regressing the smooth blocks that dominate a real environment
// map, and nothing would notice. So this file measures FIVE block classes separately —
// flat, smooth, curved, two-cluster and wide-dynamic-range — and asserts a floor on each.
//
// Two metrics per class, because neither alone is honest for HDR:
//   * peak-normalized PSNR, the figure #440 and issue #624 quote, which is an absolute
//     error measure and is therefore dominated by the brightest texels;
//   * relative RMS error, which is what a dark region's banding actually shows up in and
//     which the peak-normalized number hides completely.
// Both are printed for every class so a future encoder change can be compared honestly
// rather than argued about.
//
// The decode side is the vendored bcdec reference decoder (via DecodeToRGBAFloat), not
// our own encoder's assumptions.

#include "BC6HBlockModeReader.h"

#include "OloEngine/Renderer/BC6HEncoder.h"
#include "OloEngine/Renderer/TextureCompression.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    constexpr u32 kDim = 64; // 16x16 = 256 blocks per class

    struct QualityResult
    {
        double PeakPSNR = 0.0;    // dB, normalized to the image's peak value
        double RelativeRMS = 0.0; // fraction (0.02 == 2% RMS relative error)
        // Which block modes the encoder actually chose, indexed by bcdec mode number.
        std::array<u32, BC6H::kModeCount> ModeHistogram{};
        u32 BlockCount = 0;
    };

    // Encode `source` (RGB float, kDim x kDim) as BC6H, decode it back through bcdec, and
    // measure both metrics against the source.
    QualityResult MeasureBC6H(const std::vector<f32>& source, f32 peak)
    {
        QualityResult result;
        const CompressedTextureImage image = TextureCompression::EncodeBC6H(source.data(), kDim, kDim, 3, /*isSigned*/ false, false);
        EXPECT_TRUE(image.IsValid());
        if (!image.IsValid())
            return result;

        std::vector<f32> decoded;
        u32 dw = 0;
        u32 dh = 0;
        EXPECT_TRUE(TextureCompression::DecodeToRGBAFloat(image, 0, decoded, dw, dh));
        if (decoded.size() != static_cast<sizet>(kDim) * kDim * 4)
            return result;

        // Record which mode won each block. The mode field is re-read straight from the
        // packed bytes, so this reports what a decoder will see rather than what the
        // encoder believes it wrote.
        for (sizet offset = 0; offset + 16 <= image.Mips[0].size(); offset += 16)
        {
            const i32 mode = Tests::ReadBC6HModeIndex(image.Mips[0].data() + offset);
            EXPECT_GE(mode, 0) << "a packed block carries a reserved BC6H mode pattern";
            if (mode >= 0 && mode < static_cast<i32>(BC6H::kModeCount))
                ++result.ModeHistogram[static_cast<u32>(mode)];
            ++result.BlockCount;
        }

        double squaredError = 0.0;
        double squaredRelative = 0.0;
        sizet samples = 0;
        for (sizet texel = 0; texel < static_cast<sizet>(kDim) * kDim; ++texel)
        {
            for (u32 c = 0; c < 3; ++c)
            {
                const double a = static_cast<double>(source[texel * 3 + c]);
                const double b = static_cast<double>(decoded[texel * 4 + c]);
                const double diff = a - b;
                squaredError += diff * diff;
                // Relative error is only meaningful against a non-trivial magnitude; the
                // generators below keep every component above 0.01 so this never divides
                // by something that rounds the metric into noise.
                const double denominator = std::max(a, 1e-3);
                squaredRelative += (diff / denominator) * (diff / denominator);
                ++samples;
            }
        }
        if (samples == 0)
            return result;

        const double mse = squaredError / static_cast<double>(samples);
        result.PeakPSNR = mse < 1e-12 ? 99.0 : 10.0 * std::log10((static_cast<double>(peak) * peak) / mse);
        result.RelativeRMS = std::sqrt(squaredRelative / static_cast<double>(samples));
        return result;
    }

    // Print + record so the numbers land in the test log and the XML report, not just in
    // an assertion message that only appears on failure.
    void Report(const char* blockClass, const QualityResult& result)
    {
        std::string modes;
        u32 twoSubsetBlocks = 0;
        for (u32 mode = 0; mode < BC6H::kModeCount; ++mode)
        {
            if (result.ModeHistogram[mode] == 0)
                continue;
            if (Tests::IsBC6HTwoSubsetMode(static_cast<i32>(mode)))
                twoSubsetBlocks += result.ModeHistogram[mode];
            modes += " m" + std::to_string(mode) + "=" + std::to_string(result.ModeHistogram[mode]);
        }
        std::printf("[ BC6H     ] %-14s peak-normalized PSNR %6.2f dB   relative RMS %6.3f %%   modes:%s\n",
                    blockClass, result.PeakPSNR, result.RelativeRMS * 100.0, modes.c_str());
        ::testing::Test::RecordProperty(std::string(blockClass) + "_TwoSubsetBlocks", std::to_string(twoSubsetBlocks));
        ::testing::Test::RecordProperty(std::string(blockClass) + "_PeakPSNR_dB",
                                        std::to_string(result.PeakPSNR));
        ::testing::Test::RecordProperty(std::string(blockClass) + "_RelativeRMS_pct",
                                        std::to_string(result.RelativeRMS * 100.0));
    }

    // ---- Block-class generators ---------------------------------------------
    // Each fills a kDim x kDim RGB float image whose 4x4 blocks all belong to one class.

    // Flat: every block is a single constant colour (blocks differ from each other, so
    // this is not one trivial image). Coincident endpoints — should be near-exact.
    std::vector<f32> MakeFlatBlocks(f32 peak)
    {
        std::vector<f32> pixels(static_cast<sizet>(kDim) * kDim * 3);
        for (u32 y = 0; y < kDim; ++y)
        {
            for (u32 x = 0; x < kDim; ++x)
            {
                const f32 bx = static_cast<f32>(x / 4) / static_cast<f32>(kDim / 4 - 1);
                const f32 by = static_cast<f32>(y / 4) / static_cast<f32>(kDim / 4 - 1);
                f32* p = &pixels[(static_cast<sizet>(y) * kDim + x) * 3];
                p[0] = 0.02f + peak * bx;
                p[1] = 0.02f + peak * by;
                p[2] = 0.02f + peak * 0.5f * (bx + by) * 0.5f;
            }
        }
        return pixels;
    }

    // Smooth: a gentle linear ramp — one straight segment per block fits it well. This is
    // the class most of a real environment map falls into, and the one a careless
    // multi-mode encoder regresses.
    std::vector<f32> MakeSmoothGradient(f32 peak)
    {
        std::vector<f32> pixels(static_cast<sizet>(kDim) * kDim * 3);
        for (u32 y = 0; y < kDim; ++y)
        {
            for (u32 x = 0; x < kDim; ++x)
            {
                const f32 fx = static_cast<f32>(x) / static_cast<f32>(kDim - 1);
                const f32 fy = static_cast<f32>(y) / static_cast<f32>(kDim - 1);
                f32* p = &pixels[(static_cast<sizet>(y) * kDim + x) * 3];
                p[0] = 0.02f + peak * fx;
                p[1] = 0.02f + peak * fy;
                p[2] = 0.02f + peak * 0.5f * (fx + fy) * 0.5f;
            }
        }
        return pixels;
    }

    // Curved: the quadratic ramp #440 measured (~38 dB). A single linear segment per
    // block cannot follow the curvature, so this is where extra endpoint precision pays.
    std::vector<f32> MakeCurvedGradient(f32 peak)
    {
        std::vector<f32> pixels(static_cast<sizet>(kDim) * kDim * 3);
        for (u32 y = 0; y < kDim; ++y)
        {
            for (u32 x = 0; x < kDim; ++x)
            {
                const f32 fx = static_cast<f32>(x) / static_cast<f32>(kDim - 1);
                const f32 fy = static_cast<f32>(y) / static_cast<f32>(kDim - 1);
                f32* p = &pixels[(static_cast<sizet>(y) * kDim + x) * 3];
                p[0] = 0.02f + peak * fx * fx;
                p[1] = 0.02f + peak * fy;
                p[2] = 0.02f + peak * (fx * fy) * 0.5f;
            }
        }
        return pixels;
    }

    // Two-cluster: a hard diagonal edge inside every block splits its texels into two
    // radiance levels ~100x apart. The intuition is that this needs the two-subset modes;
    // the measurement says otherwise (see the test below), which is exactly why it is
    // measured rather than assumed.
    std::vector<f32> MakeTwoClusterBlocks(f32 peak)
    {
        std::vector<f32> pixels(static_cast<sizet>(kDim) * kDim * 3);
        for (u32 y = 0; y < kDim; ++y)
        {
            for (u32 x = 0; x < kDim; ++x)
            {
                const bool upper = ((x % 4) + (y % 4)) < 3;
                const f32 bx = static_cast<f32>(x / 4) / static_cast<f32>(kDim / 4 - 1);
                f32* p = &pixels[(static_cast<sizet>(y) * kDim + x) * 3];
                if (upper)
                {
                    p[0] = 0.05f + peak * 0.05f * bx;
                    p[1] = 0.10f;
                    p[2] = 0.40f;
                }
                else
                {
                    p[0] = 0.05f + peak * (0.60f + 0.35f * bx);
                    p[1] = 0.05f + peak * 0.70f;
                    p[2] = 0.05f + peak * 0.20f;
                }
            }
        }
        return pixels;
    }

    // Wide dynamic range: a small bright "sun" over a dim sky, so single blocks straddle
    // three orders of magnitude. Tests that bright detail does not eat the dark detail.
    std::vector<f32> MakeWideDynamicRange(f32 peak)
    {
        std::vector<f32> pixels(static_cast<sizet>(kDim) * kDim * 3);
        for (u32 y = 0; y < kDim; ++y)
        {
            for (u32 x = 0; x < kDim; ++x)
            {
                const f32 dx = static_cast<f32>(x) - 30.5f;
                const f32 dy = static_cast<f32>(y) - 30.5f;
                const f32 radius = std::sqrt(dx * dx + dy * dy);
                const f32 sun = peak * std::exp(-radius * radius / 40.0f);
                f32* p = &pixels[(static_cast<sizet>(y) * kDim + x) * 3];
                p[0] = 0.03f + sun;
                p[1] = 0.05f + sun * 0.9f;
                p[2] = 0.09f + sun * 0.7f;
            }
        }
        return pixels;
    }
} // namespace

// One test per class so a regression names the class it broke, instead of one test that
// says "BC6H got worse somewhere".

TEST(BC6HQuality, FlatBlocksAreNearLossless)
{
    constexpr f32 kPeak = 8.0f;
    const QualityResult result = MeasureBC6H(MakeFlatBlocks(kPeak), kPeak);
    Report("flat", result);
    // Measured 72.5 dB / 0.045 % (was 50.6 dB / 0.708 % with the mode-11-only encoder);
    // mode 13 stores a 16-bit endpoint, so a constant block is exact but for the half
    // quantization of that one value. Floors sit a margin below the measurement.
    EXPECT_GT(result.PeakPSNR, 68.0) << "flat blocks should be essentially exact";
    EXPECT_LT(result.RelativeRMS, 0.001) << "flat blocks should hold sub-0.1% relative error";
}

TEST(BC6HQuality, SmoothGradientHoldsHighFidelity)
{
    constexpr f32 kPeak = 8.0f;
    const QualityResult result = MeasureBC6H(MakeSmoothGradient(kPeak), kPeak);
    Report("smooth", result);
    // Measured 44.3 dB / 2.73 % (was 39.4 dB / 7.48 %).
    EXPECT_GT(result.PeakPSNR, 42.0) << "smooth gradient regressed";
    EXPECT_LT(result.RelativeRMS, 0.035) << "smooth gradient relative error regressed";
}

TEST(BC6HQuality, CurvedGradientClearsTheBaseline)
{
    constexpr f32 kPeak = 8.0f;
    const QualityResult result = MeasureBC6H(MakeCurvedGradient(kPeak), kPeak);
    Report("curved", result);
    // Measured 42.5 dB / 2.74 %. #440 scored 38.4 dB / 4.49 % on this exact generator,
    // which is the "~38 dB" figure issue #624 quotes; the floor is above it deliberately,
    // so falling back to a one-mode encoder fails here rather than passing quietly.
    EXPECT_GT(result.PeakPSNR, 40.0) << "curved gradient regressed below the #624 target";
    EXPECT_LT(result.RelativeRMS, 0.035) << "curved gradient relative error regressed";
}

TEST(BC6HQuality, TwoClusterBlocksSurviveASingleEndpointPair)
{
    constexpr f32 kPeak = 8.0f;
    const QualityResult result = MeasureBC6H(MakeTwoClusterBlocks(kPeak), kPeak);
    Report("two-cluster", result);
    // Measured 50.5 dB / 0.66 %, unchanged from #440 — worth knowing rather than
    // assuming: BC6H interpolates in a half-float (roughly logarithmic) space, which
    // compresses a 100:1 radiance ratio into a ~1.6:1 spread of endpoint values, so even
    // a hard bright/dark edge is well inside one endpoint pair's reach. The chosen-mode
    // histogram printed above is the evidence for which modes actually carry this class.
    EXPECT_GT(result.PeakPSNR, 48.0) << "two-cluster blocks regressed";
    EXPECT_LT(result.RelativeRMS, 0.01) << "two-cluster relative error regressed";
}

TEST(BC6HQuality, WideDynamicRangeKeepsDarkDetail)
{
    constexpr f32 kPeak = 64.0f;
    const QualityResult result = MeasureBC6H(MakeWideDynamicRange(kPeak), kPeak);
    Report("wide-range", result);
    // Measured 53.8 dB / 1.84 % (was 52.9 dB / 1.98 %).
    EXPECT_GT(result.PeakPSNR, 51.0) << "wide-dynamic-range blocks regressed";
    // The relative floor is the one that matters here: it is what catches an encoder that
    // buys a better peak-normalized number by crushing the dim sky.
    EXPECT_LT(result.RelativeRMS, 0.025) << "dark detail lost — relative error regressed";
}
