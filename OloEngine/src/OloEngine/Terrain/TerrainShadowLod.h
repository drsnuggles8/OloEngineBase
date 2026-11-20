#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

// =============================================================================
// Which mesh a terrain chunk casts its shadow from (issue #1721).
//
// A chunk mesh carries a vertex on every height texel. The main view does not
// draw the ground that finely except close up: it tessellates to
// TargetTriangleSize pixels, so a hundred metres away its triangles are several
// height texels long. A shadow caster finer than the surface that receives the
// shadow buys nothing, and it is not free: a 1024 px island is two million
// patches, and at a 600 m shadow range six of them cost 9-15 ms of terrain
// casters a frame.
//
// So a chunk casts from the coarsest of its meshes whose quads are still no
// larger than the triangles the main view draws that ground with. The rule is
// here as a pure function so it can be pinned without a GPU.
// =============================================================================
namespace OloEngine::TerrainShadowLod
{
    // A chunk's shadow meshes: level n spans 2^n of the chunk's quads per quad.
    // Level 0 is the chunk mesh itself. 64 quads a side leaves 8 at level 3.
    inline constexpr u32 kLevelCount = 4;

    // World metres one screen pixel spans per metre of view distance:
    // 2 tan(fovY / 2) / viewportHeight, read from a view-projection matrix.
    // Zero for an orthographic or degenerate view, which Select reads as "no
    // distance to judge by": such a view keeps every chunk at level 0.
    [[nodiscard]] inline f32 PixelSizePerMetre(const glm::mat4& viewProjection, const f32 viewportHeight)
    {
        if (!std::isfinite(viewportHeight) || viewportHeight <= 0.0f)
            return 0.0f;
        // clip.w varies with position only under a perspective projection.
        const f32 perspective = std::abs(viewProjection[0][3]) + std::abs(viewProjection[1][3]) + std::abs(viewProjection[2][3]);
        if (!(perspective > 1.0e-6f))
            return 0.0f;
        // Row 1 of projection * view is cot(fovY / 2) times a unit view axis.
        // The length drops the sign Vulkan's flipped Y puts on it.
        const f32 cotHalfFov = glm::length(glm::vec3(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1]));
        if (!std::isfinite(cotHalfFov) || cotHalfFov <= 0.0f)
            return 0.0f;
        return 2.0f / (cotHalfFov * viewportHeight);
    }

    // The level a chunk casts from. `distance` is from the camera to the
    // NEAREST point of the chunk, so the answer holds for all of it;
    // `quadSize` is the world size of one level-0 quad.
    [[nodiscard]] inline u32 Select(const f32 distance, const f32 quadSize, const f32 pixelSizePerMetre,
                                    const f32 targetTrianglePixels)
    {
        if (!std::isfinite(distance) || !std::isfinite(quadSize) || !std::isfinite(pixelSizePerMetre) ||
            !std::isfinite(targetTrianglePixels) || quadSize <= 0.0f || pixelSizePerMetre <= 0.0f ||
            targetTrianglePixels <= 0.0f)
        {
            return 0u;
        }
        // The triangles the main view draws this ground with, in metres.
        const f32 receiverTriangle = std::max(distance, 0.0f) * pixelSizePerMetre * targetTrianglePixels;
        u32 level = 0u;
        while (level + 1u < kLevelCount && quadSize * static_cast<f32>(2u << level) <= receiverTriangle)
            ++level;
        return level;
    }
} // namespace OloEngine::TerrainShadowLod
