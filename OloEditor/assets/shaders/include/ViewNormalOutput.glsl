#ifndef VIEW_NORMAL_OUTPUT_GLSL
#define VIEW_NORMAL_OUTPUT_GLSL

// =============================================================================
// ViewNormalOutput.glsl — what scene attachment 2 stores for a surface normal
// on the forward paths: the octahedral VIEW normal SSAO, GTAO and the sphere
// proxies read (issue #1452).
//
// Shared by the PBR colour pass and the DepthNormalPrepass* programs (through
// ForwardShadingNormal.glsl) and by the foliage depth-normal prepass programs
// (issue #1474), which have no PBR material block and so cannot include
// ForwardShadingNormal.glsl itself. Terrain_PBR, Terrain_Voxel and
// Terrain_VoxelGreedy still carry private copies of the same encode.
// Needs nothing from the includer.
// =============================================================================

// Octahedral encode: unit normal -> RG16F [-1,1]^2.
vec2 oloOctEncodeViewNormal(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// What scene attachment 2 stores for a world-space normal.
vec2 oloForwardViewNormalOutput(mat4 view, vec3 worldNormal)
{
    return oloOctEncodeViewNormal(normalize(mat3(view) * worldNormal));
}

#endif // VIEW_NORMAL_OUTPUT_GLSL
