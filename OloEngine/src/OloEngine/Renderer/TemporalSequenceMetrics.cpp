#include "OloEnginePCH.h"
#include "OloEngine/Renderer/TemporalSequenceMetrics.h"

#include <cmath>

namespace OloEngine::TemporalSequenceMetrics
{
    namespace
    {
        /// Every field in a sequence must be the same non-empty size, or the
        /// metrics below would compare pixel i of one resolution against
        /// pixel i of another and return a number that means nothing. A
        /// ragged sequence is a caller bug, so it returns "no frames
        /// compared" rather than a plausible value.
        [[nodiscard]] bool SequenceIsWellFormed(std::span<const std::vector<f32>> frames)
        {
            if (frames.size() < 2u || frames.front().empty())
                return false;
            const auto expected = frames.front().size();
            for (const auto& frame : frames)
            {
                if (frame.size() != expected)
                    return false;
            }
            return true;
        }

        /// A pixel counts as active when either side carries signal there.
        [[nodiscard]] bool Active(f32 lhs, f32 rhs)
        {
            return std::abs(lhs) > kActivityEpsilon || std::abs(rhs) > kActivityEpsilon;
        }

        /// Mean |lhs - rhs| over the pixels either touches, and the largest
        /// single-pixel difference. A non-finite pixel on either side is
        /// skipped rather than poisoning the mean with a NaN — a sequence
        /// with one bad texel must still report the other million honestly.
        struct FieldDifference
        {
            f64 Mean = 0.0;
            f64 Peak = 0.0;
            u32 ComparedPixels = 0u;
        };

        [[nodiscard]] FieldDifference CompareFields(const std::vector<f32>& lhs, const std::vector<f32>& rhs)
        {
            FieldDifference difference{};
            if (lhs.size() != rhs.size())
                return difference;

            f64 total = 0.0;
            for (std::size_t i = 0u; i < lhs.size(); ++i)
            {
                if (!std::isfinite(lhs[i]) || !std::isfinite(rhs[i]) || !Active(lhs[i], rhs[i]))
                    continue;
                const f64 delta = std::abs(static_cast<f64>(lhs[i]) - static_cast<f64>(rhs[i]));
                total += delta;
                difference.Peak = std::max(difference.Peak, delta);
                ++difference.ComparedPixels;
            }
            if (difference.ComparedPixels > 0u)
                difference.Mean = total / static_cast<f64>(difference.ComparedPixels);
            return difference;
        }
    } // namespace

    ShimmerResult MeasureShimmer(std::span<const std::vector<f32>> frames)
    {
        ShimmerResult result{};
        if (!SequenceIsWellFormed(frames))
            return result;

        f64 total = 0.0;
        f64 firstDelta = 0.0;
        f64 lastDelta = 0.0;
        for (std::size_t i = 1u; i < frames.size(); ++i)
        {
            const FieldDifference difference = CompareFields(frames[i], frames[i - 1u]);
            total += difference.Mean;
            result.MaxFrameDelta = std::max(result.MaxFrameDelta, difference.Mean);
            result.PeakPixelDelta = std::max(result.PeakPixelDelta, difference.Peak);
            if (i == 1u)
                firstDelta = difference.Mean;
            lastDelta = difference.Mean;
            result.ComparedPixels = difference.ComparedPixels;
            ++result.FramesCompared;
        }

        if (result.FramesCompared > 0u)
            result.MeanFrameDelta = total / static_cast<f64>(result.FramesCompared);

        // A sequence that starts perfectly still has nothing to converge
        // from, and dividing by its zero first delta would report an
        // infinity that reads as "diverging". Report 0 — "already settled" —
        // which is what a first delta of zero actually means.
        result.ConvergenceRatio = firstDelta > 0.0 ? lastDelta / firstDelta : 0.0;
        return result;
    }

    GhostingResult MeasureGhosting(std::span<const std::vector<f32>> frames, const std::vector<f32>& target,
                                   f64 tolerance)
    {
        GhostingResult result{};
        if (frames.empty() || target.empty())
            return result;

        // Walk the sequence backwards to find the first frame from which
        // EVERY later frame is within tolerance. Scanning forwards for the
        // first frame under tolerance would report a resolve that dips
        // through the target and comes back out as settled on the dip —
        // which is precisely what an overshooting history does.
        std::vector<f64> residuals;
        residuals.reserve(frames.size());
        for (const auto& frame : frames)
        {
            if (frame.size() != target.size())
                return result;
            const FieldDifference difference = CompareFields(frame, target);
            residuals.push_back(difference.Mean);
            result.PeakResidual = std::max(result.PeakResidual, difference.Mean);
            result.ResidualArea += difference.Mean;
            result.ComparedPixels = difference.ComparedPixels;
            ++result.FramesEvaluated;
        }

        result.FinalResidual = residuals.back();

        result.SettlingFrames = kNeverSettled;
        for (std::size_t i = residuals.size(); i-- > 0u;)
        {
            if (residuals[i] > tolerance)
                break;
            result.SettlingFrames = static_cast<u32>(i);
        }
        return result;
    }

    DetailResult MeasureDetail(const std::vector<f32>& measured, const std::vector<f32>& reference)
    {
        DetailResult result{};
        if (measured.size() != reference.size() || measured.empty())
            return result;

        // One pass to collect the active set, so both variances are taken
        // over the SAME pixels. Taking each over its own active set would
        // compare a variance of the lit part against a variance of the whole
        // frame, and the ratio would mean nothing.
        f64 measuredSum = 0.0;
        f64 measuredSquares = 0.0;
        f64 referenceSum = 0.0;
        f64 referenceSquares = 0.0;
        f64 absoluteError = 0.0;
        for (std::size_t i = 0u; i < measured.size(); ++i)
        {
            if (!std::isfinite(measured[i]) || !std::isfinite(reference[i]) || !Active(measured[i], reference[i]))
                continue;
            const f64 m = static_cast<f64>(measured[i]);
            const f64 r = static_cast<f64>(reference[i]);
            measuredSum += m;
            measuredSquares += m * m;
            referenceSum += r;
            referenceSquares += r * r;
            absoluteError += std::abs(m - r);
            ++result.ComparedPixels;
        }

        if (result.ComparedPixels == 0u)
        {
            // Nothing was comparable, so nothing was lost. Reported as 1
            // rather than 0 for the same reason the flat-reference case
            // below is: 0 would claim the measurement destroyed detail that
            // was never there. ComparedPixels is how a caller tells an empty
            // capture from a genuine match.
            result.RetainedFraction = 1.0;
            return result;
        }

        const f64 count = static_cast<f64>(result.ComparedPixels);
        const f64 measuredMean = measuredSum / count;
        const f64 referenceMean = referenceSum / count;
        result.MeasuredVariance = std::max(measuredSquares / count - measuredMean * measuredMean, 0.0);
        result.ReferenceVariance = std::max(referenceSquares / count - referenceMean * referenceMean, 0.0);
        result.MeanAbsoluteError = absoluteError / count;

        // A flat reference has no detail to retain, so the ratio is
        // undefined rather than infinite. 0 would claim the resolve destroyed
        // detail that was never there; 1 is the honest answer — nothing was
        // lost, because there was nothing to lose.
        result.RetainedFraction =
            result.ReferenceVariance > 0.0 ? result.MeasuredVariance / result.ReferenceVariance : 1.0;
        return result;
    }
} // namespace OloEngine::TemporalSequenceMetrics
