#pragma once

// =============================================================================
// GroomPreview.h — the static debug visualisation of an imported groom (#1232).
//
// This is the acceptance target of the issue: import correctness becomes
// VISIBLE before any hair shading exists. It draws debug lines, nothing else —
// no shader, no material, no strand expansion. Photoreal hair is #1246/#1247
// and is explicitly out of scope; drifting into it here would remove the plain
// picture that makes a bad import obvious.
//
// What the preview shows, and why each one:
//   * STRANDS — the polyline itself, so a groom that imported with scrambled
//     topology looks scrambled instead of merely wrong in a log line.
//   * ROOTS — a marker at each curve's first control point. A groom whose roots
//     are not on the surface is the single most common import failure, and it
//     is invisible unless roots are drawn separately.
//   * DIRECTION — each strand is drawn dark at the root and bright at the tip.
//     A groom imported tip-first still looks like hair; the gradient is what
//     makes the reversal visible.
//   * GROUPS — one hue per group, so group assignment can be read off the
//     screen rather than inferred from counts.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomDeformation.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    struct GroomPreviewSettings
    {
        bool ShowStrands = true;
        bool ShowRoots = true;
        // Root-to-tip brightness ramp. Off draws every strand at full colour.
        bool ShowDirection = true;
        // One hue per group. Off draws every strand in a single neutral colour.
        bool ColorByGroup = true;
        // Draw ONLY the guide curves. A production groom is millions of
        // strands and a few hundred guides; this is how the guides get looked
        // at at all.
        bool GuidesOnly = false;

        // Hard cap on drawn strands. A debug line is one command packet, so an
        // uncapped preview of a real groom would submit millions of them and
        // stall the editor — the preview SUBSAMPLES with a fixed stride rather
        // than truncating, so the visible subset still covers the whole groom.
        u32 MaxStrands = 2000;

        // Hard cap on drawn LINE SEGMENTS, which is the limit that actually
        // bites. Every debug line consumes one entry of the frame's transform
        // buffer (capacity 65536, shared with the WHOLE scene and re-consumed
        // per pass), and a strand costs one line per segment PLUS three for its
        // root cross — so 2000 eight-point strands is 20,000 entries for ONE
        // groom. Two reference grooms at that rate overflowed the buffer and
        // logged "FrameDataBuffer: Transform buffer overflow!" every frame.
        //
        // 8000 is deliberately well under the raw headroom: the budget is
        // PER GROOM, a scene may hold several, and the shadow and depth passes
        // re-submit. It still draws hundreds to low thousands of strands, which
        // is more than enough to read an import by eye. The stride below is
        // computed from BOTH caps, so raising MaxStrands on a dense groom
        // widens the stride instead of blowing the frame budget, and the
        // subsample still spans the whole groom.
        u32 MaxSegments = 8000;

        // Length of the root cross arms, in world units.
        f32 RootMarkerSize = 0.01f;
    };

    // How much of the groom the last call actually drew. Returned so the
    // editor can say "2000 of 1.2M strands" instead of silently showing a
    // thousandth of the asset as if it were all of it.
    struct GroomPreviewStats
    {
        u32 StrandsDrawn = 0;
        u32 StrandsAvailable = 0;
        u32 SegmentsDrawn = 0;
        u32 Stride = 1;
        // True when the SEGMENT budget, rather than MaxStrands, set the stride.
        // Surfaced so the editor can say why a groom is being thinned out;
        // without it, raising "Max Strands" and seeing no change looks broken.
        bool SegmentBudgetLimited = false;
        // True when the stride is applied PER GROUP rather than globally, which
        // is what guarantees every represented group contributes at least one
        // drawn strand. It requires the cook's contiguous group ranges; an
        // un-canonicalised groom with interleaved group ids falls back to a
        // global stride, because per-group phasing on interleaved ids would
        // reset the counter on nearly every curve and select everything.
        bool GroupPhasedSelection = false;
        // True when MaxSegments was hit DURING submission and the remaining
        // curves were dropped. The stride is an estimate from the average
        // strand length; on a variable-length groom it can overshoot, and the
        // cap is enforced exactly at submission. Reported rather than silent,
        // because the visible result is a partially drawn groom.
        bool SegmentBudgetExhausted = false;
    };

    // Decides WHICH curves the preview will draw, without drawing anything.
    // Pure, and separate from DrawGroomPreview on purpose: the selection rule
    // is the part with the interesting behaviour (two caps, a stride rather
    // than a truncation, guides-only striding over the guides), and splitting
    // it out lets that behaviour be tested with no GL context at all —
    // Renderer3D::DrawLine needs a live scene pass, so a test that went through
    // the drawing path could only run on a GPU machine.
    //
    // The returned stats carry StrandsAvailable, Stride and
    // SegmentBudgetLimited; the two "Drawn" counters are filled in by the draw.
    [[nodiscard]] GroomPreviewStats PlanGroomPreview(const GroomAsset& groom, const GroomPreviewSettings& settings);

    // The exact set of curve indices DrawGroomPreview will draw, in draw order.
    // Pure, and the single source of the selection rule: the stride, the
    // per-group phasing and the exact line budget all live here, so a test can
    // assert on the real selection instead of re-deriving it (a re-derivation
    // passes happily while the implementation drifts).
    //
    // `plan` must come from PlanGroomPreview for the same groom and settings.
    // Updates `plan`'s SegmentBudgetExhausted and the drawn counters.
    void SelectGroomPreviewCurves(const GroomAsset& groom, const GroomPreviewSettings& settings,
                                  GroomPreviewStats& plan, std::vector<u32>& outCurves);

    // Draws `groom` through Renderer3D's debug-line path. `transform` is the
    // entity's world matrix; the groom's points are in its own object space.
    // Must be called between Renderer3D::BeginScene and EndScene.
    GroomPreviewStats DrawGroomPreview(const GroomAsset& groom, const glm::mat4& transform,
                                       const GroomPreviewSettings& settings);

    // The colour assigned to `groupId`. Deterministic and independent of the
    // groom, so the same group index is the same colour in the viewport and in
    // the inspector's group list.
    [[nodiscard]] glm::vec3 GroomGroupColor(u32 groupId) noexcept;

    // ── Binding preview (issue #1249) ────────────────────────────

    struct GroomBindingPreviewSettings
    {
        // Hard cap on the roots drawn. Three lines per root and one command
        // packet per line, so this is the same class of budget GroomPreview's
        // is and it is deliberately far smaller: the question this view answers
        // — "is the coat attached where I think it is" — is answered by a
        // scatter of frames, not by all of them.
        u32 MaxRoots = 512;

        // Length of each frame's axis arms, in world units.
        f32 AxisLength = 0.02f;

        // Draw the roots that were HELD AT REST because their deformed triangle
        // collapsed. They are marked in a different colour and are the reason
        // this view exists at all: a patch of coat that does not move is
        // invisible next to one that does, until it is drawn.
        bool ShowHeldRoots = true;
    };

    struct GroomBindingPreviewStats
    {
        u32 RootsAvailable = 0;
        u32 RootsDrawn = 0;
        u32 RootsHeldDrawn = 0;
        u32 LinesDrawn = 0;
        u32 Stride = 1;

        [[nodiscard]] bool operator==(const GroomBindingPreviewStats&) const = default;
    };

    // Draws each bound root's DEFORMED frame through Renderer3D's debug-line
    // path. Must be called between Renderer3D::BeginScene and EndScene.
    //
    // `transforms` is indexed by curve and comes from
    // EvaluateGroomRootTransforms — the same array the strand geometry was
    // built from THIS frame, not a second evaluation. That is the whole value
    // of the view: what it draws is what the coat was drawn with, so a frame
    // that is visibly on the wrong triangle is a frame the renderer used.
    //
    // A stride over the whole binding rather than a prefix, for the reason
    // GroomStrandMesh.h gives: a prefix of a cooked groom is one contiguous
    // range of curves, which after the cook is one side of the animal.
    GroomBindingPreviewStats DrawGroomBindingPreview(const GroomBindingAsset& binding,
                                                     std::span<const GroomRootTransform> transforms,
                                                     const glm::mat4& transform,
                                                     const GroomBindingPreviewSettings& settings);
} // namespace OloEngine
