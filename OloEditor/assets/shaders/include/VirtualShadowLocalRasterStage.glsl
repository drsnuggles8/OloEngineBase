#ifndef OLO_VIRTUAL_SHADOW_LOCAL_RASTER_STAGE_GLSL
#define OLO_VIRTUAL_SHADOW_LOCAL_RASTER_STAGE_GLSL

// =============================================================================
// VirtualShadowLocalRasterStage.glsl — the local-light fragment stage (#703)
//
// Same idea as VirtualShadowRasterStage.glsl: no depth attachment, visibility
// resolved by imageAtomicMin into the R32UI pool, because the texel a fragment
// belongs to is a page-table lookup the rasterizer cannot know about. Two things
// are different, and both come from a layer being MIPPED where a clip level is
// not.
//
// 1. THE VIEWPORT IS ALWAYS THE MIP-0 RESOLUTION, and the vertex stage scales
//    each instance into a corner sub-rect of it (see VSM_DepthLocal.glsl). So a
//    layer whose pages are being redrawn at mip 5 covers a 64x64 pixel corner
//    rather than the whole 2048x2048 viewport.
//
//    THE HONEST COST, stated because it is the thing to measure first if the
//    local raster is ever the frame's long pole: the sub-rect bounds the USEFUL
//    area, not the RASTERIZED one. A caster that projects outside its face — a
//    ground plane under a lamp is the everyday case, since the plane runs to the
//    face's horizon — still generates fragments across the full viewport, and
//    they are killed by the bounds test below rather than by the clipper. That
//    is a discarded-fragment cost, not a memory-traffic one, and it is paid only
//    on frames where the layer actually has dirty pages (the cull rejects the
//    caster otherwise), so a static scene pays it once. Bounding it properly
//    means one draw per (batch, mip) with a real viewport per mip, which costs
//    six indirect commands per batch — worth doing if a profile ever says so.
//
// 2. A FRAGMENT WRITES ITS OWN MIP AND EVERY COARSER DIRTY ONE. The raster runs
//    at the FINEST mip that has a dirty page in this layer, so coarser mips are
//    reached by shifting the texel right — 4^k fragments land on one mip-k texel
//    and imageAtomicMin keeps the nearest, which is exactly the conservative
//    downsample a shadow depth wants. Rasterizing each mip separately would be
//    the alternative, and it would draw the same geometry six times.
// =============================================================================

#include "VirtualShadowResources.glsl"

layout(r32ui, binding = 0) uniform coherent uimage2D u_VSMPhysicalPages;

layout(location = 0) flat in uint v_VSMLocalLayer;
layout(location = 1) flat in uint v_VSMLocalMip;

void main()
{
    int rasterMip = int(v_VSMLocalMip);
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

    int layer = int(v_VSMLocalLayer);
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

#endif // OLO_VIRTUAL_SHADOW_LOCAL_RASTER_STAGE_GLSL
