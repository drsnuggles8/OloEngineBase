// OLO_TEST_LAYER: L1
// =============================================================================
// FineDetailDensityTest.cpp
//
// CPU proof for the fine-detail metric and its non-regression A/B (issue #1401).
// No GL context: the metric is arithmetic on an RGBA8 buffer, so these tests
// are the whole proof of the metric itself.
//
// The tests that matter most are the NEGATIVE CONTROLS. A quality gate that is
// never seen to fail is a presence check with better marketing:
//   * a stretched-texture stand-in (the same detail, blurred) must FAIL the A/B;
//   * the same frame on both sides must PASS it;
//   * dither must raise the absolute number (so a floor is not noise-proof)
//     while leaving the A/B ordering intact (so the A/B is).
// =============================================================================

#include "OloEnginePCH.h"

#include "VisualEvidenceGuards.h"

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using VisualEvidence::ExpectConditionsPinned;
        using VisualEvidence::ExpectFineDetailNotReduced;
        using VisualEvidence::FineDetailDensity;
        using VisualEvidence::PixelRect;

        constexpr u32 kW = 96;
        constexpr u32 kH = 64;

        [[nodiscard]] std::vector<u8> Solid(u32 w, u32 h, u8 v)
        {
            std::vector<u8> px(static_cast<sizet>(w) * h * 4u, v);
            for (sizet i = 3; i < px.size(); i += 4)
                px[i] = 255;
            return px;
        }

        // 1-pixel checkerboard, 0 / 255: the densest detail an 8-bit frame can carry.
        [[nodiscard]] std::vector<u8> Checker(u32 w, u32 h, u32 cell = 1)
        {
            std::vector<u8> px = Solid(w, h, 0);
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x)
                {
                    const u8 v = (((x / cell) + (y / cell)) % 2u) ? 255 : 0;
                    const sizet i = (static_cast<sizet>(y) * w + x) * 4u;
                    px[i] = px[i + 1] = px[i + 2] = v;
                }
            return px;
        }

        // Deterministic LCG noise, uniform in [0, 255].
        [[nodiscard]] std::vector<u8> Noise(u32 w, u32 h, u32 seed = 1)
        {
            std::vector<u8> px = Solid(w, h, 0);
            u32 s = seed;
            for (sizet i = 0; i < px.size(); i += 4)
            {
                s = s * 1664525u + 1013904223u;
                px[i] = px[i + 1] = px[i + 2] = static_cast<u8>(s >> 24);
            }
            return px;
        }

        // `passes` iterations of a separable radius-1 box blur, edge-clamped:
        // approaches a Gaussian, so detail falls monotonically with `passes`
        // (one wide box does not: its sidelobes re-create edges). A stand-in for
        // "the same detail, stretched over bigger triangles".
        [[nodiscard]] std::vector<u8> Blur(const std::vector<u8>& src, u32 w, u32 h, i32 passes)
        {
            std::vector<u8> cur = src;
            std::vector<u8> next = src;
            const auto pass = [&](bool horizontal)
            {
                for (i32 y = 0; y < static_cast<i32>(h); ++y)
                    for (i32 x = 0; x < static_cast<i32>(w); ++x)
                        for (sizet c = 0; c < 3; ++c)
                        {
                            f32 sum = 0.0f;
                            for (i32 k = -1; k <= 1; ++k)
                            {
                                const i32 xx = horizontal ? std::clamp(x + k, 0, static_cast<i32>(w) - 1) : x;
                                const i32 yy = horizontal ? y : std::clamp(y + k, 0, static_cast<i32>(h) - 1);
                                sum += static_cast<f32>(cur[(static_cast<sizet>(yy) * w + xx) * 4u + c]);
                            }
                            next[(static_cast<sizet>(y) * w + x) * 4u + c] = static_cast<u8>(std::lround(sum / 3.0f));
                        }
                cur = next;
            };
            for (i32 i = 0; i < passes; ++i)
            {
                pass(true);
                pass(false);
            }
            return cur;
        }

        // Add +-amplitude ordered-dither-like noise to every channel, clamped.
        [[nodiscard]] std::vector<u8> Dither(const std::vector<u8>& src, i32 amplitude, u32 seed = 7)
        {
            std::vector<u8> out = src;
            u32 s = seed;
            for (sizet i = 0; i < out.size(); i += 4)
            {
                s = s * 1664525u + 1013904223u;
                const i32 d = static_cast<i32>((s >> 24) % static_cast<u32>(2 * amplitude + 1)) - amplitude;
                for (sizet c = 0; c < 3; ++c)
                    out[i + c] = static_cast<u8>(std::clamp(static_cast<i32>(out[i + c]) + d, 0, 255));
            }
            return out;
        }

        // A frame with real structure: 8 px checker cells modulated by a smooth
        // ramp, i.e. moderate-amplitude edges spread over the frame.
        [[nodiscard]] std::vector<u8> Structured(u32 w, u32 h)
        {
            std::vector<u8> px = Checker(w, h, 3);
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * w + x) * 4u;
                    const f32 gain = 0.35f + 0.65f * static_cast<f32>(x) / static_cast<f32>(w);
                    for (sizet c = 0; c < 3; ++c)
                        px[i + c] = static_cast<u8>(static_cast<f32>(px[i + c]) * gain);
                }
            return px;
        }

        constexpr PixelRect kFull{ 0, 0, kW, kH };
    } // namespace

    // ── The metric ───────────────────────────────────────────────────────────

    TEST(FineDetailDensityTest, FlatFieldHasNoDetail)
    {
        EXPECT_EQ(FineDetailDensity(Solid(kW, kH, 0), kW, kH), 0.0);
        EXPECT_EQ(FineDetailDensity(Solid(kW, kH, 137), kW, kH), 0.0);
        EXPECT_EQ(FineDetailDensity(Solid(kW, kH, 255), kW, kH), 0.0);
    }

    TEST(FineDetailDensityTest, CheckerboardIsAlmostAllDetail)
    {
        EXPECT_GT(FineDetailDensity(Checker(kW, kH), kW, kH), 0.99);
    }

    TEST(FineDetailDensityTest, NoiseIsHighDetail)
    {
        EXPECT_GT(FineDetailDensity(Noise(kW, kH), kW, kH), 0.8);
    }

    TEST(FineDetailDensityTest, BlurringIsMonotoneAndEventuallyRemovesEverything)
    {
        // Fine texture (noise) is what foliage is; see the hard-edge caveat below.
        const std::vector<u8> src = Noise(kW, kH);
        f64 previous = FineDetailDensity(src, kW, kH);
        ASSERT_GT(previous, 0.5) << "the source has too little detail for the monotonicity check to mean anything";
        for (i32 r = 1; r <= 16; r *= 2)
        {
            const f64 d = FineDetailDensity(Blur(src, kW, kH, r), kW, kH);
            EXPECT_LE(d, previous) << "blur of " << r << " passes increased detail";
            previous = d;
        }
        EXPECT_LT(previous, 0.05) << "16 blur passes should leave a 3 px checker as a near-flat field";
    }

    TEST(FineDetailDensityTest, CaveatHardEdgesWidenUnderLightBlurSoTheCountIsNotAStructureMeter)
    {
        // Documented limit, pinned so the guide's claim stays true: a hard
        // high-contrast edge is 1 px wide, and a light blur spreads it over
        // several pixels that each still exceed the threshold, so the COUNT
        // rises before it falls. The metric orders fine texture reliably; it is
        // not a sharpness meter for hard-edged content. Compare like with like.
        const std::vector<u8> hard = Structured(kW, kH);
        const f64 sharp = FineDetailDensity(hard, kW, kH);
        const f64 softened = FineDetailDensity(Blur(hard, kW, kH, 1), kW, kH);
        EXPECT_GT(softened, sharp);
        EXPECT_LT(FineDetailDensity(Blur(hard, kW, kH, 16), kW, kH), 0.05) << "and it still ends flat";
    }

    TEST(FineDetailDensityTest, CropMeasuresOnlyTheCrop)
    {
        // Left half flat, right half checker.
        std::vector<u8> px = Solid(kW, kH, 90);
        const std::vector<u8> checker = Checker(kW, kH);
        for (u32 y = 0; y < kH; ++y)
            for (u32 x = kW / 2u; x < kW; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * kW + x) * 4u;
                std::copy_n(checker.begin() + static_cast<std::ptrdiff_t>(i), 4, px.begin() + static_cast<std::ptrdiff_t>(i));
            }
        EXPECT_EQ(FineDetailDensity(px, kW, kH, PixelRect{ 0, 0, kW / 4u, kH }), 0.0);
        EXPECT_GT(FineDetailDensity(px, kW, kH, PixelRect{ kW / 2u + 2u, 0, kW / 2u - 4u, kH }), 0.99);
        const f64 whole = FineDetailDensity(px, kW, kH);
        EXPECT_GT(whole, 0.4);
        EXPECT_LT(whole, 0.6);
    }

    TEST(FineDetailDensityTest, ThresholdIsExclusiveAndScalesTheResult)
    {
        // A vertical step of height 16: the forward difference at the one
        // column left of the edge is 16, exactly.
        std::vector<u8> px = Solid(kW, kH, 0);
        for (u32 y = 0; y < kH; ++y)
            for (u32 x = kW / 2u; x < kW; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * kW + x) * 4u;
                px[i] = px[i + 1] = px[i + 2] = 16;
            }
        const f64 oneColumn = 1.0 / static_cast<f64>(kW);
        EXPECT_NEAR(FineDetailDensity(px, kW, kH, 15.9f), oneColumn, 1e-9);
        EXPECT_EQ(FineDetailDensity(px, kW, kH, 16.0f), 0.0) << "the comparison is strictly greater-than";
    }

    TEST(FineDetailDensityTest, UnmeasurableInputReadsAsNaNNeverAsZero)
    {
        const std::vector<u8> ok = Checker(kW, kH);
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_TRUE(std::isnan(FineDetailDensity({}, kW, kH)));
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, 0, kH)));
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, kW, 0)));
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, kW + 1u, kH))) << "size mismatch";
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, kW, kH, PixelRect{ 0, 0, 0, 5 })));
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, kW, kH, PixelRect{ kW - 1u, 0, 2, 2 }))) << "crop leaves the frame";
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, kW, kH, PixelRect{ 0xFFFFFFFFu, 0, 2, 2 }))) << "wraparound";
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, kW, kH, nan)));
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, kW, kH, -1.0f)));
        EXPECT_TRUE(std::isnan(FineDetailDensity(ok, kW, kH, std::numeric_limits<f32>::infinity())));
    }

    // ── The A/B gate, and its negative controls ──────────────────────────────

    TEST(FineDetailDensityTest, NegativeControl_AStretchedTextureFailsTheAB)
    {
        // "Feature ON" is the same content smeared over big triangles: exactly
        // what #1233 was measured doing on the #1224 captures.
        const std::vector<u8> off = Structured(kW, kH);
        const std::vector<u8> on = Blur(off, kW, kH, 4);
        ASSERT_LT(FineDetailDensity(on, kW, kH), FineDetailDensity(off, kW, kH));
        EXPECT_NONFATAL_FAILURE(ExpectFineDetailNotReduced(on, off, kW, kH, kFull, "stretched"), "REDUCED fine-detail");
    }

    TEST(FineDetailDensityTest, NegativeControl_TheSameFrameOnBothSidesPasses)
    {
        const std::vector<u8> frame = Structured(kW, kH);
        const f64 ratio = ExpectFineDetailNotReduced(frame, frame, kW, kH, kFull, "identical");
        EXPECT_DOUBLE_EQ(ratio, 1.0);
    }

    TEST(FineDetailDensityTest, AddingRealDetailPasses)
    {
        const std::vector<u8> on = Noise(kW, kH);
        const std::vector<u8> off = Blur(on, kW, kH, 2);
        EXPECT_GT(ExpectFineDetailNotReduced(on, off, kW, kH, kFull, "richer"), 1.0);
    }

    TEST(FineDetailDensityTest, DitherRaisesTheAbsoluteNumberButNotTheABOrdering)
    {
        // The honest caveat from #1401, as an executable statement.
        const std::vector<u8> crisp = Structured(kW, kH);
        const std::vector<u8> smeared = Blur(crisp, kW, kH, 4);

        // Same dither on both arms: what a stochastic-coverage feature does.
        const std::vector<u8> crispDithered = Dither(crisp, 12);
        const std::vector<u8> smearedDithered = Dither(smeared, 12);

        const f64 smearedPlain = FineDetailDensity(smeared, kW, kH);
        const f64 smearedNoisy = FineDetailDensity(smearedDithered, kW, kH);
        EXPECT_GT(smearedNoisy, smearedPlain * 2.0 + 0.01) << "dither did not raise the absolute number, so this control is vacuous";

        // (1) A FLOOR set from the crisp frame is defeated: the smeared, dithered frame clears a floor the smeared
        //     plain frame fails. This is why an absolute floor may only be used on a temporally stable frame.
        const f64 floorFromCrisp = FineDetailDensity(crisp, kW, kH) * 0.5;
        EXPECT_LT(smearedPlain, floorFromCrisp);
        EXPECT_GT(smearedNoisy, floorFromCrisp) << "dither no longer defeats a floor; re-read the caveat in the guide";

        // (2) The A/B survives it: with identical dither on both arms, smeared-on versus crisp-off still fails.
        EXPECT_NONFATAL_FAILURE(
            ExpectFineDetailNotReduced(smearedDithered, crispDithered, kW, kH, kFull, "dithered stretched"),
            "REDUCED fine-detail");
        // and crisp-on versus smeared-off still passes.
        EXPECT_GT(ExpectFineDetailNotReduced(crispDithered, smearedDithered, kW, kH, kFull, "dithered richer"), 1.0);
    }

    TEST(FineDetailDensityTest, ABOnUnmeasurableInputFailsLoudly)
    {
        const std::vector<u8> ok = Structured(kW, kH);
        EXPECT_NONFATAL_FAILURE(ExpectFineDetailNotReduced({}, ok, kW, kH, kFull, "empty"), "not measurable");
        EXPECT_NONFATAL_FAILURE(ExpectFineDetailNotReduced(ok, Structured(kW + 1u, kH), kW, kH, kFull, "size"),
                                "not measurable");
    }

    TEST(FineDetailDensityTest, SlackIsAbsoluteAndBoundedByTheCaller)
    {
        const std::vector<u8> off = Noise(kW, kH);
        const std::vector<u8> on = Blur(off, kW, kH, 1);
        const f64 gap = FineDetailDensity(off, kW, kH) - FineDetailDensity(on, kW, kH);
        ASSERT_GT(gap, 0.0);
        EXPECT_NONFATAL_FAILURE(ExpectFineDetailNotReduced(on, off, kW, kH, kFull, "no slack"), "REDUCED");
        (void)ExpectFineDetailNotReduced(on, off, kW, kH, kFull, "slack covers the gap", gap + 1e-9);
    }

    TEST(FineDetailDensityTest, ConditionsPinnedFailsWhenAnArmDiffers)
    {
        struct Conditions
        {
            u32 Width;
            u32 Height;
            u32 Msaa;
            auto operator==(const Conditions&) const -> bool = default;
        };
        ExpectConditionsPinned(Conditions{ 960, 540, 1 }, Conditions{ 960, 540, 1 }, "same");
        EXPECT_NONFATAL_FAILURE(ExpectConditionsPinned(Conditions{ 960, 540, 1 }, Conditions{ 1920, 1080, 1 }, "res"),
                                "different conditions");
        EXPECT_NONFATAL_FAILURE(ExpectConditionsPinned(Conditions{ 960, 540, 1 }, Conditions{ 960, 540, 4 }, "msaa"),
                                "different conditions");
    }
} // namespace OloEngine::Tests
