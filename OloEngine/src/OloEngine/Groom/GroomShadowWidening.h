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

    /// The strand LAYERS a subset must still lay over every texel of the coat's
    /// footprint (#1533 E1). The coverage margin is an expectation, and a sparse
    /// coat meets it with strands far apart: each one a texel wide, gaps between
    /// them, and the shadow turns to streaks and blocks where the whole cast was
    /// solid (GroomSceneShadow case 9, measured). Poisson holes fall as exp(-n):
    /// four layers leave about two per cent of texels open, which the 3x3 kernel
    /// averages away.
    inline constexpr f32 kGroomCasterMinLayers = 4.0f;

    /// The share of a strand's length its shadow keeps on average: strands lie
    /// along the skin, and toward the silhouette the skin turns along the light.
    /// A half undercounts the layers, which only keeps more strands.
    inline constexpr f32 kGroomCasterProjectedLengthShare = 0.5f;

    /// The strand layers a coat cast WHOLE lays over each texel of its footprint
    /// in one view (#1533 E1): its drawn length times the width each strand is
    /// rasterised at, over the area it falls on, all in that view's NDC.
    /// `footprintNdcArea` is the coat's box projected and bounded in NDC. The
    /// box overstates the footprint and the mean width understates the floored
    /// one, so this errs low, toward keeping strands. Zero for anything that
    /// cannot be measured, which casts the coat whole.
    [[nodiscard]] inline f32 GroomShadowCasterLayers(f32 totalLengthWorld, f32 meanRadiusWorld, f32 ndcPerWorld,
                                                     f32 resolutionTexels, f32 minWidthTexels,
                                                     f32 footprintNdcArea) noexcept
    {
        if (!(footprintNdcArea > 0.0f) || !(ndcPerWorld > 0.0f) || !(totalLengthWorld > 0.0f) ||
            !std::isfinite(footprintNdcArea) || !std::isfinite(ndcPerWorld) || !std::isfinite(totalLengthWorld))
        {
            return 0.0f;
        }
        const f32 widthNdc =
            2.0f * GroomShadowHalfWidthNdc(std::max(meanRadiusWorld, 0.0f), ndcPerWorld, resolutionTexels, minWidthTexels);
        const f32 lengthNdc = kGroomCasterProjectedLengthShare * totalLengthWorld * ndcPerWorld;
        const f32 layers = lengthNdc * widthNdc / footprintNdcArea;
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
    /// The mean radius makes it a LOWER BOUND on what the subset covers: a
    /// strand wider than the floor keeps its own width, so the floored draw
    /// covers at least floor * length per strand and never less than the mean
    /// says. Where the mean strand is already a texel wide nothing is widened,
    /// nothing is over-covered, and the whole coat is cast.
    ///
    /// AND NO THINNER THAN `minLayers` OVER `layersWhole` (GroomShadowCasterLayers):
    /// the expectation above says nothing about how far apart the kept strands
    /// are, and a sparse coat thinned to it shows them one by one. A coat whose
    /// whole cast lays too few layers to spare any is cast whole.
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
