#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// FrameTimeTailTest — issue #1258, criterion 4.
//
// The cases here are shaped by the one thing this class exists to catch: a
// scheduler that improves the MEAN and worsens the TAIL. So the headline case
// is not "the percentiles are right on a uniform distribution" — every
// implementation passes that — it is a pair of distributions with the SAME mean
// where one stutters and the other does not, and the assertion is that the
// statistic tells them apart.
//
// The convention cases matter more than they look. `perf_trend.py` computes
// nearest-rank percentiles over the committed history, and this class computes
// them over live frames. If the two disagreed by an interpolation, an
// in-editor p95 and a CI trend p95 for the same run would differ by a frame and
// somebody would spend an afternoon on it.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Core/FrameTimeTail.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;

namespace
{
    [[nodiscard]] std::vector<f32> Sorted(std::vector<f32> values)
    {
        std::sort(values.begin(), values.end());
        return values;
    }
} // namespace

TEST(NearestRankPercentile, MatchesTheOfflineTrendScriptsDefinition)
{
    // perf_trend.py: ceil(q * n), 1-indexed, clamped. Spelled out as the
    // arithmetic rather than as expected constants, because the point of the
    // case is the CONVENTION and a hard-coded answer would not say which one.
    const std::vector<f32> data = Sorted({ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f });
    const sizet n = data.size();

    for (const f32 q : { 0.0f, 0.1f, 0.25f, 0.5f, 0.9f, 0.95f, 0.99f, 1.0f })
    {
        const f64 rank = std::ceil(static_cast<f64>(q) * static_cast<f64>(n));
        const sizet index = rank < 1.0 ? 0u : std::min(static_cast<sizet>(rank) - 1u, n - 1u);
        EXPECT_FLOAT_EQ(NearestRankPercentile(data, q), data[index]) << "q=" << q;
    }
}

TEST(NearestRankPercentile, EmptyAndOutOfRangeQuantilesReturnZeroRatherThanReadingPastTheEnd)
{
    const std::vector<f32> empty;
    EXPECT_FLOAT_EQ(NearestRankPercentile(empty, 0.5f), 0.0f);

    const std::vector<f32> data{ 1.0f, 2.0f };
    EXPECT_FLOAT_EQ(NearestRankPercentile(data, -0.1f), 0.0f);
    EXPECT_FLOAT_EQ(NearestRankPercentile(data, 1.1f), 0.0f);
    EXPECT_FLOAT_EQ(NearestRankPercentile(data, std::numeric_limits<f32>::quiet_NaN()), 0.0f);
}

TEST(FrameTimeTail, TheTailSeparatesTwoDistributionsWithTheSameMean)
{
    // THIS IS THE CASE THE CLASS EXISTS FOR. Both windows average 16.0 ms.
    // One is the frame this feature wants; the other is the frame a badly
    // phased amortisation produces — every animal's expensive update landing on
    // the same frame. A mean cannot tell them apart, and shipping on a mean is
    // how a stutter passes review with every number green.
    FrameTimeTail smooth(120u);
    FrameTimeTail spiky(120u);

    // EVERY FOURTH FRAME EXPENSIVE, not one frame in a hundred, and the shape
    // is chosen to match the real failure rather than to make a percentile
    // move. A badly phased amortisation does not produce one bad frame: it
    // produces a bad frame every N, because the whole population's reduced-rate
    // updates land on the same tick. That is what the deformation stagger in
    // ShouldPoseAnimalThisFrame exists to break up.
    //
    // (A single spike in 120 samples would NOT move p99 at all — nearest-rank
    // p99 of 120 is the 119th value — which is precisely why this class also
    // reports OverBudgetFrames and MaxMs. The case below pins that.)
    for (u32 i = 0; i < 120u; ++i)
    {
        smooth.Push(16.0f);
        spiky.Push((i % 4u) == 0u ? 40.0f : 8.0f);
    }

    const FrameTimeTailStats s = smooth.Query();
    const FrameTimeTailStats k = spiky.Query();

    EXPECT_NEAR(s.MeanMs, k.MeanMs, 0.05f) << "the case is only interesting while the means agree";
    EXPECT_NEAR(s.P50Ms, 16.0f, 1e-3f);
    EXPECT_NEAR(k.P50Ms, 8.0f, 1e-3f) << "the median of the stuttering window is its CHEAP frame — which is exactly "
                                         "how a stutter hides from every average anybody watches";

    EXPECT_NEAR(s.P99Ms, 16.0f, 1e-3f);
    EXPECT_NEAR(k.P99Ms, 40.0f, 1e-3f) << "the 99th percentile must see the spike the mean hid";
    EXPECT_GT(k.MaxMs, s.MaxMs * 2.0f);
}

TEST(FrameTimeTail, ASingleSpikeIsInvisibleToP99AndVisibleToMaxAndTheOverBudgetCount)
{
    // The limit of the percentile, stated rather than discovered later. Nearest
    // rank p99 of a 120-sample window is the 119th value, so ONE bad frame
    // cannot move it — and a reader who only watched p99 would conclude the
    // window was clean. MaxMs and OverBudgetFrames are what make that frame
    // visible, which is why this class reports all three.
    FrameTimeTail tail(120u);
    for (u32 i = 0; i < 120u; ++i)
    {
        tail.Push(i == 60u ? 135.0f : 15.0f);
    }

    const FrameTimeTailStats stats = tail.Query(16.6f);
    EXPECT_NEAR(stats.P99Ms, 15.0f, 1e-3f) << "one frame in 120 is below the 99th percentile's resolution";
    EXPECT_NEAR(stats.MaxMs, 135.0f, 1e-3f);
    EXPECT_EQ(stats.OverBudgetFrames, 1u);
}

