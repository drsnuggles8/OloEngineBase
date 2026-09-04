#ifndef VIRTUAL_DRAW_INFO_GLSL
#define VIRTUAL_DRAW_INFO_GLSL

// =============================================================================
// VirtualDrawInfo.glsl — the ONE GLSL spelling of the per-draw info UBO shared
// by every virtual-geometry raster / resolve / shadow pipeline
// (binding 49 == ShaderBindingLayout::UBO_VIRTUAL_DRAW), mirroring
// VirtualDrawInfoGpu in Renderer/VirtualGeometry/VirtualMeshGpuData.h.
//
// Why this is an include and not four private copies: this block is the exact
// shape ShaderUBOSizeConsistency.CrossShaderUBOMemberOffsetsAgree exists to
// police. Before #813 the G-Buffer stage declared offsets 8/12 as `_vdPad0` /
// `_vdPad1` while the resolve and shadow stages declared those same bytes as
// the visbuffer viewport size — two different structs on one binding, with the
// pad names the only reason the disagreement stayed invisible. The mesh path
// needed two more per-draw values; claiming the pad slots for them is what made
// the split visible. Giving every field its own offset in one shared block is
// what makes it impossible: identical text in every consumer cannot disagree.
//
// Every pass uploads all 32 bytes; a pass with no use for a field writes 0.
// Adding a field means adding it HERE and growing the C++ mirror with it.
layout(std140, binding = 49) uniform VirtualDrawInfo {
    // 0 — instance every draw/workgroup of this call belongs to.
    uint u_VirtualInstanceIndex;
    // 4 — the instance's global visible-slot base for the current phase region
    //     (what the cull wrote into each MDI command's BaseInstance).
    uint u_VirtualCommandBase;
    // 8/12 — visbuffer dimensions, read by the software-raster resolve and the
    //        shadow depth stage; the G-Buffer stages write them but never read.
    uint u_VirtualViewportWidth;
    uint u_VirtualViewportHeight;
    // 16 — the instance's VirtualDrawArgs slot for the current phase, read by
    //      the TASK stage as its launch count (#813). MDI stages never read it.
    uint u_VirtualArgsSlot;
    // 20 — the instance's ClusterCount: the task stage's launch clamp, mirroring
    //      the MDI arm's maxDrawCount bound against a corrupt GPU-written count.
    uint u_VirtualMaxClusters;
    // 24 — first ELEMENT of the baked lightmap uv2 tail inside the vertex
    //      arena at binding 39 (issue #867). The uv2 has no binding of its own:
    //      the SSBO namespace is full below Mesa's 80 ceiling, and the one
    //      reusable number — SSBO_BONE_PULL (63) — resolves from the draw's VAO
    //      streams on Vulkan, which the mesh-shader route does not have. So it
    //      rides that arena as a packed tail, four uv2 pairs to a 32-byte
    //      element. ZERO means "this arena carries no uv2", and every reader
    //      must treat it as don't-fetch: a buffer-device-address read past the
    //      arena has no bounds (ADR 0011 amendment (89)).
    uint u_VirtualLightmapUVBase;
    // 28 — std140 rounds the block to a 16-byte multiple; the pad is explicit so
    //      the C++ mirror can be a plain struct with the same size.
    uint u_VirtualDrawInfoPad1;
};

// ---- the packed uv2 tail: ONE spelling of the addressing math --------------
//
// Split in two so neither half needs the vertex SSBO or the VirtualGpuVertex
// type to be declared first — this include is pulled in by stages that declare
// binding 39 and by stages that do not, and an include-order dependency here
// would be a compile error in one pipeline and silence in the rest.
//
// `globalVertexIndex` is the SAME index the vertex fetch uses
// (cluster.VertexBase + local, or gl_VertexIndex): a page's vertices start at
// slot-local 0 and the arena's slot capacity is 4-aligned, so the element and
// the lane fall out of the global index with no per-page fixup.
//
// Callers MUST check `u_VirtualLightmapUVBase != 0u` and the instance's
// `LightmapScaleOffset.x > 0.0` before fetching — see the field note above.
uint oloVirtualLightmapUVElement(uint globalVertexIndex)
{
    return u_VirtualLightmapUVBase + (globalVertexIndex >> 2u);
}

// Lanes 0..3 of an element are (PositionU.xy, PositionU.zw, NormalV.xy,
// NormalV.zw). The field names belong to the vertex layout this region borrows;
// the sixteen bytes hold four uv2 pairs and nothing else.
vec2 oloVirtualLightmapUVLane(vec4 elementLow, vec4 elementHigh, uint globalVertexIndex)
{
    uint lane = globalVertexIndex & 3u;
    vec4 pair = ((lane & 2u) == 0u) ? elementLow : elementHigh;
    return ((lane & 1u) == 0u) ? pair.xy : pair.zw;
}

#endif // VIRTUAL_DRAW_INFO_GLSL
