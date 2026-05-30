// =============================================================================
// AutoExposureMathTest.cpp
//
// Pins the CPU math in OloEngine/Renderer/AutoExposure.h, which is the exact
// mirror of the histogram / averaging / adaptation logic ported into:
//   - assets/shaders/compute/AutoExposureHistogram.comp
//   - assets/shaders/compute/AutoExposureAverage.comp
//
// Auto-exposure is invisible to a single golden frame (the whole point is that
// it converges over many frames), so the per-frame contract has to be pinned on
// the CPU: a sign flip in the EV100 model, a broken bin mapping, or a
// frame-rate-dependent adaptation step would all produce "exposure pumps / never
// settles / wrong direction" bugs that no static screenshot catches. If this
// drifts from the .comp twins the GPU result is no longer pinned — update both.
//
// Classification: shaderpipe (CPU mirror of shader math).
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/AutoExposure.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using AutoExposure::kHistogramBins;

        // Default metering window used across the tests (matches the engine
        // defaults in PostProcessSettings).
        constexpr f32 kMinLogLum = -8.0f;
        constexpr f32 kMaxLogLum = 3.5f;
        constexpr f32 kLogLumRange = kMaxLogLum - kMinLogLum;
        constexpr f32 kInvLogLumRange = 1.0f / kLogLumRange;
        // One bin's worth of log2 luminance — the inherent quantisation error
        // of the round-trip through the histogram.
        constexpr f32 kBinWidthLog2 = kLogLumRange / 254.0f;

        // Build a histogram from a flat list of pixel luminances, exactly as the
        // GPU histogram pass would (LuminanceToBin + atomic increment).
        std::array<u32, kHistogramBins> MakeHistogram(const std::vector<f32>& luminances)
        {
            std::array<u32, kHistogramBins> h{};
            h.fill(0u);
            for (const f32 lum : luminances)
                ++h[AutoExposure::LuminanceToBin(lum, kMinLogLum, kInvLogLumRange)];
            return h;
        }
    } // namespace

    // ----- Luminance ---------------------------------------------------------

    TEST(AutoExposureMathTest, LuminanceUsesRec709Weights)
    {
        EXPECT_NEAR(AutoExposure::Luminance({ 1.0f, 0.0f, 0.0f }), 0.2126f, 1e-6f);
        EXPECT_NEAR(AutoExposure::Luminance({ 0.0f, 1.0f, 0.0f }), 0.7152f, 1e-6f);
        EXPECT_NEAR(AutoExposure::Luminance({ 0.0f, 0.0f, 1.0f }), 0.0722f, 1e-6f);
        EXPECT_NEAR(AutoExposure::Luminance({ 1.0f, 1.0f, 1.0f }), 1.0f, 1e-6f);
        // Negative channels (can appear after some filters) are clamped to 0.
        EXPECT_NEAR(AutoExposure::Luminance({ -5.0f, 0.0f, 0.0f }), 0.0f, 1e-6f);
    }

    // ----- Bin mapping -------------------------------------------------------

    TEST(AutoExposureMathTest, BlackPixelsGoToBinZero)
    {
        EXPECT_EQ(AutoExposure::LuminanceToBin(0.0f, kMinLogLum, kInvLogLumRange), 0u);
        EXPECT_EQ(AutoExposure::LuminanceToBin(1e-9f, kMinLogLum, kInvLogLumRange), 0u);
        // A NaN must not crash or land in a live bin.
        EXPECT_EQ(AutoExposure::LuminanceToBin(std::nanf(""), kMinLogLum, kInvLogLumRange), 0u);
        // Anything above the black threshold lands in bins 1..255.
        EXPECT_GE(AutoExposure::LuminanceToBin(1.0f, kMinLogLum, kInvLogLumRange), 1u);
    }

    TEST(AutoExposureMathTest, BinMappingIsMonotonicAndClamped)
    {
        u32 prev = 0;
        for (f32 ev = kMinLogLum; ev <= kMaxLogLum; ev += 0.5f)
        {
            const u32 bin = AutoExposure::LuminanceToBin(std::exp2(ev), kMinLogLum, kInvLogLumRange);
            EXPECT_GE(bin, prev) << "bins must be non-decreasing in luminance (ev=" << ev << ")";
            EXPECT_LE(bin, 255u);
            prev = bin;
        }
        // Below / above the window clamps to the first / last live bin.
        EXPECT_EQ(AutoExposure::LuminanceToBin(std::exp2(kMinLogLum - 5.0f), kMinLogLum, kInvLogLumRange), 1u);
        EXPECT_EQ(AutoExposure::LuminanceToBin(std::exp2(kMaxLogLum + 5.0f), kMinLogLum, kInvLogLumRange), 255u);
    }

    TEST(AutoExposureMathTest, BinRoundTripRecoversLuminance)
    {
        for (f32 ev = kMinLogLum + 0.5f; ev <= kMaxLogLum - 0.5f; ev += 0.7f)
        {
            const f32 lum = std::exp2(ev);
            const u32 bin = AutoExposure::LuminanceToBin(lum, kMinLogLum, kInvLogLumRange);
            const f32 recovered = AutoExposure::BinToLuminance(static_cast<f32>(bin), kMinLogLum, kLogLumRange);
            // Round-trip error is bounded by the quantisation of one bin.
            EXPECT_LE(std::abs(std::log2(recovered) - ev), kBinWidthLog2 + 1e-4f);
        }
    }

    // ----- Histogram average -------------------------------------------------

    TEST(AutoExposureMathTest, UniformHistogramRecoversThatLuminance)
    {
        const f32 lum = std::exp2(0.0f); // 1.0
        const auto h = MakeHistogram(std::vector<f32>(10000, lum));
        const f32 avg = AutoExposure::ComputeAverageLuminance(h, kMinLogLum, kMaxLogLum);
        EXPECT_LE(std::abs(std::log2(avg) - std::log2(lum)), kBinWidthLog2 + 1e-3f);
    }

    TEST(AutoExposureMathTest, AllBlackFloorsAtMinLogLum)
    {
        std::array<u32, kHistogramBins> h{};
        h.fill(0u);
        h[0] = 50000u; // entirely black frame
        const f32 avg = AutoExposure::ComputeAverageLuminance(h, kMinLogLum, kMaxLogLum);
        EXPECT_NEAR(std::log2(avg), kMinLogLum, 1e-3f);
    }

    TEST(AutoExposureMathTest, BlackPixelsDoNotDragTheAverage)
    {
        const f32 lum = std::exp2(1.0f); // 2.0
        const auto bright = MakeHistogram(std::vector<f32>(4000, lum));
        const f32 avgBright = AutoExposure::ComputeAverageLuminance(bright, kMinLogLum, kMaxLogLum);

        // Same bright pixels, plus a large black border: bin 0 is excluded so
        // the average over the lit pixels must be unchanged.
        auto withBlack = bright;
        withBlack[0] += 40000u;
        const f32 avgWithBlack = AutoExposure::ComputeAverageLuminance(withBlack, kMinLogLum, kMaxLogLum);
        EXPECT_NEAR(avgBright, avgWithBlack, avgBright * 1e-4f);
    }

    TEST(AutoExposureMathTest, MixedHistogramAveragesBetweenExtremes)
    {
        const f32 dark = std::exp2(-3.0f);
        const f32 lite = std::exp2(2.0f);
        std::vector<f32> px(5000, dark);
        px.insert(px.end(), 5000, lite);
        const f32 avg = AutoExposure::ComputeAverageLuminance(MakeHistogram(px), kMinLogLum, kMaxLogLum);
        // The average bin sits between the two populations (log-domain mean).
        EXPECT_GT(std::log2(avg), -3.0f);
        EXPECT_LT(std::log2(avg), 2.0f);
        EXPECT_NEAR(std::log2(avg), -0.5f, 0.2f); // midpoint of -3 and 2
    }

    // ----- Eye adaptation ----------------------------------------------------

    TEST(AutoExposureMathTest, AdaptationConvergesToTarget)
    {
        f32 current = 0.1f;
        const f32 target = 4.0f;
        for (int i = 0; i < 2000; ++i)
            current = AutoExposure::AdaptLuminance(current, target, 1.0f / 60.0f, 3.0f, 3.0f);
        EXPECT_NEAR(current, target, target * 1e-3f);
    }

    TEST(AutoExposureMathTest, AdaptationIsFrameRateIndependent)
    {
        const f32 start = 0.2f;
        const f32 target = 5.0f;
        const f32 speed = 2.5f;

        // One step of dt.
        const f32 oneBig = AutoExposure::AdaptLuminance(start, target, 0.5f, speed, speed);
        // Two steps of dt/2 must reach the same place (exact for exp easing).
        f32 twoSmall = AutoExposure::AdaptLuminance(start, target, 0.25f, speed, speed);
        twoSmall = AutoExposure::AdaptLuminance(twoSmall, target, 0.25f, speed, speed);
        EXPECT_NEAR(oneBig, twoSmall, 1e-4f);
    }

    TEST(AutoExposureMathTest, SeparateUpAndDownSpeeds)
    {
        const f32 dt = 1.0f / 60.0f;
        // Brightening uses speedUp; with a much larger speedUp it moves further
        // toward the brighter target in one step than the slow-down case does.
        const f32 up = AutoExposure::AdaptLuminance(1.0f, 10.0f, dt, 8.0f, 0.5f);
        const f32 upSlow = AutoExposure::AdaptLuminance(1.0f, 10.0f, dt, 0.5f, 8.0f);
        EXPECT_GT(up, upSlow);

        // Darkening uses speedDown symmetrically.
        const f32 down = AutoExposure::AdaptLuminance(10.0f, 1.0f, dt, 0.5f, 8.0f);
        const f32 downSlow = AutoExposure::AdaptLuminance(10.0f, 1.0f, dt, 8.0f, 0.5f);
        EXPECT_LT(down, downSlow);
    }

    TEST(AutoExposureMathTest, AdaptationEdgeCases)
    {
        // First frame: invalid/zero history snaps straight to target.
        EXPECT_NEAR(AutoExposure::AdaptLuminance(0.0f, 3.0f, 0.016f, 3.0f, 1.0f), 3.0f, 1e-6f);
        EXPECT_NEAR(AutoExposure::AdaptLuminance(-1.0f, 3.0f, 0.016f, 3.0f, 1.0f), 3.0f, 1e-6f);
        // Non-positive dt holds the current value.
        EXPECT_NEAR(AutoExposure::AdaptLuminance(2.0f, 9.0f, 0.0f, 3.0f, 1.0f), 2.0f, 1e-6f);
    }

    // ----- EV100 / exposure --------------------------------------------------

    TEST(AutoExposureMathTest, DoublingLuminanceIsOneStopOfEV)
    {
        EXPECT_NEAR(AutoExposure::EV100FromLuminance(2.0f) - AutoExposure::EV100FromLuminance(1.0f), 1.0f, 1e-5f);
    }

    TEST(AutoExposureMathTest, BrighterSceneGivesSmallerExposure)
    {
        const f32 dim = AutoExposure::ComputeExposure(0.1f, 0.0f, 0.001f, 1000.0f);
        const f32 bright = AutoExposure::ComputeExposure(10.0f, 0.0f, 0.001f, 1000.0f);
        EXPECT_LT(bright, dim);
        // 100x brighter luminance => ~100x smaller exposure (linear in 1/L).
        EXPECT_NEAR(dim / bright, 100.0f, 1.0f);
    }

    TEST(AutoExposureMathTest, ExposureCompensationIsInStops)
    {
        const f32 base = AutoExposure::ComputeExposure(1.0f, 0.0f, 0.001f, 1000.0f);
        const f32 plusOne = AutoExposure::ComputeExposure(1.0f, 1.0f, 0.001f, 1000.0f);
        EXPECT_NEAR(plusOne / base, 2.0f, 1e-3f); // +1 EV doubles brightness
        const f32 minusOne = AutoExposure::ComputeExposure(1.0f, -1.0f, 0.001f, 1000.0f);
        EXPECT_NEAR(minusOne / base, 0.5f, 1e-3f);
    }

    TEST(AutoExposureMathTest, ExposureRespectsClamp)
    {
        // Extremely dark scene wants a huge exposure -> clamped to max.
        EXPECT_NEAR(AutoExposure::ComputeExposure(1e-6f, 0.0f, 0.05f, 8.0f), 8.0f, 1e-4f);
        // Extremely bright scene wants a tiny exposure -> clamped to min.
        EXPECT_NEAR(AutoExposure::ComputeExposure(1e6f, 0.0f, 0.05f, 8.0f), 0.05f, 1e-4f);
    }

    // ----- Full per-frame step ----------------------------------------------

    TEST(AutoExposureMathTest, StepDrivesExposureTowardSceneOverTime)
    {
        // A scene that is uniformly mid-grey (luminance 1.0).
        const auto h = MakeHistogram(std::vector<f32>(10000, 1.0f));

        f32 adapted = 0.0f; // uninitialised history -> first step snaps to target
        f32 exposure = 1.0f;
        for (int i = 0; i < 600; ++i) // ~10s at 60fps
        {
            const auto r = AutoExposure::Step(h, adapted, 1.0f / 60.0f,
                                              kMinLogLum, kMaxLogLum, 3.0f, 3.0f,
                                              0.0f, 0.001f, 1000.0f);
            adapted = r.AdaptedLuminance;
            exposure = r.Exposure;
        }

        // Adapted luminance settles at the scene luminance (~1.0).
        EXPECT_NEAR(std::log2(adapted), 0.0f, kBinWidthLog2 + 1e-2f);
        // Exposure matches the closed-form value for that luminance.
        const f32 expected = AutoExposure::ComputeExposure(adapted, 0.0f, 0.001f, 1000.0f);
        EXPECT_NEAR(exposure, expected, expected * 1e-4f);
    }
} // namespace OloEngine::Tests
