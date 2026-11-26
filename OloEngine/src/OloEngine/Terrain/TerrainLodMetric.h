#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>

// =============================================================================
// The screen-space error a terrain quadtree node is split by (issue #1721).
//
// A node is drawn as a grid of kNodeQuads quads a side, whatever its level, and
// TerrainLODConfig::TargetTriangleSize is the on-screen size those quads should
// have, in pixels. So the error of a node is the size of ONE OF ITS QUADS:
//
//     pixels = (nodeWorldSize / kNodeQuads) * projectionScale / distance
//
// Both descents (TerrainQuadtree on the CPU, TerrainNodeSelect.comp on the GPU)
// take their projection scale from here, which is what keeps them selecting the
// same nodes; TerrainGPUQuadtreeParityTest holds them to it.
//
// What this replaced compared the WHOLE NODE's extent against the triangle
// target, and took its scale from viewProjection[1][1]. The first made every
// node split until it was a few pixels across, so every terrain in view was
// drawn at its deepest level at any distance: sixty-four times the detail the
// target asked for along each axis, and a cost that grew with the square of the
// height map's resolution. The second is the projection's cot(fovY/2) times the
// camera's up axis along world Y, so the scale fell as the camera pitched and
// reached zero looking straight down, where the whole terrain collapsed to its
// root node.
// =============================================================================
namespace OloEngine::TerrainLodMetric
{
    // Quads along one edge of a drawn node: TerrainGPUQuadtree's patch grid and
    // TerrainChunk's mesh, which static_assert against it where they are defined.
    inline constexpr u32 kNodeQuads = 64u;

    // Pixels one world metre spans at one metre of view distance, DIVIDED by
    // kNodeQuads: multiply by a node's world size over its distance for the
    // size of one of its quads on screen.
    //
    // cot(fovY / 2) is the length of the second row of projection * view, since
    // the view matrix's rows are unit vectors. Reading the single element
    // [1][1] instead multiplies in how far the camera's up axis leans from
    // world Y. The length also drops the sign Vulkan's flipped Y puts there.
    // Zero for a degenerate matrix or viewport: nothing then splits.
    [[nodiscard]] inline f32 QuadProjectionScale(const glm::mat4& viewProjection, const f32 viewportHeight)
    {
        if (!std::isfinite(viewportHeight) || viewportHeight <= 0.0f)
            return 0.0f;
        f32 verticalScale = glm::length(glm::vec3(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1]));
        // In a terrain-local VP both clip.y and perspective clip.w contain
        // the carrier's scale. The local camera distance already accounts for
        // it. Normalize by the depth row as in the perspective divide.
        const f32 depthScale = glm::length(glm::vec3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3]));
        if (depthScale > 0.0f)
            verticalScale /= depthScale;
        if (!std::isfinite(verticalScale) || verticalScale <= 0.0f)
            return 0.0f;
        return verticalScale * viewportHeight * 0.5f / static_cast<f32>(kNodeQuads);
    }
} // namespace OloEngine::TerrainLodMetric
