#pragma once

// =============================================================================
// GroomShadowWidening.h — the light-space one-texel width floor, on the CPU.
// Issue #1323.
//
// TWIN OF OloEditor/assets/shaders/include/GroomShadowWidening.glsl. The two
// sides compute the same numbers the same way, and the pair exists for the
// reason GroomCoverage.h and GroomStrandCommon.glsl are a pair: the claim
// "a 70 um hair is far below a cascade texel, so rasterised honestly it casts
// nothing" is a MEASUREMENT, and a measurement that only exists in a shader
// cannot be asserted by a test on a machine with no GPU.
//
// NO RENDERER HEADERS, deliberately, like the rest of Groom/. Two floats and a
// matrix column go in; a half width comes out.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/glm.hpp>

namespace OloEngine
{
    /// NDC per world metre for an offset perpendicular to the light's view
    /// axis, from a view-projection and the clip w of the point being widened.
    ///
    /// ONE EXPRESSION FOR BOTH PROJECTION KINDS. Row 0 of a view-projection,
    /// read as a row vector over world xyz, has length |P[0][0]| for a
    /// perspective projection and 1/halfExtent for an orthographic one; clip.w
    /// is 1 in the orthographic case, so the divide is the identity there and
    /// the perspective foreshortening everywhere else.
    ///
    /// The MAGNITUDE of each row, never the signed element: Vulkan's clip space
    /// has +Y downwards and the engine uploads a projection whose [1][1] is
    /// negative there.
    [[nodiscard]] inline f32 GroomShadowNdcPerWorld(const glm::mat4& viewProjection, f32 clipW) noexcept
    {
        const f32 safeW = std::max(std::abs(clipW), 1.0e-6f);
        const f32 sx = glm::length(glm::vec3(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0]));
        const f32 sy = glm::length(glm::vec3(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1]));
        return 0.5f * (sx + sy) / safeW;
    }

    /// The SMALLER of the two NDC-per-metre row scales at clip w `clipW`: the
    /// least an offset across the view's ray moves NDC by, for an
    /// orthographic or a symmetric perspective view (every shadow view's
    /// kind). CPU only: what a lower bound on a projected NDC length is
    /// converted at (DecideGroomCasterRun), where GroomShadowNdcPerWorld's
    /// mean of the two would overstate a non-square view.
    [[nodiscard]] inline f32 GroomShadowMinNdcPerWorld(const glm::mat4& viewProjection, f32 clipW) noexcept
    {
        const f32 safeW = std::max(std::abs(clipW), 1.0e-6f);
        const f32 sx = glm::length(glm::vec3(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0]));
        const f32 sy = glm::length(glm::vec3(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1]));
        return std::min(sx, sy) / safeW;
    }

    /// The half width, in NDC, a strand of `radiusWorld` metres is rasterised
    /// at against a `resolutionTexels`-wide target, floored at
    /// `minWidthTexels` texels of FULL width.
    ///
    /// The floor is on the FULL width: NDC spans [-1, 1] over
    /// `resolutionTexels` texels, so one texel is 2/resolution of NDC and a
    /// band of half width 1/resolution is one texel across — the smallest band
    /// that reliably contains a texel centre.
    [[nodiscard]] inline f32 GroomShadowHalfWidthNdc(f32 radiusWorld, f32 ndcPerWorld, f32 resolutionTexels,
                                                     f32 minWidthTexels) noexcept
    {
        const f32 trueHalfNdc = radiusWorld * ndcPerWorld;
        const f32 floorHalfNdc = std::max(minWidthTexels, 0.0f) / std::max(resolutionTexels, 1.0f);
        return std::max(trueHalfNdc, floorHalfNdc);
    }

    /// How much the floor widened this strand: the rasterised half width over
    /// the true one. 1.0 means the strand was already at least a texel wide.
    ///
    /// This is the number the OPAQUE-shadow approximation is bounded by. A
    /// depth target has no alpha to weight, and a hashed discard would be a
    /// stochastic technique with nothing to converge it — the fallback
    /// groom-strand-visibility.md rule 6 refuses — so a widened strand casts an
    /// opaque shadow and over-occludes by at most this factor. Reported rather
    /// than hidden: it is what says whether a coat's shadow is a silhouette or
    /// an over-estimate, and it is the first number to look at when one reads
    /// too dark.
    [[nodiscard]] inline f32 GroomShadowWideningFactor(f32 radiusWorld, f32 ndcPerWorld, f32 resolutionTexels,
                                                       f32 minWidthTexels) noexcept
    {
        const f32 trueHalfNdc = radiusWorld * ndcPerWorld;
        if (!(trueHalfNdc > 0.0f) || !std::isfinite(trueHalfNdc))
        {
            return 1.0f;
        }
        return GroomShadowHalfWidthNdc(radiusWorld, ndcPerWorld, resolutionTexels, minWidthTexels) / trueHalfNdc;
    }

    /// How many times the true coverage a caster SUBSET keeps (#1533 E1): see
    /// GroomShadowCasterFraction. Two, so the floored map still blocks more
    /// than the real coat does wherever a fringe thins out.
    inline constexpr f32 kGroomCasterCoverageMargin = 2.0f;

    /// The smallest share of a coat any view casts with: past sixteen-fold
    /// the saving is noise and the subset is a few strands per texel.
    inline constexpr f32 kGroomCasterMinFraction = 1.0f / 16.0f;

    /// The strand LAYERS a run's subset should still lay over its footprint
    /// (#1533 E1). The coverage margin is an expectation, and a sparse coat meets
    /// it with strands far apart: each one a texel wide, gaps between them, and
    /// the shadow turns to streaks and blocks where the whole cast was solid
    /// (GroomSceneShadow case 9, measured). IF strands landed on texels as a
    /// Poisson process, four layers would leave exp(-4), about two per cent, of
    /// texels open. That is a MODEL, not a guarantee: the layers are a run-wide
    /// average over a box, strands are lines rather than points, and a fringe
    /// where the run thins out keeps fewer layers than its average
    /// (GroomCasterCoverageTest measures where the model holds).
    inline constexpr f32 kGroomCasterMinLayers = 4.0f;

    /// The share of a strand's length its shadow is ASSUMED to keep when the
    /// run's direction moments are not known (an order built without strand
    /// summaries; DecideGroomCasterRun). An assumption, not a bound: a strand
    /// along the light projects to a dot, and this half credits it with a line
    /// (#1533 follow-up review). Runs with moments use
    /// GroomShadowProjectedLengthLowerBound instead.
    inline constexpr f32 kGroomCasterProjectedLengthShare = 0.5f;

    /// The direction a view projects along at `point` (#1533): the one along
    /// which the point's NDC position does not change, so a strand lying along
    /// it lands on one spot of the map. The cross product of the gradients of
    /// x/w and y/w: rows 0 and 1 for an orthographic view, the ray from the
    /// light for a perspective one. `point` is in the space `viewProjection`
    /// maps (render-relative for the shadow pass's matrices). The sign is
    /// arbitrary; zero when the matrix is degenerate there.
    [[nodiscard]] inline glm::vec3 GroomShadowProjectionDirection(const glm::mat4& viewProjection,
                                                                  const glm::vec3& point) noexcept
    {
        const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
        const glm::vec3 row0{ viewProjection[0][0], viewProjection[1][0], viewProjection[2][0] };
        const glm::vec3 row1{ viewProjection[0][1], viewProjection[1][1], viewProjection[2][1] };
        const glm::vec3 row3{ viewProjection[0][3], viewProjection[1][3], viewProjection[2][3] };
        const glm::vec3 direction = glm::cross(row0 * clip.w - row3 * clip.x, row1 * clip.w - row3 * clip.y);
        const f32 length = glm::length(direction);
        return (length > 0.0f && std::isfinite(length)) ? direction / length : glm::vec3(0.0f);
    }

    /// A LOWER BOUND on the length a run of strands projects to across a unit
    /// direction `d` (in the same space as its moments): a segment of length l at
    /// angle theta to d projects to l sin(theta), and sin(theta) >= sin^2(theta)
    /// = 1 - cos^2(theta), so the sum is at least L - d^T M d with M the summed
    /// l * outer(t, t) (GroomCasterStrand::Moments). Exact for strands lying
    /// across the light, zero for strands along it -- the case the half share
    /// overstates -- and, with only second moments known, the tightest bound
    /// there is: a run split between strands exactly along and exactly across d
    /// meets it. Never negative.
    ///
    /// A bound ONLY for the actual moments of the geometry, with ONE direction
    /// for every segment: an estimate of M, or a light whose rays fan across
    /// the run, needs its own correction (DecideGroomCasterRun).
    [[nodiscard]] inline f32 GroomShadowProjectedLengthLowerBound(f32 totalLength, const std::array<f32, 6>& moments,
                                                                  const glm::vec3& direction) noexcept
    {
        const f32 dmd = moments[0] * direction.x * direction.x + moments[1] * direction.y * direction.y +
                        moments[2] * direction.z * direction.z +
                        2.0f * (moments[3] * direction.x * direction.y + moments[4] * direction.x * direction.z +
                                moments[5] * direction.y * direction.z);
        const f32 bound = totalLength - dmd;
        return std::isfinite(bound) ? std::max(bound, 0.0f) : 0.0f;
    }

    /// The strand layers a run cast WHOLE lays over its footprint in one view, as
    /// an ESTIMATE (#1533): the area its floored ribbons cover -- their projected
    /// length times the floored width -- over the NDC area of its box. The
    /// ribbon has NO END CAPS (GroomShadowWidening.glsl widens sideways only),
    /// so a strand along the light covers nothing beyond its projected length,
    /// and there is no dot term to add.
    ///
    /// A run-wide MEAN, not a per-texel count: the box is at least the area the
    /// strands cover, which keeps the mean low, but a fringe where the run
    /// thins out lays fewer layers than the mean says. `projectedLengthWorld`
    /// should be GroomShadowProjectedLengthLowerBound's when the run's moments
    /// are known. Zero for anything that cannot be measured, which casts the run
    /// whole.
    [[nodiscard]] inline f32 GroomShadowCasterLayersFromProjection(f32 projectedLengthWorld, f32 meanRadiusWorld,
                                                                   f32 ndcPerWorld, f32 resolutionTexels,
                                                                   f32 minWidthTexels, f32 footprintNdcArea) noexcept
    {
        if (!(footprintNdcArea > 0.0f) || !(ndcPerWorld > 0.0f) || !(projectedLengthWorld > 0.0f) ||
            !std::isfinite(footprintNdcArea) || !std::isfinite(ndcPerWorld) || !std::isfinite(projectedLengthWorld))
        {
            return 0.0f;
        }
        const f32 widthNdc =
            2.0f * GroomShadowHalfWidthNdc(std::max(meanRadiusWorld, 0.0f), ndcPerWorld, resolutionTexels, minWidthTexels);
        const f32 layers = projectedLengthWorld * ndcPerWorld * widthNdc / footprintNdcArea;
        return std::isfinite(layers) ? layers : 0.0f;
    }

    /// The share of a coat's strands one shadow view needs (#1533 E1), given the
    /// coat's LENGTH-WEIGHTED mean radius in world metres.
    ///
    /// THE FLOOR ALREADY OVER-OCCLUDES, by GroomShadowWideningFactor: a strand a
    /// fifth of a texel wide is rasterised a full texel wide and blocks five
    /// times the area it really does. So a uniform 1/F of the coat at the floor
    /// blocks, in expectation, the area the whole coat blocks at its true widths
    /// -- a depth map's texel occupancy, 1 - exp(-coverage), then matches the
    /// real coat's opacity instead of exceeding it F-fold. The fraction keeps
    /// `margin` times that, clamped to [minFraction, 1].
    ///
    /// In EXPECTATION only, and with the mean radius standing for every strand:
    /// a strand wider than the floor keeps its own width and is not widened, so
    /// a run of mixed widths is over-thinned where its thin strands are -- which
    /// is why runs are per coat role (GroomCasterRun) rather than coat-wide.
    /// Where the mean strand is already a texel wide nothing is widened, nothing
    /// is over-covered, and the whole run is cast.
    ///
    /// AND NO THINNER THAN `minLayers` OVER `layersWhole` (the layer estimate):
    /// the expectation above says nothing about how far apart the kept strands
    /// are, and a sparse run thinned to it shows them one by one. A run whose
    /// whole cast lays too few layers to spare any is cast whole. The layers are
    /// an estimate (see kGroomCasterMinLayers), so this is a heuristic floor, not
    /// a per-texel bound.
    [[nodiscard]] inline f32 GroomShadowCasterFraction(f32 meanRadiusWorld, f32 ndcPerWorld, f32 resolutionTexels,
                                                       f32 minWidthTexels, f32 margin, f32 minFraction,
                                                       f32 layersWhole, f32 minLayers) noexcept
    {
        const f32 widening = GroomShadowWideningFactor(meanRadiusWorld, ndcPerWorld, resolutionTexels, minWidthTexels);
        if (!(widening > 1.0f) || !std::isfinite(widening))
        {
            return 1.0f;
        }
        // A margin below 1 would cast less than the real coat blocks.
        const f32 kept = std::max(std::isfinite(margin) ? margin : 1.0f, 1.0f) / widening;
        // Unmeasured layers, or a floor that is not a number, keep every strand.
        const f32 dense = (layersWhole > 0.0f && std::isfinite(layersWhole) && std::isfinite(minLayers))
                              ? std::max(minLayers, 0.0f) / layersWhole
                              : 1.0f;
        const f32 lowest = std::isfinite(minFraction) ? std::clamp(minFraction, 0.0f, 1.0f) : 1.0f;
        return std::clamp(std::max(kept, dense), lowest, 1.0f);
    }
} // namespace OloEngine
