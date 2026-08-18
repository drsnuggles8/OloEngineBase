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

layout(std140, binding = 81) uniform VirtualShadowGlobals
{
    VSMClipProjection u_VSMClips[VSM_CLIP_LEVELS];
    mat4 u_VSMInverseViewProjection;
    vec4 u_VSMLightDirection;
    vec4 u_VSMCameraPosition;
    vec4 u_VSMParams0; // x = clip0 half extent, y = clip selection bias, z = depth bias, w = normal bias
    vec4 u_VSMParams1; // x = softness, y = max shadow distance, z = physical resolution, w = physical page table res
    ivec4 u_VSMParams2; // x = enabled, y = debug mode, z = full invalidate, w = frame index
    ivec4 u_VSMParams3; // x = depth width, y = depth height, z = marking stride, w = unused
    // Local lights (issue #703). LayerCount is the high-water mark of the layer
    // pool (what the cull and the HPB build dispatch over); LightCount is how
    // many entries of b_LocalLightHead are valid (what the MARKER walks) — the
    // two differ because a point light spends six layers on one light.
    ivec4 u_VSMParams4; // x = local enabled, y = local light count, z = local layer count, w = directional enabled
    vec4 u_VSMParams5;  // x = local detail bias, y = local depth bias (metres), z/w unused
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
#define VSM_LOCAL_ENABLED          (u_VSMParams4.x)
#define VSM_LOCAL_LIGHT_COUNT      (u_VSMParams4.y)
#define VSM_LOCAL_LAYER_COUNT      (u_VSMParams4.z)
#define VSM_DIRECTIONAL_ENABLED    (u_VSMParams4.w)
#define VSM_LOCAL_DETAIL_BIAS      (u_VSMParams5.x)
#define VSM_LOCAL_DEPTH_BIAS_METERS (u_VSMParams5.y)

// Per-dispatch / per-draw scratch. Refilled immediately before each use, which is
// the documented pattern for this engine's compute parameter blocks (#691 Phase
// 7): GL re-uploads the bound buffer and the Vulkan arena mints a fresh address
// per SetData. Two consumers, disjoint in time:
//   .x — VSM_BuildHPB: the mip being written.
//      — VSM_Depth raster: the base of this batch's compacted instance run.
layout(std140, binding = 82) uniform VirtualShadowPass
{
    uvec4 u_VSMPassParams;
};

// Virtual page table — VSM_TOTAL_PAGE_TABLE_ENTRIES entries, one uint each:
// the directional clip levels first, then the local-light layers (issue #703).
#ifndef VSM_PAGE_TABLE_READONLY
layout(std430, binding = 68) coherent buffer VSMPageTable { uint b_PageTable[]; };
#else
layout(std430, binding = 68) readonly buffer VSMPageTable { uint b_PageTable[]; };
#endif

// One local-light LAYER: a point light's cube face, or a whole spot light.
// C++ twin: VSM::LocalLight.
struct VSMLocalLight
{
    // Same two flavours, same split, as VSMClipProjection above (ADR 0011 (59)):
    // the raw one for anything that projects and then interprets the result, the
    // adjusted one for gl_Position alone.
    mat4 ViewProjection;
    mat4 ViewProjectionRaster;
    vec4 PositionRange; // xyz = render-relative light position, w = range
    // x = near plane, y = far plane (= range), z = face index (0..5; 0 for a
    // spot), w = kind: 0 unused, 1 spot, 2 point.
    vec4 Params;
};

// The local-light working set. ONE buffer with a fixed header and one unsized
// tail, because std430 allows only the last member to be unsized.
//
// b_LocalRasterMip is GPU-WRITTEN (atomicMin, by the local HPB build) and read
// by the local raster's VERTEX stage: it is the finest page-table mip that has
// any dirty page in that layer this frame, and therefore the resolution the
// layer has to be rasterized at. Rasterizing every layer at mip 0 would be
// correct and 1024x too expensive for a distant light — which is the cost this
// issue exists to remove.
// ONE declaration with a macro'd qualifier, not two #ifdef'd copies. The page
// table above is spelled the other way and this block was too for about an hour,
// during which the two copies had already drifted by a member — which is a
// silent std430 layout change for every consumer that took the shorter branch.
#ifdef VSM_LOCAL_LIGHTS_READONLY
#define VSM_LOCAL_LIGHTS_QUALIFIER readonly
#else
#define VSM_LOCAL_LIGHTS_QUALIFIER coherent
#endif

layout(std430, binding = 78) VSM_LOCAL_LIGHTS_QUALIFIER buffer VSMLocalLights
{
    uint b_LocalRasterMip[VSM_MAX_LOCAL_LAYERS];
    // Packed (layerBase << 1) | isPoint, compacted so the marker walks LIGHTS
    // without stepping over the holes a per-light-stable layer assignment leaves.
    uint b_LocalLightHead[VSM_MAX_LOCAL_LAYERS];
    // 1 = flush every page of this layer this frame. Set by the CPU when a light
    // moved, changed range/cone, or lost its layers to another light — the local
    // analogue of VSM_FULL_INVALIDATE, but per layer, because one light moving
    // must not cost the other 250 layers their cache.
    uint b_LocalLayerInvalidate[VSM_MAX_LOCAL_LAYERS];
    VSMLocalLight b_LocalLights[];
};

#define VSM_LOCAL_HEAD_LAYER(h) (int((h) >> 1))
#define VSM_LOCAL_HEAD_IS_POINT(h) (((h) & 1u) != 0u)

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

// --- Local-light layers (issue #703) -----------------------------------------
//
// The local twins of the three helpers above. Two structural differences from
// the directional ones, both deliberate:
//
//   * NO WRAPPING. Toroidal addressing exists so a clip frustum can scroll under
//     its cached pages; a light's face does not scroll — it either stands still
//     (and every page stays valid) or the light moves (and the whole layer is
//     invalidated). So a local page's slot IS its virtual page.
//   * A MIP, not a level. The layer is the light face; the mip is how finely it
//     is being resolved this frame, chosen per texel by vsmLocalMipForDistances.
//
// `viewDistance` comes back as clipPos.w, which for these perspective faces is
// exactly the distance along the face axis. The depth bias needs it (see
// vsmLocalDepthBias) and deriving it from the radial distance instead would
// under-bias the face corners by up to 3x — acne in a ring, which reads as a
// filtering problem rather than a bias one.
// THE UV IS RANGE-TESTED WITH A TOLERANCE, THEN CLAMPED — and it is worth being
// precise about why it is neither a plain test nor a plain clamp, because both
// simpler versions are wrong in a different place.
//
//   * A PLAIN TEST breaks the CUBE SEAMS. A point light's face is picked by
//     dominant axis (vsmCubeFace), which puts a point exactly on the boundary at
//     |x| == |y|, and floating point then decides whether its uv reads 0.9999999
//     or 1.0000001. Rejecting the second makes the marker skip the page and the
//     sampler skip the read, and since "no page" means LIT, the artefact is a
//     one-texel unshadowed line along all twelve cube edges.
//   * A PLAIN CLAMP breaks SPOT LIGHTS. A spot has no face selection, so a point
//     inside its range but outside its CONE would clamp onto the cone edge and
//     mark — and sample — a page it has no business touching. The shading is
//     saved by the cone falloff being zero out there, but the marking is not:
//     a spot would request the edge pages of a whole sphere of radius `range`.
//
// So: reject anything meaningfully outside, clamp what is only outside by
// rounding. Both the marker and the sampler call this, which is what keeps their
// answers identical.
bool vsmProjectIntoLocal(vec3 worldPosRelative, int layer, out vec2 uv, out float depth, out float viewDistance)
{
    vec4 clipPos = b_LocalLights[layer].ViewProjection * vec4(worldPosRelative, 1.0);
    viewDistance = clipPos.w;
    uv = vec2(0.0);
    depth = 0.0;
    if (clipPos.w <= 0.0)
        return false; // behind the light's near plane

    vec3 ndc = clipPos.xyz / clipPos.w;
    vec2 rawUV = ndc.xy * 0.5 + 0.5;

    // Wide enough to swallow the seam's rounding by orders of magnitude, narrow
    // enough that a point outside a spot cone is still rejected: one part in a
    // thousand of a face is a third of a mip-0 TEXEL.
    const float kEdgeTolerance = 1.0e-3;
    if (any(lessThan(rawUV, vec2(-kEdgeTolerance))) || any(greaterThan(rawUV, vec2(1.0 + kEdgeTolerance))))
        return false;

    // 1.0 - 2^-20: the largest float below 1, so `floor(uv * mipRes)` cannot
    // reach mipRes for any mip this table uses.
    uv = clamp(rawUV, vec2(0.0), vec2(0.99999905));
    depth = ndc.z * 0.5 + 0.5;
    // Depth IS tested outright: past the light's range (far plane) or inside its
    // near plane there is genuinely nothing stored, and reading the clamped edge
    // there would compare against an unrelated occluder.
    return depth >= 0.0 && depth <= 1.0;
}

ivec2 vsmLocalUVToPage(vec2 uv, int mip)
{
    int mipRes = VSM_LOCAL_PAGE_TABLE_RESOLUTION >> mip;
    return clamp(ivec2(floor(uv * float(mipRes))), ivec2(0), ivec2(mipRes - 1));
}

ivec2 vsmLocalPhysicalTexel(vec2 uv, int mip, uint pageEntry)
{
    int res = VSM_LOCAL_VIRTUAL_RESOLUTION >> mip;
    ivec2 virtualTexel = clamp(ivec2(uv * float(res)), ivec2(0), ivec2(res - 1));
    ivec2 inPage = virtualTexel & ivec2(VSM_PAGE_SIZE - 1);
    return vsmUnpackPhysicalPage(pageEntry) * VSM_PAGE_SIZE + inPage;
}

// The depth bias, authored in METRES and converted here to the [0,1] depth the
// face's perspective projection produces at this distance. It has to be a
// conversion rather than a constant: a perspective depth buffer's metre-per-unit
// scale falls off as 1/d², so one NDC number is either useless at the near plane
// or a peter-pan at the far one, across a range that varies per light.
//
//   depth01(d) = 0.5 + 0.5*((f+n)/(f-n) - 2fn/((f-n) d))
//   d(depth01)/dd = f*n / ((f-n) d²)
float vsmLocalDepthBias(int layer, float viewDistance)
{
    float n = b_LocalLights[layer].Params.x;
    float f = b_LocalLights[layer].Params.y;
    float d = max(viewDistance, max(n, 1e-4));
    return (f * n * max(VSM_LOCAL_DEPTH_BIAS_METERS, 0.0)) / (max(f - n, 1e-4) * d * d);
}

#endif // OLO_VIRTUAL_SHADOW_RESOURCES_GLSL
