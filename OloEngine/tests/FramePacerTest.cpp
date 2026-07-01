// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/FramePacer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <cmath>
#include <limits>

namespace
{
    using namespace OloEngine;

    constexpr f32 kInf = std::numeric_limits<f32>::infinity();
    const f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

    // =========================================================================
    // ComputeSleepSeconds — pure sleep/spin split math
    // =========================================================================

    TEST(FramePacerComputeSleep, LeavesTheSpinMarginToBusyWait)
    {
        // 5 ms remaining, 1 ms spin margin -> coarse-sleep 4 ms, spin the rest.
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(0.005f, 0.001f), 0.004f);
    }

    TEST(FramePacerComputeSleep, ReturnsZeroWhenWithinTheMargin)
    {
        // Remaining <= margin -> spin only, no coarse sleep.
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(0.001f, 0.001f), 0.0f);
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(0.0005f, 0.001f), 0.0f);
    }

    TEST(FramePacerComputeSleep, ReturnsZeroWhenBudgetAlreadySpent)
    {
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(0.0f, 0.001f), 0.0f);
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(-0.010f, 0.001f), 0.0f);
    }

    TEST(FramePacerComputeSleep, TreatsNonFiniteAsZero)
    {
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(kNaN, 0.001f), 0.0f);
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(kInf, 0.001f), 0.0f); // non-finite remaining -> no sleep
        // A bad margin is treated as 0 -> sleep the whole remaining time.
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(0.005f, kNaN), 0.005f);
        EXPECT_FLOAT_EQ(FramePacer::ComputeSleepSeconds(0.005f, -1.0f), 0.005f);
    }

    // =========================================================================
    // SmoothFrameTime — pure single EMA step
    // =========================================================================

    TEST(FramePacerSmoothStep, AlphaOneAdoptsTheSample)
    {
        EXPECT_FLOAT_EQ(FramePacer::SmoothFrameTime(0.010f, 0.020f, 1.0f), 0.020f);
    }

    TEST(FramePacerSmoothStep, AlphaZeroKeepsThePrevious)
    {
        EXPECT_FLOAT_EQ(FramePacer::SmoothFrameTime(0.010f, 0.020f, 0.0f), 0.010f);
    }

    TEST(FramePacerSmoothStep, BlendsProportionally)
    {
        // previous + alpha*(sample - previous) = 0.010 + 0.25*0.010 = 0.0125
        EXPECT_FLOAT_EQ(FramePacer::SmoothFrameTime(0.010f, 0.020f, 0.25f), 0.0125f);
    }

    TEST(FramePacerSmoothStep, SanitizesNonFiniteInputs)
    {
        EXPECT_FLOAT_EQ(FramePacer::SmoothFrameTime(0.010f, kNaN, 0.5f), 0.010f);   // bad sample -> keep prev
        EXPECT_FLOAT_EQ(FramePacer::SmoothFrameTime(kInf, 0.020f, 0.5f), 0.020f);   // bad prev -> adopt sample
        EXPECT_FLOAT_EQ(FramePacer::SmoothFrameTime(0.010f, 0.020f, kNaN), 0.020f); // bad alpha -> treat as 1.0
    }

    TEST(FramePacerSmoothStep, ConvergesTowardAConstantSample)
    {
        f32 value = 0.0f;
        for (int i = 0; i < 200; ++i)
        {
            value = FramePacer::SmoothFrameTime(value, 0.016f, 0.2f);
        }
        EXPECT_NEAR(value, 0.016f, 1e-4f);
    }

    // =========================================================================
    // Frame-rate cap accessors
    // =========================================================================

    TEST(FramePacerCap, UncappedByDefault)
    {
        FramePacer pacer;
        EXPECT_EQ(pacer.GetTargetFPS(), FramePacer::kUncapped);
        EXPECT_FALSE(pacer.IsCapEnabled());
        EXPECT_FLOAT_EQ(pacer.GetTargetFrameTime(), 0.0f);
    }

    TEST(FramePacerCap, TargetFrameTimeIsReciprocalOfFPS)
    {
        FramePacer pacer;
        pacer.SetTargetFPS(60);
        EXPECT_TRUE(pacer.IsCapEnabled());
        EXPECT_FLOAT_EQ(pacer.GetTargetFrameTime(), 1.0f / 60.0f);

        pacer.SetTargetFPS(144);
        EXPECT_FLOAT_EQ(pacer.GetTargetFrameTime(), 1.0f / 144.0f);

        pacer.SetTargetFPS(FramePacer::kUncapped);
        EXPECT_FALSE(pacer.IsCapEnabled());
        EXPECT_FLOAT_EQ(pacer.GetTargetFrameTime(), 0.0f);
    }

    // =========================================================================
    // Smoothing factor + stateful SmoothDelta
    // =========================================================================

    TEST(FramePacerSmoothing, DisabledByDefault)
    {
        FramePacer pacer;
        EXPECT_FLOAT_EQ(pacer.GetSmoothingFactor(), 1.0f);
        EXPECT_FALSE(pacer.IsSmoothingEnabled());
    }

    TEST(FramePacerSmoothing, SetterClampsToValidRange)
    {
        FramePacer pacer;
        pacer.SetSmoothingFactor(0.3f);
        EXPECT_FLOAT_EQ(pacer.GetSmoothingFactor(), 0.3f);
        EXPECT_TRUE(pacer.IsSmoothingEnabled());

        pacer.SetSmoothingFactor(5.0f); // above 1 -> clamps to 1
        EXPECT_FLOAT_EQ(pacer.GetSmoothingFactor(), 1.0f);

        pacer.SetSmoothingFactor(0.0f); // 0 would freeze the average -> floored
        EXPECT_GT(pacer.GetSmoothingFactor(), 0.0f);

        pacer.SetSmoothingFactor(kNaN); // non-finite -> disabled (1.0)
        EXPECT_FLOAT_EQ(pacer.GetSmoothingFactor(), 1.0f);
    }

    TEST(FramePacerSmoothing, FirstCallSeedsWithTheRawDelta)
    {
        FramePacer pacer;
        pacer.SetSmoothingFactor(0.1f);
        EXPECT_FLOAT_EQ(pacer.SmoothDelta(0.02f), 0.02f);
        EXPECT_FLOAT_EQ(pacer.GetSmoothedDelta(), 0.02f);
    }

    TEST(FramePacerSmoothing, DisabledSmoothingIsAnIdentityPassThrough)
    {
        FramePacer pacer; // alpha == 1.0
        EXPECT_FLOAT_EQ(pacer.SmoothDelta(0.016f), 0.016f);
        EXPECT_FLOAT_EQ(pacer.SmoothDelta(0.033f), 0.033f);
        EXPECT_FLOAT_EQ(pacer.SmoothDelta(0.008f), 0.008f);
    }

    TEST(FramePacerSmoothing, DampsAJitterSpike)
    {
        FramePacer pacer;
        pacer.SetSmoothingFactor(0.2f);
        pacer.SmoothDelta(0.016f); // seed at 16 ms

        // A single doubled frame should barely move the smoothed value.
        const f32 afterSpike = pacer.SmoothDelta(0.032f);
        EXPECT_GT(afterSpike, 0.016f);
        EXPECT_LT(afterSpike, 0.020f); // far below the raw 32 ms spike
    }

    TEST(FramePacerSmoothing, IgnoresNonFiniteSamples)
    {
        FramePacer pacer;
        pacer.SetSmoothingFactor(0.5f);
        pacer.SmoothDelta(0.016f);
        const f32 kept = pacer.SmoothDelta(kNaN);
        EXPECT_FLOAT_EQ(kept, 0.016f);
        EXPECT_FLOAT_EQ(pacer.GetSmoothedDelta(), 0.016f);
    }

    TEST(FramePacerSmoothing, ResetReseedsOnNextSample)
    {
        FramePacer pacer;
        pacer.SetSmoothingFactor(0.2f);
        pacer.SmoothDelta(0.016f);
        pacer.SmoothDelta(0.016f);

        pacer.Reset();
        // After a reset the next sample seeds directly instead of blending.
        EXPECT_FLOAT_EQ(pacer.SmoothDelta(0.100f), 0.100f);
    }

    // =========================================================================
    // LimitFrameRate — no-op paths (never block the deterministic test run)
    // =========================================================================

    TEST(FramePacerLimit, UncappedReturnsImmediately)
    {
        FramePacer pacer; // uncapped
        const f32 before = Time::GetTime();
        pacer.LimitFrameRate(before);
        // No cap -> should not have paced anything meaningfully.
        EXPECT_LT(Time::GetTime() - before, 0.05f);
    }

    TEST(FramePacerLimit, FrozenClockDoesNotHang)
    {
        // A mock (frozen) clock never advances; the limiter must bail instead of
        // spinning forever waiting for elapsed time that can't accrue.
        Time::SetMockTime(123.0f);
        {
            FramePacer pacer;
            pacer.SetTargetFPS(60);
            pacer.LimitFrameRate(123.0f); // returns immediately by contract
            SUCCEED();
        }
        Time::ClearMockTime();
    }
} // namespace
