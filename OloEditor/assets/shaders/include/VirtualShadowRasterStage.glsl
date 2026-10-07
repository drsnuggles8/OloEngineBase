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

#include "VirtualShadowRasterWrite.glsl"

layout(location = 0) flat in uint v_VSMClipLevel;

void main()
{
    vsmWriteRasterDepth(v_VSMClipLevel);
}

#endif // OLO_VIRTUAL_SHADOW_RASTER_STAGE_GLSL
