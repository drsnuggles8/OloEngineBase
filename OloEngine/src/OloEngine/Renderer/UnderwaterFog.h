#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    // Underwater fog math — CPU mirror of the GLSL formula in
    // PostProcess_Underwater.glsl. Both sides MUST match exactly so that the
    // L1 contract test can pin the shader path against this header.
    //
    // Beer–Lambert exponential absorption: `T = exp(-density * viewDist)` is
    // the transmittance, and `mix(sceneColor, fogColor, 1 - T)` blends toward
    // the deep-water tint. The view-distance is clamped to a small floor so
    // pixels at the camera don't blow up the result.
    namespace UnderwaterFog
    {
        // Compute the underwater fog transmittance at the given view-space distance.
        // Returns 1.0 (no fog) at distance=0, approaches 0.0 as distance grows.
        // density is per-metre absorption (clamped to [0,10]); distance is in metres.
        [[nodiscard]] inline f32 ComputeTransmittance(f32 viewDistance, f32 density) noexcept
        {
            // Defensive clamps mirror what the GPU shader does: the UBO upload
            // path may have already clamped, but the math has to stay sane for
            // anything that lands in the test harness directly.
            if (!std::isfinite(viewDistance) || viewDistance < 0.0f)
                viewDistance = 0.0f;
            if (!std::isfinite(density) || density < 0.0f)
                density = 0.0f;
            const f32 d = std::clamp(density, 0.0f, 10.0f);
            return std::exp(-d * viewDistance);
        }

        // Blend the input scene colour toward the fog colour using transmittance.
        // Equivalent to `mix(sceneColor, fogColor, 1 - transmittance)`.
        [[nodiscard]] inline glm::vec3 Apply(const glm::vec3& sceneColor,
                                             const glm::vec3& fogColor,
                                             f32 viewDistance,
                                             f32 density) noexcept
        {
            const f32 t = ComputeTransmittance(viewDistance, density);
            return sceneColor * t + fogColor * (1.0f - t);
        }

        // Length of the portion of the segment [camPos -> worldPos] that lies
        // below the water plane (y < waterSurfaceY). This is the per-pixel
        // "how much water does this view ray pass through" used by the tone-map
        // fog so the waterline is handled per pixel (underwater part fogged,
        // above-water part clear) rather than as a whole-screen toggle.
        // MUST match the GLSL `underwaterSegmentLength` in PostProcess_ToneMap.glsl.
        [[nodiscard]] inline f32 UnderwaterSegmentLength(const glm::vec3& camPos,
                                                         const glm::vec3& worldPos,
                                                         f32 waterSurfaceY) noexcept
        {
            const bool camUnder = camPos.y < waterSurfaceY;
            const bool fragUnder = worldPos.y < waterSurfaceY;
            if (camUnder && fragUnder)
                return glm::length(worldPos - camPos);
            if (!camUnder && !fragUnder)
                return 0.0f;
            // Segment crosses the plane: find the crossing point.
            const f32 dy = worldPos.y - camPos.y;
            if (std::abs(dy) < 1e-6f)
                return 0.0f; // parallel to the plane and not both under (handled above)
            const f32 tCross = (waterSurfaceY - camPos.y) / dy; // in [0,1]
            const glm::vec3 crossPoint = camPos + (worldPos - camPos) * tCross;
            return camUnder ? glm::length(crossPoint - camPos)   // cam under, frag above
                            : glm::length(worldPos - crossPoint); // cam above, frag under
        }
    } // namespace UnderwaterFog
} // namespace OloEngine
