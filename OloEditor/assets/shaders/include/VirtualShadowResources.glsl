#ifndef OLO_VIRTUAL_SHADOW_RESOURCES_GLSL
#define OLO_VIRTUAL_SHADOW_RESOURCES_GLSL

// =============================================================================
// VirtualShadowResources.glsl — the resource declarations every VSM stage shares
// (issue #702). Constants and pure helpers live in VirtualShadowCommon.glsl;
// this file is what makes them addressable.
//
// The page table and the meta table are SSBOs, not R32UI images. That is the one
// structural deviation from the Timberdoodle reference and it is forced: its meta
// table is R64_UINT and needs int64 IMAGE atomics, which are an extension in GL
// 4.6. Directional-only page ownership fits in 32 bits (see the encoding in
// VirtualShadowCommon.glsl), and SSBO atomics are core — so the whole allocator
// runs on guaranteed functionality.
//
// A shader includes this and then declares only the extra buffers it needs.
// =============================================================================

#include "VirtualShadowCommon.glsl"

// C++ twin: VSM::ClipProjection / VSM::GlobalsUBO in
// OloEngine/src/OloEngine/Renderer/Shadow/VirtualShadowMap.h.
struct VSMClipProjection
{
    // The MATH flavour (raw on both backends) -- everything that projects a world
    // position and interprets the result itself reads this one.
    mat4 ViewProjection;
    // The RASTERIZER flavour (Vulkan y flip + z remap applied CPU-side). ONLY the
    // depth raster's gl_Position reads it; on GL the two are identical.
    mat4 ViewProjectionRaster;
    ivec2 PageOffset;
    ivec2 PrevPageOffset;
    ivec2 PageDelta;
    float HalfExtent;
    float TexelWorldSize;
};

layout(std140, binding = 80) uniform VirtualShadowGlobals
{
    VSMClipProjection u_VSMClips[VSM_CLIP_LEVELS];
    mat4 u_VSMInverseViewProjection;
    vec4 u_VSMLightDirection;
    vec4 u_VSMCameraPosition;
    vec4 u_VSMParams0; // x = clip0 half extent, y = clip selection bias, z = depth bias, w = normal bias
    vec4 u_VSMParams1; // x = softness, y = max shadow distance, z = physical resolution, w = physical page table res
    ivec4 u_VSMParams2; // x = enabled, y = debug mode, z = full invalidate, w = frame index
    ivec4 u_VSMParams3; // x = depth width, y = depth height, z = marking stride, w = unused
};

#define VSM_CLIP0_HALF_EXTENT      (u_VSMParams0.x)
#define VSM_CLIP_SELECTION_BIAS    (u_VSMParams0.y)
#define VSM_DEPTH_BIAS             (u_VSMParams0.z)
#define VSM_NORMAL_BIAS            (u_VSMParams0.w)
#define VSM_SOFTNESS               (u_VSMParams1.x)
#define VSM_MAX_SHADOW_DISTANCE    (u_VSMParams1.y)
#define VSM_PHYSICAL_RESOLUTION    (u_VSMParams1.z)
#define VSM_PHYSICAL_PAGE_TABLE_RES (int(u_VSMParams1.w))
#define VSM_ENABLED                (u_VSMParams2.x)
#define VSM_DEBUG_MODE             (u_VSMParams2.y)
#define VSM_FULL_INVALIDATE        (u_VSMParams2.z)

// Per-dispatch / per-draw scratch. Refilled immediately before each use, which is
// the documented pattern for this engine's compute parameter blocks (#691 Phase
// 7): GL re-uploads the bound buffer and the Vulkan arena mints a fresh address
// per SetData. Two consumers, disjoint in time:
//   .x — VSM_BuildHPB: the mip being written.
//      — VSM_Depth raster: the base of this batch's compacted instance run.
layout(std140, binding = 81) uniform VirtualShadowPass
{
    uvec4 u_VSMPassParams;
};

// Virtual page table — VSM_TOTAL_VIRTUAL_PAGES entries, one uint each.
#ifndef VSM_PAGE_TABLE_READONLY
layout(std430, binding = 58) coherent buffer VSMPageTable { uint b_PageTable[]; };
#else
layout(std430, binding = 58) readonly buffer VSMPageTable { uint b_PageTable[]; };
#endif

// Selects the clip level for a render-relative world position, using the shared
// heuristic. Every producer and consumer of a page must agree on this, so nobody
// reimplements it locally.
int vsmSelectClipLevel(vec3 worldPosRelative)
{
    float dist = length(worldPosRelative - u_VSMCameraPosition.xyz);
    return vsmClipLevelForDistance(dist, VSM_CLIP0_HALF_EXTENT, VSM_CLIP_SELECTION_BIAS);
}

// Projects a render-relative world position into a clip level. Returns the
// virtual UV in `uv` and the clip-space depth in `depth`; false when the point
// falls outside that level's frustum.
bool vsmProjectIntoClip(vec3 worldPosRelative, int clipLevel, out vec2 uv, out float depth)
{
    vec4 clipPos = u_VSMClips[clipLevel].ViewProjection * vec4(worldPosRelative, 1.0);
    vec3 ndc = clipPos.xyz / clipPos.w;
    uv = ndc.xy * 0.5 + 0.5;
    depth = ndc.z * 0.5 + 0.5;
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0))) &&
           depth >= 0.0 && depth <= 1.0;
}

// Virtual UV -> the wrapped page-table slot that currently owns it.
ivec2 vsmUVToWrappedPage(vec2 uv, int clipLevel)
{
    ivec2 virtualPage = ivec2(floor(uv * float(VSM_PAGE_TABLE_RESOLUTION)));
    virtualPage = clamp(virtualPage, ivec2(0), ivec2(VSM_PAGE_TABLE_MASK));
    return vsmWrapPage(virtualPage, u_VSMClips[clipLevel].PageOffset);
}

// Virtual UV + a resident page entry -> the texel in the physical pool.
ivec2 vsmPhysicalTexel(vec2 uv, uint pageEntry)
{
    ivec2 virtualTexel = ivec2(uv * float(VSM_VIRTUAL_RESOLUTION));
    virtualTexel = clamp(virtualTexel, ivec2(0), ivec2(VSM_VIRTUAL_RESOLUTION - 1));
    ivec2 inPage = virtualTexel & ivec2(VSM_PAGE_SIZE - 1);
    return vsmUnpackPhysicalPage(pageEntry) * VSM_PAGE_SIZE + inPage;
}

int vsmPhysicalPageIndex(ivec2 physicalPage)
{
    return physicalPage.y * VSM_PHYSICAL_PAGE_TABLE_RES + physicalPage.x;
}

#endif // OLO_VIRTUAL_SHADOW_RESOURCES_GLSL
