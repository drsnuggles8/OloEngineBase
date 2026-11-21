#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Terrain/TerrainLodMetric.h"

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
//
// "No larger than the main view's" has a ceiling the pixel target does not
// know about: the main view draws a node as TerrainLodMetric::kNodeQuads quads
// a side whatever its level, so its quads are never larger than the ROOT
// node's. Past the distance where the root's quads are under the pixel target
// the receiver stops coarsening, and so must the caster. A tile is
// ceil(resolution / 64) chunks a side, so from 449 samples up (8 chunks) a
// chunk's coarsest mesh is no coarser than the root's quads and the ceiling
// never binds; on a smaller map it is what keeps the caster from being a
// blunter terrain than the one drawn. A 129 px map (3 chunks: 2.7 m quads at
// the coarsest level against the root's 1 m) cast a 24 m rim's shadow 2.9 m
// short without it (TerrainShadowCasterEvidenceTest).
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
        // The main view's own scale (TerrainLodMetric), so "the triangles the
        // main view draws" is the quantity its quadtree splits by and not a
        // second derivation of it. That scale is pixels per metre at one metre,
        // per quad of a node; this is its reciprocal, per metre.
        const f32 quadScale = TerrainLodMetric::QuadProjectionScale(viewProjection, viewportHeight);
        if (quadScale <= 0.0f)
            return 0.0f;
        return 1.0f / (quadScale * static_cast<f32>(TerrainLodMetric::kNodeQuads));
    }

    // The largest quad the main view draws a tile of this size with: its root
    // node's. A tile that is not square takes the shorter side, the finer answer.
    [[nodiscard]] inline f32 CoarsestReceiverQuad(const f32 worldSizeX, const f32 worldSizeZ)
    {
        const f32 side = std::min(worldSizeX, worldSizeZ);
        if (!std::isfinite(side) || side <= 0.0f)
            return 0.0f;
        return side / static_cast<f32>(TerrainLodMetric::kNodeQuads);
    }

    // The level a chunk casts from. `distance` is from the camera to the
    // NEAREST point of the chunk, so the answer holds for all of it;
    // `quadSize` is the world size of one level-0 quad; `coarsestReceiverQuad`
    // is CoarsestReceiverQuad of the tile.
    [[nodiscard]] inline u32 Select(const f32 distance, const f32 quadSize, const f32 pixelSizePerMetre,
                                    const f32 targetTrianglePixels, const f32 coarsestReceiverQuad)
    {
        if (!std::isfinite(distance) || !std::isfinite(quadSize) || !std::isfinite(pixelSizePerMetre) ||
            !std::isfinite(targetTrianglePixels) || !std::isfinite(coarsestReceiverQuad) || quadSize <= 0.0f ||
            pixelSizePerMetre <= 0.0f || targetTrianglePixels <= 0.0f || coarsestReceiverQuad <= 0.0f)
        {
            return 0u;
        }
        // The triangles the main view draws this ground with, in metres.
        const f32 receiverTriangle =
            std::min(std::max(distance, 0.0f) * pixelSizePerMetre * targetTrianglePixels, coarsestReceiverQuad);
        u32 level = 0u;
        while (level + 1u < kLevelCount && quadSize * static_cast<f32>(2u << level) <= receiverTriangle)
            ++level;
        return level;
    }
} // namespace OloEngine::TerrainShadowLod
