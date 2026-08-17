#ifndef OLO_VIRTUAL_SHADOW_RASTER_STAGE_GLSL
#define OLO_VIRTUAL_SHADOW_RASTER_STAGE_GLSL

// =============================================================================
// VirtualShadowRasterStage.glsl — the VSM fragment stage (issue #702)
//
// THE INDIRECTION HAPPENS HERE, and this is the design decision the whole system
// rests on. A virtual shadow map cannot use a fixed-function depth attachment:
// the texel a fragment belongs to is decided by a page-table lookup, and the
// rasterizer has already committed to a screen position by the time we know it.
// So the pass rasterizes into the clip level's full VSM_VIRTUAL_RESOLUTION²
// viewport with NO depth attachment, and each fragment resolves its own page and
// does an imageAtomicMin into the physical pool.
//
// R32UI, not a float image: imageAtomicMin has no float form. Non-negative IEEE
// floats order identically to their bit patterns and the clip projections are
// orthographic over a [0,1] depth range, so the raw bits are a valid comparison
// key — this is not a trick that happens to work for typical values.
//
// The two early-outs are what make a cached frame cheap:
//   * page not ALLOCATED — nothing is backing this region, so there is nowhere to
//     write. (The sampler falls back to a coarser clip level for these.)
//   * page not DIRTY — it already holds valid texels from an earlier frame.
//     Overwriting them would be correct but pointless; skipping is the caching.
//
// VSM_CullCasters removes most of the geometry that would land on non-dirty pages
// before it is ever submitted. This is the backstop for the rest: a caster whose
// bounds overlap a dirty page still rasterizes fragments over its non-dirty
// neighbours, and those must not be written.
// =============================================================================

#include "VirtualShadowResources.glsl"

layout(r32ui, binding = 0) uniform coherent uimage2D u_VSMPhysicalPages;

layout(location = 0) flat in uint v_VSMClipLevel;

void main()
{
    // The viewport IS the virtual texture, so gl_FragCoord.xy is a virtual texel.
    ivec2 virtualTexel = ivec2(gl_FragCoord.xy);

#ifdef OLO_VULKAN
    // THE ONE BACKEND FORK IN THE WHOLE SYSTEM, and it is one line because of
    // where it is placed. gl_FragCoord's origin is bottom-left on GL and top-left
    // on Vulkan, and the clip projection additionally carries a y flip on Vulkan
    // (VSMClipProjection::ViewProjectionRaster). The two compose to
    //
    //     fragCoord.y_vulkan = VSM_VIRTUAL_RESOLUTION - fragCoord.y_gl
    //
    // so undoing it here makes `virtualTexel` name the SAME virtual texel on both
    // backends. That is the property worth having: the physical pool's contents
    // become backend-identical, so nothing downstream needs a fork — not the page
    // lookup, not the sampler in the lit pass, and not a golden image.
    //
    // Doing it the other way round (letting the pool mirror and flipping the
    // SAMPLE instead, which is how the CSM path handles its own row flip) would
    // put the fork in three consumers rather than one, and every one of them
    // would fail silently.
    virtualTexel.y = (VSM_VIRTUAL_RESOLUTION - 1) - virtualTexel.y;
#endif

    if (any(lessThan(virtualTexel, ivec2(0))) || any(greaterThanEqual(virtualTexel, ivec2(VSM_VIRTUAL_RESOLUTION))))
        return;

    int clipLevel = int(v_VSMClipLevel);
    ivec2 virtualPage = virtualTexel >> VSM_PAGE_SIZE_LOG2;
    ivec2 wrappedPage = vsmWrapPage(virtualPage, u_VSMClips[clipLevel].PageOffset);

    uint entry = b_PageTable[vsmPageIndex(clipLevel, wrappedPage)];
    if (!vsmPageIsAllocated(entry) || !vsmPageIsDirty(entry))
        return;

    ivec2 inPage = virtualTexel & ivec2(VSM_PAGE_SIZE - 1);
    ivec2 physicalTexel = vsmUnpackPhysicalPage(entry) * VSM_PAGE_SIZE + inPage;

    imageAtomicMin(u_VSMPhysicalPages, physicalTexel, floatBitsToUint(gl_FragCoord.z));
}

#endif // OLO_VIRTUAL_SHADOW_RASTER_STAGE_GLSL
