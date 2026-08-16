#ifndef TERRAIN_GPU_DRIVEN_VERTEX_GLSL
#define TERRAIN_GPU_DRIVEN_VERTEX_GLSL

// =============================================================================
// The GPU-driven branch of the terrain vertex stages (issue #714).
//
// Included by Terrain_PBR / Terrain_GBuffer / Terrain_Depth's vertex stage. With
// `u_TerrainGpuDriven == 0` every one of them behaves exactly as before: the VBO
// carries baked TerrainChunk geometry and this file's helper is a no-op. With it
// set, the VBO is the shared unit grid (TerrainGPUQuadtree::GetSharedPatchMesh)
// drawn instanced, and each instance turns itself into one quadtree node here.
//
// Two things happen per vertex:
//
//   1. The unit grid coordinate is mapped into the node's rect in normalized
//      terrain space. Nothing else about the vertex changes — the tess_eval
//      stage still displaces Y from the heightmap at the UV computed here, so
//      LOD is purely a matter of which rect a patch covers.
//   2. If this vertex is on an edge whose neighbour is COARSER, its index along
//      that edge is snapped down to the neighbour's spacing. That is the whole
//      crack fix: the in-between vertices collapse onto the coarse neighbour's,
//      producing zero-area triangles instead of a gap, and both sides then
//      sample the heightmap at exactly the same points.
//
// The corners are load-bearing and cost nothing to keep: index 0 and index K are
// multiples of every 2^delta up to K, so no snapping can move them, which is
// what welds a patch's four corners to its neighbours regardless of the seam
// pattern.
// =============================================================================

#include "TerrainQuadtreeCommon.glsl"

// Terrain UBO (binding 10) — declared here so the vertex stage can read the
// GPU-driven mode flag. Same block the tess/fragment stages declare inline;
// keep the member list identical or the std140 layout forks per stage.
layout(std140, binding = 10) uniform TerrainParams {
    vec4 u_WorldSizeAndHeightScale;
    vec4 u_TerrainParams;
    int u_HeightmapResolution;
    int u_TerrainGpuDriven;
    int u_TerrainGpuGridRes;
    int _terrainPad2;
    vec4 u_TessFactors;
    vec4 u_TessFactors2;
    vec4 u_LayerTilingScales0;
    vec4 u_LayerTilingScales1;
    vec4 u_LayerBlendSharpness0;
    vec4 u_LayerBlendSharpness1;
};

// The GPU-built visible-node list: .x = packed (level, x, y), .y = packed seam
// deltas. One entry per drawn patch, indexed by the instance.
layout(std430, binding = 59) readonly buffer TerrainVisibleNodes {
    uvec2 b_TerrainVisibleNodes[];
};

void oloTerrainApplyGpuDrivenNode(inout vec3 position, inout vec2 texCoord, inout vec3 normal)
{
    if (u_TerrainGpuDriven == 0)
        return;

    uvec2 node = b_TerrainVisibleNodes[gl_InstanceIndex];
    uint packed = node.x;
    uint seams = node.y;
    uint level = oloTerrainNodeLevel(packed);
    uint nodeX = oloTerrainNodeX(packed);
    uint nodeY = oloTerrainNodeY(packed);

    int gridRes = u_TerrainGpuGridRes;
    // The patch VBO stores its unit grid coordinate in Position.xz, so rounding
    // recovers the exact integer index the CPU wrote — the snapping below is
    // integer arithmetic and must not inherit a float error.
    ivec2 gi = ivec2(round(position.xz * float(gridRes)));

    if (gi.x == 0)
        gi.y = oloTerrainSnapEdgeIndex(gi.y, oloTerrainSeamDelta(seams, OLO_TERRAIN_SEAM_NX));
    else if (gi.x == gridRes)
        gi.y = oloTerrainSnapEdgeIndex(gi.y, oloTerrainSeamDelta(seams, OLO_TERRAIN_SEAM_PX));

    if (gi.y == 0)
        gi.x = oloTerrainSnapEdgeIndex(gi.x, oloTerrainSeamDelta(seams, OLO_TERRAIN_SEAM_NZ));
    else if (gi.y == gridRes)
        gi.x = oloTerrainSnapEdgeIndex(gi.x, oloTerrainSeamDelta(seams, OLO_TERRAIN_SEAM_PZ));

    float span = 1.0 / float(1u << level);
    vec2 uv = (vec2(float(nodeX), float(nodeY)) + vec2(gi) / float(gridRes)) * span;

    texCoord = uv;
    // Y stays 0: the tess_eval stage overwrites it from the heightmap. It only
    // reads the incoming Y through the morph term, which the GPU-driven path
    // pins to 0 (TessFactors2.y) precisely because there is no coarse mesh
    // height to morph from here.
    position = vec3(uv.x * u_WorldSizeAndHeightScale.x, 0.0, uv.y * u_WorldSizeAndHeightScale.y);
    normal = vec3(0.0, 1.0, 0.0);
}

#endif // TERRAIN_GPU_DRIVEN_VERTEX_GLSL
