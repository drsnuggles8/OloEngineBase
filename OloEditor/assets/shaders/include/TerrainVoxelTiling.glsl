// =============================================================================
// The scale a voxel terrain's layer textures are sampled at (issue #1721).
//
// A layer's TilingScale is repeats across the terrain TILE. The heightfield
// surface samples in tile units, so on a tile that is not square one repeat is
// a different number of metres along X than along Z, and its side projections
// follow the axis they run along (include/TerrainLayerBlend.glsl,
// oloTerrainProjection). A voxel surface samples a world position in metres, so
// it needs the same two numbers: repeats per metre along world X and along
// world Z. With one, taken from X, a carved wall matched the ground beside it
// along one axis of a 2:1 tile and was half its scale along the other.
//
// The voxel draw uploads the tile's size in u_WorldSizeAndHeightScale.xy and
// the layers' TilingScale, unconverted, in u_LayerTilingScales0/1.
//
// Include after include/TerrainParamsBlock.glsl. Used by all four voxel surface
// shaders, with the projections applied the same way in each:
//   X-facing faces (y, z) * tiling.y   the Z axis' scale, as the surface's SideX
//   Y-facing faces (x, z) * tiling     the top projection: both
//   Z-facing faces (x, y) * tiling.x   the X axis' scale, as the surface's SideZ
// =============================================================================

#ifndef OLO_TERRAIN_VOXEL_TILING_GLSL
#define OLO_TERRAIN_VOXEL_TILING_GLSL

// (.x, .y) = repeats per metre along world (X, Z) for a layer that repeats
// `repeatsAcrossTile` times across the tile.
vec2 oloTerrainVoxelTiling(float repeatsAcrossTile)
{
    vec2 tileSize = u_WorldSizeAndHeightScale.xy;
    // No authored tiling, or a draw that uploaded no tile: a repeat every 10 m,
    // the default these shaders have always fallen back to.
    if (repeatsAcrossTile < 0.001 || min(tileSize.x, tileSize.y) < 0.001)
        return vec2(0.1);
    return vec2(repeatsAcrossTile) / tileSize;
}

#endif // OLO_TERRAIN_VOXEL_TILING_GLSL
