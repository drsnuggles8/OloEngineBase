#ifndef TERRAIN_HEIGHT_SAMPLING_GLSL
#define TERRAIN_HEIGHT_SAMPLING_GLSL

// A tessellation-evaluation shader has no implicit texture derivatives. Using
// texture() therefore selects mip zero even when a coarse terrain node places
// its vertices many heightmap texels apart. That undersamples the heightfield
// and turns sub-vertex detail into detached distant summits (issue #953).
//
// gl_TessLevelOuter[i] is the subdivision count for the edge opposite control
// point i. Divide each UV edge by its actual subdivision count, then convert
// the largest resulting footprint to source texels. This works for both the
// GPU-driven unit patches and the CPU chunk fallback; it describes the
// geometry that will really be emitted rather than guessing from distance.
float oloTerrainHeightMip(vec2 uv0, vec2 uv1, vec2 uv2)
{
    float edge0 = length(uv1 - uv2) / max(gl_TessLevelOuter[0], 1.0);
    float edge1 = length(uv2 - uv0) / max(gl_TessLevelOuter[1], 1.0);
    float edge2 = length(uv0 - uv1) / max(gl_TessLevelOuter[2], 1.0);
    float footprintTexels = max(max(edge0, edge1), edge2) * float(max(u_HeightmapResolution, 1));
    float maxMip = floor(log2(float(max(u_HeightmapResolution, 1))));
    return clamp(log2(max(footprintTexels, 1.0)), 0.0, maxMip);
}

float oloTerrainFilteredHeight(vec2 uv, float mip)
{
    return textureLod(u_TerrainHeightmap, uv, mip).r;
}

#endif // TERRAIN_HEIGHT_SAMPLING_GLSL
