#ifndef OLO_VIRTUALSHADOWRASTER_WRITE_GLSL
#define OLO_VIRTUALSHADOWRASTER_WRITE_GLSL
#include "VirtualShadowPhysicalImage.glsl"

void vsmWriteRasterDepth(uint clipLevelInput)
{
    OLO_VSM_PHYSICAL_IMAGE();
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

    int clipLevel = int(clipLevelInput);
    ivec2 virtualPage = virtualTexel >> VSM_PAGE_SIZE_LOG2;
    ivec2 wrappedPage = vsmWrapPage(virtualPage, u_VSMClips[clipLevel].PageOffset);

    uint entry = b_PageTable[vsmPageIndex(clipLevel, wrappedPage)];
    if (!vsmPageIsAllocated(entry) || !vsmPageIsDirty(entry))
        return;

    ivec2 inPage = virtualTexel & ivec2(VSM_PAGE_SIZE - 1);
    ivec2 physicalTexel = vsmUnpackPhysicalPage(entry) * VSM_PAGE_SIZE + inPage;

    imageAtomicMin(u_VSMPhysicalPages, physicalTexel, floatBitsToUint(gl_FragCoord.z));
}


#endif
