#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Splat/GaussianSplatCloud.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine::GaussianSplat
{
    // Camera-dependent ordering, visibility and LOD for one splat cloud
    // (issue #971).
    //
    // WHY THIS IS A SEPARATE STAGE AND NOT PART OF THE DRAW. A Gaussian splat
    // is a semi-transparent primitive, so the frame is only correct if the
    // splats are composited back-to-front from the current eye. That ordering
    // changes every time the camera moves, which makes the sort a PER-FRAME,
    // PER-VIEW cost proportional to the whole cloud -- not a bake. It is the
    // single largest thing standing between "this renders" and "this is
    // affordable", so it lives on its own where it can be measured.
    //
    // Everything here is plain glm/std with no GL, so the ordering contract is
    // pinned on the CPU by GaussianSplatPipelineTest rather than inferred from
    // a screenshot.

    struct ViewSettings
    {
        // Hard cap on splats submitted for one view. This is the "bounded LOD"
        // the spike asks for: beyond the cap the lowest-contribution splats are
        // dropped, so frame cost has a ceiling that does not depend on how big
        // the imported cloud was.
        u32 MaxSplats = 1u << 20;

        // A splat whose 3-sigma footprint projects to less than this many
        // pixels is dropped. Sub-pixel splats are the bulk of a distant scan
        // and each one still costs a sort key, a quad and a blend.
        f32 MinScreenExtentPixels = 1.0f;

        // Splats closer than this in view space are dropped. The projection of
        // a Gaussian diverges at the eye plane, so there is no correct value to
        // draw there; the alternative to dropping is a screen-filling blob.
        f32 NearClip = 0.05f;

        // Alpha below which a splat contributes nothing a viewer can see. The
        // record quantises opacity to 8 bits, so anything under 1/255 is
        // already zero; this is the knob above that floor.
        f32 MinAlpha = 1.0f / 255.0f;
    };

    struct ViewStats
    {
        u32 Total = 0;
        u32 BehindNearPlane = 0;
        u32 FrustumCulled = 0;
        u32 TooSmall = 0;
        u32 TooFaint = 0;
        u32 OverBudget = 0;
        u32 Drawn = 0;
    };

    // The draw order for one view: indices into the cloud, far-to-near.
    struct ViewOrdering
    {
        std::vector<u32> Indices;
        ViewStats Stats;
    };

    // Ordering key for one splat: the IEEE bit pattern of its positive view
    // depth. For strictly positive floats that pattern is monotonic in the
    // value, so an integer radix sort over it is an EXACT float sort -- no
    // quantisation, no bucket-boundary ties, and no need to pick a depth range.
    [[nodiscard]] auto DepthSortKey(f32 viewDepth) -> u32;

    // Builds the draw order for `cloud` seen through `view`/`projection` at
    // `viewportPixels`. Deterministic: equal depths keep cloud order.
    void BuildViewOrdering(const SplatCloud& cloud,
                           const glm::mat4& view,
                           const glm::mat4& projection,
                           const glm::vec2& viewportPixels,
                           const ViewSettings& settings,
                           ViewOrdering& out);

    // The sort BuildViewOrdering uses: four 8-bit LSD radix passes over
    // `keys`, permuting `indices` alongside, leaving DESCENDING key order
    // (far-to-near). Stable, so equal keys keep their input order.
    //
    // Exposed because the sort is the measured quantity in the spike's
    // perf probe, which compares it against std::sort on the same input.
    void RadixSortDescending(std::span<u32> keys, std::span<u32> indices, std::vector<u32>& keyScratch,
                             std::vector<u32>& indexScratch);

    // The projected 3-sigma screen extent, in pixels, of a splat at
    // `viewDepth` whose largest world-space sigma is `worldSigma`.
    // `focalPixels` is projection[1][1] * viewportHeight * 0.5.
    [[nodiscard]] auto ProjectedExtentPixels(f32 worldSigma, f32 viewDepth, f32 focalPixels) -> f32;

    // Largest sigma of a splat, from the packed covariance upper triangle.
    // Uses sqrt(trace) rather than a real eigenvalue: it never underestimates,
    // and a culler that errs towards keeping a splat is the safe direction.
    [[nodiscard]] auto ConservativeSigma(const GpuSplat& splat) -> f32;
} // namespace OloEngine::GaussianSplat
