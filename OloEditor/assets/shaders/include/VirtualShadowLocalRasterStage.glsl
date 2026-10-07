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

#include "VirtualShadowLocalRasterWrite.glsl"

layout(location = 0) flat in uint v_VSMLocalLayer;
layout(location = 1) flat in uint v_VSMLocalMip;

void main()
{
    vsmWriteLocalRasterDepth(v_VSMLocalLayer, v_VSMLocalMip);
}

#endif // OLO_VIRTUAL_SHADOW_LOCAL_RASTER_STAGE_GLSL
