// =============================================================================
// The terrain's geometric normal, per PIXEL (issue #1721).
//
// The evaluation stage computes a normal per VERTEX, from height differences at
// the mip that matches the vertex spacing. That is the right normal for the
// mesh, and the wrong one to light with: its detail is the tessellation's, so
// relief lighting softens as a node coarsens with distance and steps when a
// node changes level. A height map holds the relief at every scale, so the
// fragment stage reads it at the scale of the PIXEL instead, and the lighting
// no longer knows what level of detail the geometry is drawn at.
//
// Shared by Terrain_PBR.glsl (Forward, Forward+) and Terrain_GBuffer.glsl
// (Deferred). Include after, in the fragment stage:
//   - include/TerrainParamsBlock.glsl (binding 10),
//   - the sampler declaration u_TerrainHeightmap (slot-based or OLO_BINDLESS).
// =============================================================================

#ifndef OLO_TERRAIN_PIXEL_NORMAL_GLSL
#define OLO_TERRAIN_PIXEL_NORMAL_GLSL

// Tile-local normal at `uv`. `uvDx` / `uvDy` are the screen-space gradients of
// the terrain UV, taken by the caller before any branch.
//
// The mip is the pixel's footprint in height texels, fractional, so the detail
// fades in continuously with distance instead of switching. The differences
// are taken one texel OF THAT MIP apart, the same stencil the evaluation stage
// uses at its own mip: a slope is a height difference over the distance it was
// measured across, and sampling a coarse mip at fine offsets would divide a
// blurred difference by too short a run.
vec3 oloTerrainPixelNormalLocal(vec2 uv, vec2 uvDx, vec2 uvDy)
{
    float resolution = float(max(u_HeightmapResolution, 1));
    float footprintTexels = max(length(uvDx), length(uvDy)) * resolution;
    float maxMip = floor(log2(resolution));
    float mip = clamp(log2(max(footprintTexels, 1.0)), 0.0, maxMip);
    float texel = u_TerrainParams.x * exp2(mip);

    float heightScale = u_WorldSizeAndHeightScale.z;
    float hL = textureLod(u_TerrainHeightmap, uv + vec2(-texel, 0.0), mip).r * heightScale;
    float hR = textureLod(u_TerrainHeightmap, uv + vec2( texel, 0.0), mip).r * heightScale;
    float hD = textureLod(u_TerrainHeightmap, uv + vec2(0.0, -texel), mip).r * heightScale;
    float hU = textureLod(u_TerrainHeightmap, uv + vec2(0.0,  texel), mip).r * heightScale;

    float dX = (hR - hL) / (2.0 * texel * max(u_WorldSizeAndHeightScale.x, 0.0001));
    float dZ = (hU - hD) / (2.0 * texel * max(u_WorldSizeAndHeightScale.y, 0.0001));
    return normalize(vec3(-dX, 1.0, -dZ));
}

#endif // OLO_TERRAIN_PIXEL_NORMAL_GLSL
