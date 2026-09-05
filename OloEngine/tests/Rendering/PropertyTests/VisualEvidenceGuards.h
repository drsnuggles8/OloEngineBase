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
//
// Neither check knows anything about the subject beyond a predicate, so a test
// supplies its own (the boat's albedo, a magenta seafloor, a green gizmo).
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <gtest/gtest.h>

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
} // namespace OloEngine::Tests::VisualEvidence
