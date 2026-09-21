#pragma once

// =============================================================================
// TemporalSequenceMetrics.h — ghosting, shimmer and detail loss as NUMBERS
// over a frame sequence (issue #1256).
//
// WHY THIS EXISTS. A temporal resolve cannot be judged from a settled
// screenshot. Two settled captures are equally settled whatever the
// accumulator is doing, so a resolve that ghosts, sparkles or has quietly
// blurred away half the detail passes every still comparison in the suite.
// #1256's third acceptance criterion says so outright: static accumulated
// captures are insufficient. What separates a fix from a wash is a
// measurement taken ACROSS frames, and that is what this header is.
//
// The three defects are three different questions and get three different
// instruments — collapsing them into one "temporal error" number is how a fix
// that trades shimmer for ghosting reads as an improvement:
//
//   * SHIMMER is movement where there should be none. Measured on a STATIC
//     sequence as the mean frame-to-frame difference. A correct resolve
//     converges, so this decays toward zero; sparkle and crawl keep it up.
//   * GHOSTING is history that will not let go. Measured on a sequence that
//     STEPS to a new steady state: how long the output takes to get there,
//     and how much stale signal it drags along on the way.
//   * DETAIL LOSS is the price paid for both. Measured as the spatial
//     variance the output retains against a reference that was never
//     temporally filtered. A resolve can beat shimmer and ghosting by
//     blurring everything to a flat field, and this is the number that
//     catches it doing so.
//
// It is CPU-only over plain scalar fields and needs no GL context, which is
// what lets the six minimal reproductions #1256 asks for run headless, under
// mock time, in CI, on every change. A live editor cannot supply this: 69 % of
// pixels move between two captures of the same windy scene, so a live pixel
// A/B has no signal to measure at all.
//
// A "frame" here is a row-major scalar field — luma, coverage, visibility,
// whatever AOV the caller is judging. Scalar rather than RGB on purpose: all
// three questions are about one channel's behaviour over time, and a caller
// judging colour should ask them per channel rather than average a hue.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <limits>
#include <span>
#include <vector>

namespace OloEngine::TemporalSequenceMetrics
{
    /// Returned by MeasureGhosting when the sequence never reached tolerance.
    inline constexpr u32 kNeverSettled = std::numeric_limits<u32>::max();

    /// Pixels neither the frame nor the reference touches are excluded from
    /// every metric below. Including them lets a larger empty border drive
    /// every number toward zero and makes two resolutions incomparable — the
    /// same choice, for the same reason, as GroomCoverage::CompareCoverage.
    inline constexpr f32 kActivityEpsilon = 1.0e-6f;

    struct ShimmerResult
    {
        /// Mean |f(t) - f(t-1)| over active pixels, averaged over the
        /// sequence. THE headline number: a converged resolve drives it to
        /// zero, sparkle and crawl hold it up.
        f64 MeanFrameDelta = 0.0;
        /// The largest single frame-pair mean — one bad frame, which a mean
        /// over a long sequence averages away.
        f64 MaxFrameDelta = 0.0;
        /// The largest single-PIXEL change anywhere in the sequence. A
        /// resolve can have a tiny mean and still pop one strand violently.
        f64 PeakPixelDelta = 0.0;
        /// Ratio of the last frame-pair delta to the first. Below 1 the
        /// sequence is converging; at or above 1 it is not settling at all,
        /// which is a different defect from simply being noisy.
        f64 ConvergenceRatio = 0.0;
        u32 FramesCompared = 0;
        /// Active pixels the LAST frame pair was measured over. Zero means
        /// every pixel was skipped — an all-NaN or entirely empty sequence —
        /// in which case MeanFrameDelta is 0 because nothing was compared,
        /// not because nothing moved. A caller that does not check this
        /// cannot tell a converged resolve from a blank capture, which is
        /// the failure "a difference assertion cannot catch an empty frame"
        /// names.
        u32 ComparedPixels = 0;
    };

    /// `frames` is a sequence of equally-sized fields captured under a STATIC
    /// configuration — no camera motion, no LOD change. Fewer than two
    /// frames, or ragged sizes, return a zeroed result with FramesCompared 0
    /// rather than a plausible number computed from nothing.
    [[nodiscard]] ShimmerResult MeasureShimmer(std::span<const std::vector<f32>> frames);

    struct GhostingResult
    {
        /// Frames after the step before the output first came within
        /// `tolerance` of `target` AND stayed there. kNeverSettled if it
        /// never did. The headline number.
        u32 SettlingFrames = kNeverSettled;
        /// Largest mean residual |f(t) - target| seen after the step.
        f64 PeakResidual = 0.0;
        /// Sum of the mean residual over every frame — the "trail". Two
        /// resolves can settle on the same frame and drag very different
        /// amounts of stale signal getting there, and this is what separates
        /// them. Directly comparable only between runs of the same length.
        f64 ResidualArea = 0.0;
        /// Residual of the final frame. A resolve that settles to the WRONG
        /// value has a small SettlingFrames only because it never got close;
        /// this is what catches that.
        f64 FinalResidual = 0.0;
        u32 FramesEvaluated = 0;
        /// Active pixels the FINAL frame's residual was measured over. Zero
        /// means nothing was compared, so a `SettlingFrames` of 0 says the
        /// capture was empty rather than that the resolve settled instantly.
        u32 ComparedPixels = 0;
    };

    /// `frames` is the sequence captured AFTER the step; `target` is the
    /// steady state it should reach — in practice a cold-history render of
    /// the post-step configuration, captured in the same run rather than
    /// hard-coded, so the assertion is a comparison between two measurements.
    ///
    /// `tolerance` is a mean-residual threshold in the field's own units.
    [[nodiscard]] GhostingResult MeasureGhosting(std::span<const std::vector<f32>> frames,
                                                 const std::vector<f32>& target, f64 tolerance);

    struct DetailResult
    {
        /// Spatial variance of the reference over active pixels.
        f64 ReferenceVariance = 0.0;
        /// Spatial variance of the measured field.
        f64 MeasuredVariance = 0.0;
        /// MeasuredVariance / ReferenceVariance, and 1 when nothing was
        /// comparable — consistent with the flat-reference case below,
        /// because "no detail was lost" is the honest answer when there was
        /// no detail to lose. `ComparedPixels == 0` is how a caller tells
        /// that apart from a genuine match. 1 keeps every bit of
        /// detail; below 1 the resolve has blurred. ABOVE 1 is not "extra
        /// detail" — it is the resolve adding variance the reference never
        /// had, which is sharpening overshoot or noise, and is reported
        /// honestly rather than clamped so a caller can tell the two apart.
        f64 RetainedFraction = 0.0;
        /// Mean |measured - reference|, so a field that kept its variance by
        /// moving every pixel the wrong way is still visible as wrong.
        f64 MeanAbsoluteError = 0.0;
        u32 ComparedPixels = 0;
    };

    /// `reference` is a field that was never temporally filtered — the
    /// un-resolved current frame, or a supersampled ground truth.
    [[nodiscard]] DetailResult MeasureDetail(const std::vector<f32>& measured, const std::vector<f32>& reference);
} // namespace OloEngine::TemporalSequenceMetrics
