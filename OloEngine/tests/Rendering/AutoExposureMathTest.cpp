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
#include "OloEngine/Renderer/PostProcessSettings.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
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
        // Integer loop counter (S2193: no float counter); ev is computed fresh
        // each step so there's no accumulating float error.
        const int steps = static_cast<int>(std::round((kMaxLogLum - kMinLogLum) / 0.5f)) + 1;
        for (int i = 0; i < steps; ++i)
        {
            const f32 ev = kMinLogLum + 0.5f * static_cast<f32>(i);
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
        const int steps = static_cast<int>(std::round((kMaxLogLum - 0.5f - (kMinLogLum + 0.5f)) / 0.7f)) + 1;
        for (int i = 0; i < steps; ++i)
        {
            const f32 ev = kMinLogLum + 0.5f + 0.7f * static_cast<f32>(i);
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

    // ----- Percentile-banded metering ---------------------------------------
    // The band (UE EyeAdaptation Low/HighPercent style) keys exposure on the
    // brightest coherent population. Two concrete regressions pinned here,
    // both from the VehiclesTest water scene (issue #438 follow-up): a slight
    // camera tilt toward the horizon added a broad dark far-water population
    // and the naive mean dropped so hard the whole frame washed out (2.4x
    // display-brightness swing over 10 degrees of pitch), and the compact
    // sun-glint sparkle population steered exposure whenever it entered the
    // frame.

    TEST(AutoExposureMathTest, PercentileDefaultsReproduceThePlainMean)
    {
        const f32 dark = std::exp2(-3.0f);
        const f32 lite = std::exp2(2.0f);
        std::vector<f32> px(5000, dark);
        px.insert(px.end(), 5000, lite);
        const auto h = MakeHistogram(px);
        const f32 plain = AutoExposure::ComputeAverageLuminance(h, kMinLogLum, kMaxLogLum);
        const f32 explicitFull = AutoExposure::ComputeAverageLuminance(h, kMinLogLum, kMaxLogLum, 0.0f, 1.0f);
        EXPECT_EQ(plain, explicitFull) << "default arguments must be the plain weighted mean";
        // A degenerate band falls back to the plain mean instead of dividing by zero.
        const f32 degenerate = AutoExposure::ComputeAverageLuminance(h, kMinLogLum, kMaxLogLum, 0.5f, 0.5f);
        EXPECT_EQ(plain, degenerate);
    }

    TEST(AutoExposureMathTest, BandIgnoresACompactBrightGlint)
    {
        const f32 waterLum = std::exp2(-1.0f);
        std::vector<f32> water(100000, waterLum);
        const auto plainWater = AutoExposure::ComputeAverageLuminance(MakeHistogram(water), kMinLogLum, kMaxLogLum, 0.80f, 0.98f);

        // Add a 1.5% ultra-bright glint population (sun sparkle).
        auto withGlint = water;
        withGlint.insert(withGlint.end(), 1500, std::exp2(3.0f));
        const auto banded = AutoExposure::ComputeAverageLuminance(MakeHistogram(withGlint), kMinLogLum, kMaxLogLum, 0.80f, 0.98f);
        const auto naive = AutoExposure::ComputeAverageLuminance(MakeHistogram(withGlint), kMinLogLum, kMaxLogLum);

        // The banded average must not move (the glint sits above the 98th
        // percentile); the naive mean visibly does.
        EXPECT_NEAR(std::log2(banded), std::log2(plainWater), 0.05f)
            << "a compact glint above the high percentile must not steer the metered average";
        EXPECT_GT(std::log2(naive), std::log2(plainWater) + 0.05f)
            << "sanity: the naive mean must actually shift for this histogram, or this test is vacuous";
    }

    TEST(AutoExposureMathTest, BandStaysStableWhenABroadDarkPopulationEnters)
    {
        // Bright near-water fills the frame...
        const f32 nearLum = std::exp2(0.0f);
        std::vector<f32> nearOnly(100000, nearLum);
        const f32 before = AutoExposure::ComputeAverageLuminance(MakeHistogram(nearOnly), kMinLogLum, kMaxLogLum, 0.80f, 0.98f);

        // ...then the camera tilts up and 60% of the frame becomes much
        // darker far water. The 80th..98th percentile band still lands inside
        // the bright population (which now occupies the top 40%), so the
        // metered average must barely move; the naive mean collapses.
        auto tilted = nearOnly;
        tilted.insert(tilted.end(), 150000, std::exp2(-4.0f));
        const f32 bandedAfter = AutoExposure::ComputeAverageLuminance(MakeHistogram(tilted), kMinLogLum, kMaxLogLum, 0.80f, 0.98f);
        const f32 naiveAfter = AutoExposure::ComputeAverageLuminance(MakeHistogram(tilted), kMinLogLum, kMaxLogLum);

        EXPECT_NEAR(std::log2(bandedAfter), std::log2(before), 0.05f)
            << "a broad dark population below the band must not steer the metered average";
        EXPECT_LT(std::log2(naiveAfter), std::log2(before) - 1.0f)
            << "sanity: the naive mean must collapse for this histogram, or this test is vacuous";
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
            const f32 target = AutoExposure::ComputeAverageLuminance(h, kMinLogLum, kMaxLogLum);
            adapted = AutoExposure::AdaptLuminance(adapted, target, 1.0f / 60.0f, 3.0f, 3.0f);
            exposure = AutoExposure::ComputeExposure(adapted, 0.0f, 0.001f, 1000.0f);
        }

        // Adapted luminance settles at the scene luminance (~1.0).
        EXPECT_NEAR(std::log2(adapted), 0.0f, kBinWidthLog2 + 1e-2f);
        // Exposure matches the closed-form value for that luminance.
        const f32 expected = AutoExposure::ComputeExposure(adapted, 0.0f, 0.001f, 1000.0f);
        EXPECT_NEAR(exposure, expected, expected * 1e-4f);
    }

    // ----- Settings sanitization (load-time validation) ----------------------

    TEST(AutoExposureMathTest, SanitizeReplacesNonFiniteWithFiniteDefaults)
    {
        PostProcessSettings s;
        s.AutoExposureMinLogLuminance = std::nanf("");
        s.AutoExposureSpeedUp = std::numeric_limits<f32>::infinity();
        s.AutoExposureSpeedDown = -std::numeric_limits<f32>::infinity();
        s.AutoExposureMaxExposure = std::nanf("");
        SanitizeAutoExposure(s);
        EXPECT_TRUE(std::isfinite(s.AutoExposureMinLogLuminance));
        EXPECT_TRUE(std::isfinite(s.AutoExposureSpeedUp));
        EXPECT_TRUE(std::isfinite(s.AutoExposureSpeedDown));
        EXPECT_TRUE(std::isfinite(s.AutoExposureMaxExposure));
        EXPECT_GE(s.AutoExposureSpeedUp, 0.0f); // speeds clamped non-negative
        EXPECT_GE(s.AutoExposureSpeedDown, 0.0f);
    }

    TEST(AutoExposureMathTest, SanitizeOrdersInvertedMinMaxPairs)
    {
        PostProcessSettings s;
        s.AutoExposureMinLogLuminance = 5.0f; // inverted vs max
        s.AutoExposureMaxLogLuminance = -2.0f;
        s.AutoExposureMinExposure = 8.0f; // inverted vs max
        s.AutoExposureMaxExposure = 0.1f;
        SanitizeAutoExposure(s);
        EXPECT_LE(s.AutoExposureMinLogLuminance, s.AutoExposureMaxLogLuminance);
        EXPECT_LE(s.AutoExposureMinExposure, s.AutoExposureMaxExposure);
    }
} // namespace OloEngine::Tests
