#pragma once

// =============================================================================
// VisualEvidenceGuards — the two checks every multi-angle visual-evidence test
// needs, and the RGBA8 utilities they share.
//
// WHY THIS EXISTS (issue #931).
//   A capture test whose camera never actually moved rendered its whole set
//   from one pose: three "different angles", byte-identical frames, sky and
//   water present and the subject absent. It was caught only because that test
//   happened to measure its own noise floor and happened to look for the
//   subject's colour. A test that rebaked on first run without those two checks
//   would have committed three identical PNGs as goldens and gone green
//   forever, proving nothing, for as long as the file existed.
//
//   So the two checks are not decoration on the golden comparison — they are
//   what makes the golden comparison MEAN anything, and every new
//   *VisualEvidenceTest that captures more than one pose should call both.
//
//   1. `ExpectCapturesAreDistinct` — the frames from different poses must
//      differ by far more than the run-to-run noise floor. Capture the SAME
//      pose twice to measure that floor rather than guessing a constant: it is
//      the only number that separates "the renderer is jittery" from "the
//      camera never moved".
//   2. `ExpectFrameHasSubject` — a content mask must actually find the subject.
//      A frame can be perfectly distinct from its neighbours and still be sky
//      and nothing else; only a mask that names a colour the BACKGROUND cannot
//      produce answers "is the thing I am testing on screen at all".
//   3. `ExpectFineDetailNotReduced` (issue #1401) - a feature that claims to
//      add geometric richness must not REDUCE measured fine detail in its own
//      A/B. Presence checks (coverage, luma, "the frames differ") cannot fail a
//      feature that makes the frame worse; this one cannot be satisfied by
//      switching the feature on. See docs/agent-rules/visual-quality-criteria.md.
//
// Neither check knows anything about the subject beyond a predicate, so a test
// supplies its own (the boat's albedo, a magenta seafloor, a green gizmo).
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace OloEngine::Tests::VisualEvidence
{
    /// Mean per-channel RMSE over RGB (alpha ignored) between two equal-size,
    /// tightly-packed RGBA8 buffers, in 0..255 units. Returns
    /// `std::numeric_limits<f64>::max()` for mismatched or empty inputs so a
    /// size bug reads as "maximally different" rather than as a pass.
    [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b);

    /// Mean RGB SSIM on 8x8 windows, used by the L8 RMSE/SSIM cascade.
    [[nodiscard]] f32 Rgba8Ssim(const std::vector<u8>& a, const std::vector<u8>& b, u32 width, u32 height);

    /// Flip an RGBA8 buffer vertically, in place. `glGetTextureImage` hands back
    /// rows bottom-up (GL origin); `stbi_write_png` and every "the lower band is
    /// the foreground" assertion treat row 0 as the TOP.
    void FlipRgbaRowsInPlace(std::vector<u8>& pixels, u32 width, u32 height);

    /// Fraction (0..1) of pixels for which `isSubject(r, g, b)` holds.
    [[nodiscard]] f64 SubjectCoverage(const std::vector<u8>& pixels,
                                      const std::function<bool(u32, u32, u32)>& isSubject);

    /// A frame must actually contain the thing the test is about.
    ///
    /// `minCoverage` is a FRACTION of the frame, so it survives a resolution
    /// change. Keep it small (a subject filling 1% of a 1280x720 frame is
    /// ~9200 pixels) — this guard is here to catch "nothing rendered", not to
    /// police framing.
    void ExpectFrameHasSubject(const std::vector<u8>& pixels, const std::string& poseName,
                               const std::function<bool(u32, u32, u32)>& isSubject,
                               f64 minCoverage = 0.002);

    /// Every pair of `captures` must differ by more than `noiseFloorRmse` times
    /// `margin`.
    ///
    /// Measure `noiseFloorRmse` by capturing ONE pose twice through the same
    /// code path the real captures use — that number carries the renderer's
    /// actual run-to-run variance (temporal jitter, particle streams, a clock
    /// that advanced) and nothing else. A hardcoded threshold either passes a
    /// frozen camera on a deterministic renderer or fails a noisy one; the
    /// measured floor does neither.
    ///
    /// `margin` defaults to 4: a real change of camera angle over any scene
    /// with geometry in it moves far more than four times the jitter, and the
    /// failure being guarded against is a ratio of ONE.
    void ExpectCapturesAreDistinct(const std::vector<std::vector<u8>>& captures,
                                   const std::vector<std::string>& poseNames,
                                   f64 noiseFloorRmse, f64 margin = 4.0);

    /// A pixel rectangle in the buffer's OWN row order (row 0 = first row stored).
    /// A raw GL readback is bottom-up; flip first (`FlipRgbaRowsInPlace`) if the
    /// crop is described in image coordinates, and use the same crop for both
    /// arms of an A/B either way.
    struct PixelRect
    {
        u32 X = 0;
        u32 Y = 0;
        u32 Width = 0;
        u32 Height = 0;
    };

    /// Luminance-gradient threshold, in 0..255 units, above which a pixel counts
    /// as "fine detail". Fixed, not tunable per call site: a threshold picked to
    /// make one test pass is exactly the tuning that makes a gate vacuous.
    inline constexpr f32 kFineDetailGradientThreshold = 8.0f;

    /// Fine-detail density (issue #1401): the fraction (0..1) of pixels inside
    /// `crop` whose Rec.601 luminance gradient magnitude exceeds `threshold`.
    /// The gradient is the FORWARD difference sqrt(gx^2 + gy^2) with gx, gy =
    /// next - this (zero on the frame's last row/column), taking the neighbour
    /// from the whole frame so a crop does not create an artificial edge along
    /// its border. Not a central difference: that reads zero on a 1 px
    /// alternation, blind to exactly the finest detail (and dither) there is.
    ///
    /// It is a property of a frame at ONE output resolution. Never compare a
    /// native capture with an upscaled one, or two captures with different post
    /// stacks; noise and dither RAISE the number, so it is safe as an A/B
    /// (both arms carry the same dither) and unsafe as an absolute floor on a
    /// frame that is not temporally stable.
    ///
    /// Returns NaN, never 0, for a zero-size or mismatched buffer, a crop that
    /// leaves the frame, or a non-finite / negative threshold, so a wiring bug
    /// fails every comparison instead of reading as "no detail".
    [[nodiscard]] f64 FineDetailDensity(const std::vector<u8>& rgba, u32 width, u32 height, const PixelRect& crop,
                                        f32 threshold = kFineDetailGradientThreshold);

    /// Whole-frame overload.
    [[nodiscard]] f64 FineDetailDensity(const std::vector<u8>& rgba, u32 width, u32 height,
                                        f32 threshold = kFineDetailGradientThreshold);

    /// Non-regression A/B: the arm with the feature ON must carry at least as
    /// much fine detail as the arm with it OFF, minus `slack` (an absolute
    /// fraction, default 0; pass the measured run-to-run difference of one arm
    /// if the frame is not bit-stable). Both buffers must be `width * height`
    /// RGBA8, captured at the same resolution with the same post configuration
    /// (see `ExpectConditionsPinned`). Returns the density ratio on/off for
    /// logging (NaN when either is unmeasurable or the off arm has no detail).
    f64 ExpectFineDetailNotReduced(const std::vector<u8>& featureOn, const std::vector<u8>& featureOff, u32 width,
                                   u32 height, const PixelRect& crop, const std::string& label, f64 slack = 0.0);

    /// The WEAKER check (issue #1401): the frame's fine-detail density must be at
    /// least `minFraction` of a curated reference photograph's. It needs a
    /// reference per subject, and the reference is measured at its own native
    /// resolution, so `minFraction` is a wide margin (0.5), not a tuned target.
    /// It is defeated by dither or a shimmering alpha test (both raise the
    /// number), so use it only on a temporally stable frame and never instead of
    /// `ExpectFineDetailNotReduced`. Returns frame / reference (NaN if either is
    /// unmeasurable or the reference has no detail).
    f64 ExpectFineDetailFloorAgainstReference(const std::vector<u8>& frame, u32 width, u32 height, const PixelRect& crop,
                                              const std::vector<u8>& reference, u32 referenceWidth,
                                              u32 referenceHeight, f64 minFraction, const std::string& label);

    /// The A/B is only meaningful between arms captured under identical
    /// conditions. Snapshot whatever the capture depends on (output size, MSAA
    /// sample count, the full post-process settings struct...) at each arm's
    /// capture time and compare the snapshots here: a check, not a comment.
    template<typename Conditions>
    void ExpectConditionsPinned(const Conditions& onArm, const Conditions& offArm, const std::string& label)
    {
        EXPECT_TRUE(onArm == offArm) << label
                                     << ": the two arms were captured under different conditions (resolution, MSAA "
                                        "or post configuration). Fine-detail density is only comparable at "
                                        "identical conditions, so the A/B proves nothing (issue #1401).";
    }
} // namespace OloEngine::Tests::VisualEvidence
