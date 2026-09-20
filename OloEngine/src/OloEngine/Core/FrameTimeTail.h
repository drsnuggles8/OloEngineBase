#pragma once

// =============================================================================
// FrameTimeTail.h — frame-time distribution, with the TAIL as the headline.
// Issue #1258, criterion 4.
//
// WHY THIS EXISTS AT ALL. Nothing in this engine measured a frame-time
// percentile before this file. `PerformanceProfiler` keeps one frame's totals,
// `GPUPassTimerPool` keeps one frame's per-pass GPU cost, the Layer-6
// microbenchmarks take a MINIMUM of twenty samples, and `perf_trend.py`
// computes a p95 offline over committed history. Every one of those is blind to
// the thing a scheduler introduces.
//
// A SCHEDULER'S CHARACTERISTIC FAILURE IS INVISIBLE IN A MEAN. Amortising work
// across frames does not reduce the work; it moves it. Done well the mean falls
// and the tail falls with it. Done badly — every animal's expensive frame
// landing on the same frame — the mean falls and the 99th percentile gets
// WORSE, and the picture stutters while every number anyone was watching
// improved. So this reports p50/p95/p99/max, and the acceptance evidence is the
// tail, not the average.
//
// THE MINIMUM-OF-N THE PERF SUITE USES IS THE OPPOSITE MEASUREMENT, and that is
// not a criticism of it. A microbenchmark wants the machine's best effort with
// the scheduler noise removed, so it takes the minimum. A frame-pacing question
// wants exactly the noise, so it takes the tail. Both are right for their
// question; using either for the other's is how a stutter ships green.
//
// NEAREST-RANK, NOT INTERPOLATED, matching perf_trend.py::percentile so the
// in-engine number and the offline trend number are the same statistic. Two
// percentile conventions that differ by an interpolation are the kind of
// discrepancy that gets argued about for an afternoon.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <array>
#include <span>
#include <vector>

namespace OloEngine
{

    /// One window's distribution, in milliseconds.
    struct FrameTimeTailStats
    {
        u32 SampleCount = 0u;
        f32 MinMs = 0.0f;
        f32 MeanMs = 0.0f;
        f32 P50Ms = 0.0f;
        f32 P95Ms = 0.0f;
        f32 P99Ms = 0.0f;
        f32 MaxMs = 0.0f;

        /// Frames over the pacing target. A COUNT beside the percentiles rather
        /// than only a percentile, because at a 120-sample window p99 is one
        /// frame: the percentile alone cannot tell "one bad frame" from "six",
        /// and six is a visible stutter while one is not.
        u32 OverBudgetFrames = 0u;

        /// The target those were counted against, carried so a reader never has
        /// to guess which budget a count refers to.
        f32 BudgetMs = 0.0f;
    };

    // A fixed-capacity ring of recent frame times.
    //
    // FIXED CAPACITY AND NO ALLOCATION AFTER CONSTRUCTION: this is written to
    // once per frame from the frame loop, and a tracker that reallocated would
    // be measuring itself. The percentile query copies and sorts, so it is the
    // QUERY that costs — call it from a panel or a test, not from the loop.
    class FrameTimeTail
    {
      public:
        /// 600 frames is ten seconds at 60 Hz, which is the shortest window in
        /// which a 99th percentile means anything at all: at 120 samples p99 is
        /// a single frame and the statistic is just the second-worst frame
        /// wearing a percentile's name.
        static constexpr u32 kDefaultCapacity = 600u;

        explicit FrameTimeTail(u32 capacity = kDefaultCapacity);

        /// Record one frame. A non-finite or negative sample is DROPPED and
        /// counted, never clamped to zero: a zero frame time would pull every
        /// percentile down and make a broken clock look like a fast frame.
        void Push(f32 frameMs);

        /// Forget every sample. Called across a discontinuity — a scene load, a
        /// play-mode transition, a resolution change — for the reason
        /// SkeletalDeformationSystem::ResetHistory exists: a window that spans
        /// two different configurations describes neither.
        void Reset();

        /// The distribution over the samples held right now.
        ///
        /// `budgetMs` is the pacing target OverBudgetFrames is counted against;
        /// pass 0 to skip that count. Sorts a copy, so this is O(n log n) and
        /// belongs in a panel, a capture or a test rather than in the frame
        /// loop.
        [[nodiscard]] FrameTimeTailStats Query(f32 budgetMs = 0.0f) const;

        [[nodiscard]] u32 GetSampleCount() const noexcept
        {
            return m_Count;
        }
        [[nodiscard]] u32 GetCapacity() const noexcept
        {
            return static_cast<u32>(m_Samples.size());
        }
        /// Samples refused as non-finite or negative. Non-zero means something
        /// upstream is handing out a broken delta, which is worth knowing before
        /// reading any percentile below it.
        [[nodiscard]] u32 GetRejectedSamples() const noexcept
        {
            return m_Rejected;
        }

      private:
        std::vector<f32> m_Samples;
        u32 m_Head = 0u;
        u32 m_Count = 0u;
        u32 m_Rejected = 0u;
    };

    /// The nearest-rank percentile of an ALREADY SORTED ascending range.
    ///
    /// Nearest-rank: `ceil(q * n)`-th value, 1-indexed, clamped into range —
    /// the same definition as `OloEngine/tests/scripts/perf_trend.py`. Exposed
    /// because the census and the capture both need it over data this class
    /// never held, and two copies of a percentile convention drift.
    ///
    /// `q` outside [0, 1] and an empty range both return 0.
    [[nodiscard]] f32 NearestRankPercentile(std::span<const f32> sortedAscending, f32 q) noexcept;

} // namespace OloEngine
