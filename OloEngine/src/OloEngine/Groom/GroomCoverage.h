#pragma once

// =============================================================================
// GroomCoverage.h — the measured comparison behind #1246's acceptance
// criterion 1, and the arithmetic the GPU strand shader has to agree with.
//
// WHAT THIS IS FOR. Criterion 1 asks for a bounded comparison of composition
// approaches on measured evidence — near silhouette, dense overlap, sub-pixel
// coverage — before an approach is chosen. A screenshot cannot settle that:
// every approach produces a plausible picture of hair, and the differences
// live in the fraction of a pixel a strand covers. So the comparison is done
// here, in arithmetic, against a ground truth this file also computes, and the
// numbers go into docs/analysis/groom-strand-visibility-1246.md.
//
// It is CPU-only and needs no GL context, which is what lets the comparison be
// re-run in CI on every change instead of being a one-off measurement in a PR
// body that nothing defends.
//
// THE MODEL, AND WHY IT IS FAITHFUL RATHER THAN CONVENIENT.
//
//   * A strand segment is a TAPERED BAND WITH SQUARE ENDS in screen space —
//     one quad, exactly what GroomStrandMesh emits and the vertex shader
//     widens. A screen point is inside it when it lies between the segment's
//     two ends AND within the half-width interpolated along it. Square ends
//     matter at these widths: round caps of the RASTERISED half width are
//     ~0.79 px^2 each regardless of how thin the strand is, which on a
//     17 500-segment sub-pixel case reported about a fifth of the silhouette
//     as coverage of a shape the GPU never draws.
//
//   * A strand THINNER THAN A PIXEL is widened to one pixel and its alpha is
//     scaled by how much it was widened. This is the load-bearing step of
//     every production hair renderer and the reason sub-pixel coverage works
//     at all: a quarter-pixel-wide strand rasterised honestly produces a
//     fragment only where it happens to cross a pixel centre, so a coat of
//     them dissolves into sparkle as the camera moves. Widened to one pixel at
//     alpha 0.25 it produces a continuous, correctly-weighted line. The
//     widening is what every mode below then has to turn back into coverage.
//
//   * GROUND TRUTH is the same band set at its TRUE, un-widened half widths,
//     rasterised at NxN samples per pixel and box-filtered. It is the analytic
//     answer to "what fraction of this pixel is strand", to within 1/N^2.
//
//   * EVERY MODE IS EVALUATED INCREMENTALLY AND COMMUTATIVELY, one segment at
//     a time. That is not an optimisation; it is the statement that these four
//     modes are order-independent, expressed as code that could not depend on
//     order even if it wanted to. A fifth mode that needed a sorted fragment
//     list would not fit here, and that is the correct signal.
//
// WHAT IT DELIBERATELY DOES NOT MODEL. Shading, lighting, tone mapping and the
// temporal resolve's own filtering. The question is coverage; a mode that gets
// coverage wrong cannot be rescued downstream, and a mode that gets it right
// is then a shading problem (#1247), which is a different issue on purpose.
// The one temporal effect that IS modelled is the convergence of a stochastic
// estimate over F frames, because refusing to model it would flatter the
// deterministic modes.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomVisibility.h"

#include <glm/glm.hpp>

#include <vector>

namespace OloEngine
{
    class GroomAsset;

    namespace GroomCoverage
    {
        // ── Screen-space strand geometry ────────────────────────────────────

        // One curve segment after projection. Screen coordinates are in pixels
        // with the origin at the top-left corner of pixel (0,0), so the centre
        // of pixel (x,y) is (x + 0.5, y + 0.5).
        //
        // HalfWidth is in PIXELS and is the projected half of the cooked
        // per-point DIAMETER (GroomAsset stores diameters; the halving happens
        // once, here). Depth is the NDC depth in [0,1] — carried so a future
        // occlusion model can use it, and so a segment behind the camera can be
        // rejected rather than projected to nonsense.
        struct ScreenSegment
        {
            glm::vec2 A{ 0.0f };
            glm::vec2 B{ 0.0f };
            f32 HalfWidthA = 0.0f;
            f32 HalfWidthB = 0.0f;
            f32 DepthA = 0.0f;
            f32 DepthB = 0.0f;
            // Stable per-segment identity, used to decorrelate the stochastic
            // hash between two strands that overlap the same pixel. Derived
            // from the curve and segment indices, so it is a pure function of
            // the groom and survives a re-run.
            u32 Id = 0;

            [[nodiscard]] bool operator==(const ScreenSegment&) const = default;
        };