TEST(FrameTimeTail, OverBudgetFramesCountsFramesRatherThanReportingAPercentile)
{
    // At a 120-sample window p99 is a single frame, so the percentile alone
    // cannot tell "one bad frame" from "six" — and six is a visible stutter
    // while one is not. The count is what makes that distinction available.
    FrameTimeTail one(120u);
    FrameTimeTail six(120u);
    for (u32 i = 0; i < 120u; ++i)
    {
        one.Push(i == 10u ? 40.0f : 15.0f);
        six.Push(i < 6u ? 40.0f : 15.0f);
    }

    EXPECT_EQ(one.Query(16.6f).OverBudgetFrames, 1u);
    EXPECT_EQ(six.Query(16.6f).OverBudgetFrames, 6u);
    EXPECT_FLOAT_EQ(one.Query(16.6f).BudgetMs, 16.6f);
    // A zero budget means "do not count", not "every frame is over".
    EXPECT_EQ(one.Query(0.0f).OverBudgetFrames, 0u);
}

TEST(FrameTimeTail, APartiallyFilledRingReportsOnlyTheSamplesItHas)
{
    // The failure this prevents: including the zero-filled remainder of the ring
    // puts p50 at zero, which reads as a spectacularly fast frame at exactly the
    // moment a window has just been reset.
    FrameTimeTail tail(600u);
    for (u32 i = 0; i < 40u; ++i)
    {
        tail.Push(20.0f);
    }

    const FrameTimeTailStats stats = tail.Query();
    EXPECT_EQ(stats.SampleCount, 40u);
    EXPECT_NEAR(stats.P50Ms, 20.0f, 1e-3f);
    EXPECT_NEAR(stats.MinMs, 20.0f, 1e-3f);
    EXPECT_NEAR(stats.MeanMs, 20.0f, 1e-3f);
}

TEST(FrameTimeTail, TheRingKeepsTheMostRecentWindowAndForgetsTheRest)
{
    FrameTimeTail tail(8u);
    for (u32 i = 0; i < 8u; ++i)
    {
        tail.Push(100.0f); // an old, slow era
    }
    for (u32 i = 0; i < 8u; ++i)
    {
        tail.Push(10.0f); // the present
    }

    const FrameTimeTailStats stats = tail.Query();
    EXPECT_EQ(stats.SampleCount, 8u);
    EXPECT_NEAR(stats.MaxMs, 10.0f, 1e-3f) << "a wrapped ring is still reporting frames that have scrolled off";
}

TEST(FrameTimeTail, ABrokenDeltaIsDroppedAndCountedRatherThanClampedToZero)
{
    // A zero substituted for a NaN would pull every percentile down, so a clock
    // fault would read as a performance win and the counter is the only thing
    // that would ever say otherwise.
    FrameTimeTail tail(16u);
    for (u32 i = 0; i < 8u; ++i)
    {
        tail.Push(20.0f);
    }
    tail.Push(std::numeric_limits<f32>::quiet_NaN());
    tail.Push(std::numeric_limits<f32>::infinity());
    tail.Push(-1.0f);

    const FrameTimeTailStats stats = tail.Query();
    EXPECT_EQ(stats.SampleCount, 8u);
    EXPECT_EQ(tail.GetRejectedSamples(), 3u);
    EXPECT_NEAR(stats.MinMs, 20.0f, 1e-3f);
    EXPECT_NEAR(stats.P50Ms, 20.0f, 1e-3f);
}

TEST(FrameTimeTail, ResetForgetsEverythingIncludingTheRejectionCount)
{
    FrameTimeTail tail(16u);
    tail.Push(20.0f);
    tail.Push(std::numeric_limits<f32>::quiet_NaN());
    ASSERT_EQ(tail.GetSampleCount(), 1u);
    ASSERT_EQ(tail.GetRejectedSamples(), 1u);

    tail.Reset();
    EXPECT_EQ(tail.GetSampleCount(), 0u);
    EXPECT_EQ(tail.GetRejectedSamples(), 0u);
    EXPECT_EQ(tail.Query().SampleCount, 0u);
}

TEST(FrameTimeTail, AnEmptyWindowReportsZeroesRatherThanReadingPastTheEnd)
{
    const FrameTimeTail tail(16u);
    const FrameTimeTailStats stats = tail.Query(16.6f);
    EXPECT_EQ(stats.SampleCount, 0u);
    EXPECT_FLOAT_EQ(stats.P99Ms, 0.0f);
    EXPECT_FLOAT_EQ(stats.MaxMs, 0.0f);
}

TEST(FrameTimeTail, ADegenerateCapacityIsClampedRatherThanCrashing)
{
    // Capacity arrives from settings, so zero is reachable and a modulo by zero
    // is not a wrong answer, it is a fault.
    FrameTimeTail tail(0u);
    EXPECT_GE(tail.GetCapacity(), 2u);
    tail.Push(16.0f);
    EXPECT_EQ(tail.Query().SampleCount, 1u);
}
