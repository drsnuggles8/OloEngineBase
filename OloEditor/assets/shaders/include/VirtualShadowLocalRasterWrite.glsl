#ifndef OLO_VIRTUALSHADOWLOCALRASTER_WRITE_GLSL
#define OLO_VIRTUALSHADOWLOCALRASTER_WRITE_GLSL
#include "VirtualShadowPhysicalImage.glsl"

void vsmWriteLocalRasterDepth(uint layerInput, uint mipInput)
{
    OLO_VSM_PHYSICAL_IMAGE();
    int rasterMip = int(mipInput);
    int rasterRes = VSM_LOCAL_VIRTUAL_RESOLUTION >> rasterMip;

    ivec2 texel = ivec2(gl_FragCoord.xy);

#ifdef OLO_VULKAN
    // The same one-line fork, and the same reasoning, as the directional raster
    // stage — but against THIS instance's face resolution, not the global
    // virtual one. gl_FragCoord's origin is bottom-left on GL and top-left on
    // Vulkan, the raster-flavour projection adds a y flip on Vulkan, and the two
    // compose to `y_vulkan = rasterRes - y_gl` because the sub-rect map is what
    // decides how many pixels the face covers. Using VSM_LOCAL_VIRTUAL_RESOLUTION
    // here instead would be right only at mip 0 and would mirror every coarser
    // layer about a line outside its own footprint — i.e. write nothing at all.
    texel.y = (rasterRes - 1) - texel.y;
#endif

    if (any(lessThan(texel, ivec2(0))) || any(greaterThanEqual(texel, ivec2(rasterRes))))
        return; // outside this instance's sub-rect — see the header

    int layer = int(layerInput);
    uint depthBits = floatBitsToUint(gl_FragCoord.z);

    for (int mip = rasterMip; mip < VSM_LOCAL_MIP_COUNT; ++mip)
    {
        ivec2 mipTexel = texel >> (mip - rasterMip);
        ivec2 page = mipTexel >> VSM_PAGE_SIZE_LOG2;

        uint entry = b_PageTable[vsmLocalPageIndex(layer, mip, page)];
        // Both tests, same as the directional stage: nothing is backing an
        // unallocated page, and a non-dirty page already holds valid texels —
        // skipping it IS the caching.
        if (!vsmPageIsAllocated(entry) || !vsmPageIsDirty(entry))
            continue;

        ivec2 inPage = mipTexel & ivec2(VSM_PAGE_SIZE - 1);
        imageAtomicMin(u_VSMPhysicalPages, vsmUnpackPhysicalPage(entry) * VSM_PAGE_SIZE + inPage, depthBits);
    }
}


#endif
