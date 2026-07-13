// VirtualDebugViz.glsl — shared debug-visualization writes for the virtualized-
// geometry raster paths (issue #629). Included by the fragment stages of
// VirtualMeshGBuffer.glsl (hardware) and VirtualVisibilityResolve.glsl
// (software). The caller resolves the per-pixel cluster index + LOD level and
// calls WriteVirtualDebug(); this writes into the registry's debug images, which
// VirtualGeometryPass binds only while a debug mode is active. Fragment-stage
// only (uses gl_FragCoord). No-op when u_DebugMode == 0.

layout(rgba8, binding = 0) uniform writeonly image2D u_VirtualDebugColor;
layout(r32ui, binding = 1) uniform uimage2D u_VirtualDebugCount;

// A scalar uniform must live in a block for the Vulkan-SPIR-V graphics path
// (binding 50 = UBO_VIRTUAL_DEBUG). 0 = off, 1 = cluster id, 2 = lod, 3 = overdraw.
layout(std140, binding = 50) uniform VirtualDebugInfo {
    int u_DebugMode;
    int _vdbgPad0; int _vdbgPad1; int _vdbgPad2;
};

// Distinct-ish colour per integer id (Knuth multiplicative hash -> RGB).
vec3 VirtualDebugHashColor(uint id)
{
    uint h = id * 2654435761u;
    return vec3(float((h >> 0u) & 0xFFu), float((h >> 8u) & 0xFFu), float((h >> 16u) & 0xFFu)) / 255.0;
}

// Colour ramp for LOD levels (finest = green ... coarsest = red), cycling if deep.
vec3 VirtualDebugLodColor(uint lod)
{
    const vec3 ramp[6] = vec3[6](
        vec3(0.1, 0.9, 0.2), vec3(0.5, 0.9, 0.1), vec3(0.9, 0.9, 0.1),
        vec3(0.95, 0.6, 0.1), vec3(0.95, 0.3, 0.1), vec3(0.9, 0.1, 0.1));
    return ramp[lod % 6u];
}

void WriteVirtualDebug(uint clusterIndex, uint lod)
{
    if (u_DebugMode == 0)
        return;
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (u_DebugMode == 3)
    {
        // Overdraw: accumulate a per-pixel fragment count, colorized later by
        // VirtualDebugColorize.comp.
        imageAtomicAdd(u_VirtualDebugCount, px, 1u);
    }
    else
    {
        vec3 c = (u_DebugMode == 1) ? VirtualDebugHashColor(clusterIndex) : VirtualDebugLodColor(lod);
        imageStore(u_VirtualDebugColor, px, vec4(c, 1.0));
    }
}
