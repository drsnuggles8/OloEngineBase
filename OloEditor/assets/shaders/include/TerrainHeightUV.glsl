// =============================================================================
// Where a tile UV reads the terrain height map (issue #1721).
//
// A height map is a grid of SAMPLES, not of areas. Its N texels along an axis
// are N heights: the first on one edge of the tile, the last on the other,
// N - 1 intervals apart. That is how the CPU reads it (TerrainData::GetHeightAt
// maps [0,1] onto the texel index range [0, N-1]), what the collision shape and
// the chunk meshes are built from, and what the picker transcribes.
//
// A texture fetch reads a texel as an AREA centred half a texel in from the
// edge: N texels span N intervals. Handing a tile UV straight to textureLod
// therefore drew the ground (uv - 0.5) texels away from the ground the CPU
// stands things on: nothing at the middle of a tile, half a texel at its edges.
// On a 256 px map over 260 m that is half a metre sideways, and on a slope as
// much up or down, between the drawn ground and the collision under it.
//
// oloTerrainHeightUV returns the texture coordinate whose bilinear fetch IS the
// CPU's lerp between samples. It holds at every mip of a power-of-two map: mip
// m's texel j is the mean of base samples j * 2^m .. (j + 1) * 2^m - 1, centred
// at j * 2^m + (2^m - 1) / 2 in sample units, so the coordinate that reads
// sample position p is (p + 0.5) / N whatever the mip.
//
// EVERY fetch of u_TerrainHeightmap by a tile UV goes through this
// (TerrainHeightSampleConventionTest counts them). The splatmaps and the macro
// map do not: those are painted areas, and a texel centre is where their value
// belongs.
// =============================================================================

#ifndef OLO_TERRAIN_HEIGHT_UV_GLSL
#define OLO_TERRAIN_HEIGHT_UV_GLSL

// `resolution` is the height map's base size in texels along the axis.
vec2 oloTerrainHeightUV(vec2 uv, float resolution)
{
    float n = max(resolution, 1.0);
    return (uv * (n - 1.0) + 0.5) / n;
}

#endif // OLO_TERRAIN_HEIGHT_UV_GLSL