        // Projects every segment of `groom` into `outSegments` (cleared
        // first). Segments with a non-positive clip w at either end, or a
        // non-finite coordinate, are DROPPED and counted in the return value —
        // never clamped into view, which would invent coverage that is not
        // there.
        //
        // The three matrices are kept APART rather than pre-multiplied because
        // the half-width projection needs them separately: a strand's screen
        // thickness is its world radius times `projection[1][1] * height / 2 /
        // clipW`, which is exact for both a perspective and an orthographic
        // projection and cannot be recovered from a combined MVP. `model`
        // additionally supplies the object-to-world scale, taken as the mean of
        // its three axis lengths — the same convention AlembicGroomImporter
        // uses when it bakes a non-uniform transform into the widths.
        //
        // `widthScale` multiplies the cooked object-space diameters; it is the
        // authoring lever that lets one groom be measured at several apparent
        // thicknesses without re-cooking.
        struct ProjectionStats
        {
            u32 SegmentsProjected = 0;
            u32 SegmentsDroppedBehindCamera = 0;
            u32 SegmentsDroppedNonFinite = 0;
            f32 MinHalfWidthPixels = 0.0f;
            f32 MaxHalfWidthPixels = 0.0f;
            f32 MeanHalfWidthPixels = 0.0f;
        };

        [[nodiscard]] ProjectionStats ProjectGroom(const GroomAsset& groom, const glm::mat4& model,
                                                   const glm::mat4& view, const glm::mat4& projection,
                                                   u32 viewportWidth, u32 viewportHeight, f32 widthScale,
                                                   std::vector<ScreenSegment>& outSegments);

        // ── Ground truth ────────────────────────────────────────────────────

        // Per-pixel coverage in [0,1], row-major, `width * height` entries.
        //
        // `supersample` is the linear sample rate: N gives N*N samples per
        // pixel and a quantisation of 1/N^2. 16 is the value the analysis doc
        // reports; below 8 the reference is no longer meaningfully finer than
        // the modes it judges, so values under 4 are rejected (returns an empty
        // vector) rather than silently producing a reference that flatters
        // everything.
        [[nodiscard]] std::vector<f32> ReferenceCoverage(const std::vector<ScreenSegment>& segments, u32 width,
                                                         u32 height, u32 supersample);

        // ── What each mode actually produces ────────────────────────────────

        // Everything a mode needs beyond the geometry itself.
        struct ModeParameters
        {
            /// Alpha cutoff for OpaqueRibbon. 0.5 is the engine's Material
            /// default (Material.h) and the value the analysis reports.
            f32 AlphaCutoff = 0.5f;

            /// Sample count for AlphaToCoverage. Must be 2, 4 or 8 — the set
            /// GBuffer::Create accepts.
            u32 SampleCount = 4;

            /// Frames the stochastic estimate is averaged over. 1 models a
            /// single frame with no temporal resolve (which is the state
            /// SelectGroomComposition refuses to ship); 8 and 16 model TAA's
            /// and FSR2's effective history lengths. The Halton jitter is NOT
            /// modelled here — the stochastic hash is reseeded per frame, and
            /// modelling jitter as well would conflate two error sources the
            /// comparison needs to keep apart.
            u32 TemporalFrames = 1;

            /// Seed for the stochastic hash. Changing it must not change any
            /// reported statistic beyond noise; GroomCoverageTest asserts that.
            u32 StochasticSeed = 1246;
        };

        // Coverage produced by `mode`, in the same layout as
        // ReferenceCoverage. Returns an empty vector for an invalid mode or
        // invalid parameters.
        [[nodiscard]] std::vector<f32> ModeCoverage(const std::vector<ScreenSegment>& segments, u32 width, u32 height,
                                                    GroomCompositionMode mode, const ModeParameters& parameters);

        // ── The comparison ──────────────────────────────────────────────────

        // Error of `measured` against `reference`, over the pixels either of
        // them touches. Pixels neither touches are excluded: including them
        // would let a larger empty border drive every metric toward zero and
        // make two resolutions incomparable, which is exactly the comparison
        // criterion 3 asks for.
        struct CoverageError
        {
            /// Mean |measured - reference|. The headline number.
            f64 MeanAbsolute = 0.0;
            /// Root-mean-square of the same difference. Punishes the
            /// occasional badly-wrong pixel that MeanAbsolute averages away —
            /// which is what a stochastic mode's noise looks like.
            f64 Rmse = 0.0;
            /// Largest single-pixel error.
            f64 MaxAbsolute = 0.0;
            /// Mean SIGNED (measured - reference). Separates a mode that is
            /// noisy-but-unbiased from one that systematically drops or
            /// invents coverage — the two look identical in MeanAbsolute and
            /// are completely different defects.
            f64 MeanSignedBias = 0.0;
            /// Pixels the metrics were computed over.
            u32 ComparedPixels = 0;
            /// Pixels the reference covers at all. Reported so a result over a
            /// nearly-empty frame is recognisable as one.
            u32 ReferenceCoveredPixels = 0;
        };

