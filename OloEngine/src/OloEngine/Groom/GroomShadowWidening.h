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
} // namespace OloEngine
