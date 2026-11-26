#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/WaterSurface.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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
                return 0.0f;                                    // parallel to the plane and not both under (handled above)
            const f32 tCross = (waterSurfaceY - camPos.y) / dy; // in [0,1]
            const glm::vec3 crossPoint = camPos + (worldPos - camPos) * tCross;
            return camUnder ? glm::length(crossPoint - camPos)    // cam under, frag above
                            : glm::length(worldPos - crossPoint); // cam above, frag under
        }

        // Whether the eye itself is in the water, for the fog: 0 in the air, 1 from
        // kEyeInWaterRamp under the surface drawn above it. `eyeDepth` is that
        // surface's height minus the eye's, waves included.
        //
        // This is NOT UnderwaterCaustics::EyeSubmersion, the lens weight. The wobble
        // is a look and eases in over a quarter metre. The fog is the water in front
        // of the eye, and an eye a hand's breadth under has all of it: eased like
        // the lens, a camera at the waterline saw two thirds of the way through the
        // sea to its bed (Water_Waterline.png holds that frame, from 8 cm under a
        // crest). The ramp is the width of a lens, so the crossing takes a few
        // frames and not one.
        // MUST match the GLSL `underwaterEyeInWater` in PostProcess_ToneMap.glsl.
        inline constexpr f32 kEyeInWaterRamp = 0.05f;
        [[nodiscard]] inline f32 EyeInWater(f32 eyeDepth) noexcept
        {
            if (!std::isfinite(eyeDepth))
                return 0.0f;
            return std::clamp(eyeDepth / kEyeInWaterRamp, 0.0f, 1.0f);
        }

        // How much of the underwater shading (caustics and fog) a pixel takes, given
        // the height of the water surface in front of it: the one the water pass
        // drew at that pixel, or the still plane where it drew none.
        //
        // A surface BELOW the eye is seen from above: the water shader has already
        // put its reflection and its own depth tint there, and fog laid over that
        // replaces the whole sea with one flat colour (which is what every camera
        // inside the flag's reach of the surface used to show). So an eye in the air
        // leaves such a pixel alone. An eye in the water has water between it and
        // everything and fogs it: `eyeInWater` is EyeInWater (above) of the eye's
        // depth.
        // A surface ABOVE the eye is a crest's underside, with water on this side of
        // it, and is always fogged.
        // MUST match the GLSL `underwaterSurfacePixelFogShare` in PostProcess_ToneMap.glsl.
        [[nodiscard]] inline f32 SurfacePixelFogShare(f32 cameraY, f32 surfaceY, f32 eyeInWater) noexcept
        {
            if (!std::isfinite(eyeInWater))
                eyeInWater = 0.0f;
            return cameraY >= surfaceY ? std::clamp(eyeInWater, 0.0f, 1.0f) : 1.0f;
        }

        // The height of a water tile's plane (its local y = 0) on the VERTICAL line
        // through a world position: where the surface is over, or under, an eye there.
        //
        // Not the foot of the perpendicular from the eye onto the plane, which is what
        // transforming the eye into the tile's space and zeroing its y gives. The two
        // are one point for a level tile, however it is moved, scaled or turned about
        // the vertical. For a tile tilted 45 degrees they are not: an eye at (1, 0.9)
        // has the surface 0.1 above it and the perpendicular's foot only 0.05 above.
        // `fallbackY` answers for a tile standing on edge, which has no height there.
        [[nodiscard]] inline f32 TilePlaneYAt(const glm::mat4& model, f32 worldX, f32 worldZ, f32 fallbackY) noexcept
        {
            const auto frame = WaterSurface::SurfaceFrame::FromModel(model);
            const f32 y = frame ? frame->RestHeight({ worldX, worldZ }) : fallbackY;
            return std::isfinite(y) ? y : fallbackY;
        }

        // A tilted tile's footprint is bounded at the intersection of the
        // vertical line through the eye with local Y=0.
        [[nodiscard]] inline bool TryGetTileFootprintGap(const glm::mat4& model, f32 width, f32 depth,
                                                         const glm::vec3& eye, f32& gap) noexcept
        {
            const auto frame = WaterSurface::SurfaceFrame::FromModel(model);
            if (!frame)
                return false;
            const glm::vec2 worldXZ(eye.x, eye.z);
            const glm::vec2 local = frame->LocalXZ(worldXZ);
            const glm::vec2 halfExtent(WaterSurface::SanitizeTileDimension(width) * 0.5f,
                                       WaterSurface::SanitizeTileDimension(depth) * 0.5f);
            if (!glm::all(glm::lessThanEqual(glm::abs(local), halfExtent)))
                return false;
            gap = frame->RestHeight(worldXZ) - eye.y;
            return std::isfinite(gap);
        }

    } // namespace UnderwaterFog
} // namespace OloEngine