        [[nodiscard]] CoverageError CompareCoverage(const std::vector<f32>& measured, const std::vector<f32>& reference);

        // Total silhouette coverage — the sum of per-pixel coverage, i.e. the
        // strand area in pixels. Reported alongside the error because a mode
        // can have a small mean error and still have lost a tenth of the coat:
        // criterion 3's "stable silhouettes" is a claim about this number
        // holding steady across frames and resolutions, and it is the one
        // statistic that is directly comparable between two resolutions once
        // divided by the pixel count.
        [[nodiscard]] f64 TotalCoverage(const std::vector<f32>& coverage);

        // ── Temporal stability ──────────────────────────────────────────────

        // How much a mode's coverage MOVES between consecutive frames of a
        // fixed camera, which is what "crawl" and "sparkle" are. Measured by
        // evaluating the mode at `frameCount` successive frame indices and
        // taking the mean absolute difference between consecutive frames.
        //
        // A deterministic mode scores exactly 0 here on a static camera, which
        // is the correct and useful answer: it says the mode's error is fixed
        // pattern rather than noise, and a fixed pattern is what a temporal
        // resolve cannot remove.
        struct TemporalStability
        {
            /// Mean |coverage(f) - coverage(f-1)| over covered pixels.
            f64 MeanFrameToFrameDelta = 0.0;
            /// The largest such mean over the sequence — a single bad frame.
            f64 MaxFrameToFrameDelta = 0.0;
            /// Mean absolute error of the frames AVERAGED together, against
            /// the reference. This is what a temporal resolve converges to,
            /// and it is the number that decides whether a stochastic mode is
            /// viable at all.
            f64 ConvergedMeanAbsolute = 0.0;
            u32 FramesEvaluated = 0;
        };

        [[nodiscard]] TemporalStability MeasureTemporalStability(const std::vector<ScreenSegment>& segments, u32 width,
                                                                 u32 height, GroomCompositionMode mode,
                                                                 const ModeParameters& parameters, u32 frameCount,
                                                                 const std::vector<f32>& reference);

        // ── Memory ──────────────────────────────────────────────────────────

        // Bytes a mode costs ON TOP of drawing the strands at all, for one
        // frame at this resolution. Criterion 1 asks for memory alongside
        // quality and cost, and the honest answer is a function of the mode and
        // the resolution rather than a measurement: the strand vertex data is
        // identical in every mode, so the only thing that differs is the
        // targets the mode needs.
        //
        // OpaqueRibbon and StochasticAlpha add nothing. AlphaToCoverage
        // multiplies the colour and depth attachments by the sample count.
        // WeightedBlendedOIT adds the RGBA16F accumulation and RG16F revealage
        // targets. `gbufferBytesPerSamplePerPixel` is the engine's
        // per-attachment total, passed in rather than hard-coded so the number
        // tracks GBuffer's attachment table instead of drifting from it.
        [[nodiscard]] u64 ModeExtraBytes(GroomCompositionMode mode, u32 width, u32 height, u32 sampleCount,
                                         u32 gbufferBytesPerSamplePerPixel);

        // ── The hash, exposed ───────────────────────────────────────────────

        // The stochastic alpha test's hash: a pure function of pixel, frame,
        // segment id and seed, returning a value in [0,1).
        //
        // Exposed because the GLSL side must compute the SAME value from the
        // same inputs — a CPU model that predicts a different sample set than
        // the shader is a model of nothing. GroomCoverageTest pins its
        // distribution and GroomStrandGpuParityTest pins the GLSL agreement.
        [[nodiscard]] f32 StochasticHash(u32 pixelX, u32 pixelY, u32 frameIndex, u32 segmentId, u32 seed) noexcept;

        // The sample mask hardware alpha-to-coverage produces for `alpha` at
        // `sampleCount` samples.
        //
        // Modelled as the hardware does it — a DETERMINISTIC mask that is a
        // function of alpha alone — and not as a dithered one, because that
        // determinism is precisely alpha-to-coverage's dense-overlap failure:
        // two strands at alpha 0.5 overlapping a pixel claim the SAME samples,
        // so the pixel reads 0.5 where the true union is 0.75. Modelling a
        // decorrelated mask here would hide the defect the comparison exists
        // to find.
        [[nodiscard]] u32 AlphaToCoverageMask(f32 alpha, u32 sampleCount) noexcept;
    } // namespace GroomCoverage
} // namespace OloEngine
