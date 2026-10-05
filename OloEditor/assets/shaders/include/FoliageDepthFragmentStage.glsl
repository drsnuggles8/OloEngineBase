#ifdef OLO_VSM_FAMILY
#include "VirtualShadowFamilyFragment.glsl"
#endif
layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in float v_AlphaCutoff;
layout(location = 2) in float v_MeshCoverage;
layout(location = 3) in vec2 v_LodSeedFade;

#include "BindlessHeap.glsl"
#ifdef OLO_BINDLESS
#define u_DiffuseTexture OLO_HEAP_TEX_2D(0)  // TEX_DIFFUSE
#else
layout(binding = 0) uniform sampler2D u_DiffuseTexture;
#endif

// Foliage UBO (binding 12) — shared with vertex stage
#include "FoliageParams.glsl"

#include "FoliageInstanceGeometry.glsl"

void main()
{
    // The same partition rule, over the same per-instance coverage, so the
    // caster set tracks the drawn set as the hand-over crosses (issue #1233).
    // The dither is keyed on gl_FragCoord, which is the SHADOW MAP's here — so
    // inside the band an individual shadow-map texel may come from the other
    // side than the lit pixel it shadows. That is a sub-pixel difference at
    // matching coverage, which is what a dithered LOD is; what it cannot do is
    // shadow a quad where the lit frame drew a pine, because the coverage both
    // passes partition is computed per instance from the same pivot.
    if (!foliageLodKeep(u_MeshParams.x > 0.5, v_MeshCoverage, gl_FragCoord.xy, v_LodSeedFade.x,
                        foliageStochasticCoverage(u_LodTransition0)))
        discard;

    // The thinning fade, resolved the same way (issue #1237): a depth pass has
    // no alpha either, so a plant mid-thinning casts a dithered shadow rather
    // than a full one that vanishes in a frame.
    if (foliageStochasticCoverage(u_LodTransition0) &&
        !foliageDensityKeep(v_LodSeedFade.y, gl_FragCoord.xy, v_LodSeedFade.x))
        discard;

    float alpha = texture(u_DiffuseTexture, v_TexCoord).a;
    if (alpha < v_AlphaCutoff)
        discard;

#ifdef OLO_VSM_FAMILY
    vsmFamilyWriteDepth();
#endif
}
