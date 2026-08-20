// =============================================================================
// DDGI_BlendVisibility.glsl — hit distances -> Chebyshev atlas (issue #632)
//
// Runs once per CAPTURED probe (viewport = its 16x16 visibility tile on the
// current write atlas; the pass first copies prev -> current so untouched
// tiles carry forward). Each texel power-cosine-convolves the probe's hit
// distances into a directional (mean, mean^2) estimate and EMA-blends against
// the PREVIOUS atlas texel with the plain volume hysteresis — visibility gets
// NO threshold adjust (distances pop only on recapture/relocation, which the
// EMA smooths; ADR 0007: visibility updates at capture time, not per frame).
// Border texels copy their interior source, same gutter scheme as irradiance.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 Phase 8 (ADR 0011 §5): V1 vertex pull. On the Vulkan route the
// pipeline has no vertex-input state, so attributes are READ from binding 57
// (the engine-wide vertex-pull binding; the root struct carries this buffer's
// device address). This pass draws MeshPrimitives::GetFullscreenTriangle(),
// a 20-byte {vec3 position @0, vec2 uv @12} interleave, so the stride is
// 5 floats. The GL attribute branch below is untouched.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
#endif

layout(location = 0) out vec2 v_TexCoord;

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 5;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
    vec2 a_TexCoord = vec2(b_Vertices.v[vertBase + 3], b_Vertices.v[vertBase + 4]);
#endif
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

#include "include/DDGICommon.glsl"

#include "include/DDGIPassData.glsl"

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691 Phase 3, bucket 1). Each name maps to
// the SAME binding number the pass binds with, so the two variants cannot
// disagree; the shader BODY is byte-identical between them.
#ifdef OLO_BINDLESS
#define u_HitGeo OLO_HEAP_TEX_2D(0)   // b = hit distance (<0 sky), a = flag
#define u_PrevVisibility OLO_HEAP_TEX_2D(1)   // previous atlas (EMA history)
#else
layout(binding = 0) uniform sampler2D u_HitGeo;         // b = hit distance (<0 sky), a = flag
layout(binding = 1) uniform sampler2D u_PrevVisibility; // previous atlas (EMA history)
#endif

layout(location = 0) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_MeanMean2;

void main()
{
    int probeIdx = int(u_DDGIProbePosition.w + 0.5);
    ivec2 tile = ddgiProbeTileCoord(probeIdx);
    ivec2 atlasTexel = ivec2(gl_FragCoord.xy);
    ivec2 local = atlasTexel - tile * DDGI_VISIBILITY_TILE;

    ivec2 src = ddgiBorderSourceTexel(local, DDGI_VISIBILITY_TILE);
    vec3 texelDir = ddgiTexelDirection(src - ivec2(1), DDGI_VISIBILITY_INTERIOR);

    // The hit cache and the visibility atlas share the same TILE grid (issue
    // #707: column = level * DimX + x, row = z * DimY + y), so one tile
    // coordinate addresses both — cascades change the mapping in exactly one
    // place, ddgiCascadedProbeTileCoord.
    int t = u_DDGIHitCacheTexels;
    ivec2 hitOrigin = tile * t;

    // Per-CASCADE distance clamp (issue #707). u_DDGIMaxRayDistance describes
    // cascade 0 only; a coarse cascade's probes are further apart and their
    // hit distances are correspondingly longer, so clamping every cascade to
    // cascade 0's reach would report every coarse probe as "occluder right
    // here" and Chebyshev would de-weight the entire outer field.
    float maxRayDistance = u_DDGICascadeOrigin[ddgiCascadeOfProbeIndex(probeIdx)].w;

    float sumW = 0.0;
    float mean = 0.0;
    float mean2 = 0.0;
    for (int y = 0; y < t; ++y)
    {
        for (int x = 0; x < t; ++x)
        {
            vec4 geo = texelFetch(u_HitGeo, hitOrigin + ivec2(x, y), 0);
            vec3 hitDir = ddgiTexelDirection(ivec2(x, y), t);
            // Mirrors DDGI::DistanceBlendWeight (power-cosine lobe, RTXGI
            // probeDistanceExponent default 50 — sharp so the distance
            // estimate stays directional).
            float w = pow(max(0.0, dot(texelDir, hitDir)), 50.0);
            if (w < 1e-8)
            {
                continue;
            }
            bool sky = geo.b < 0.0;
            bool backface = (geo.a > 0.25 && geo.a < 0.75);
            // Sky = unoccluded out to the clamp; backface distances are
            // crushed (x0.2, RTXGI convention) so an in-wall direction reads
            // as "occluder right here" instead of averaging away.
            float dist = sky ? maxRayDistance : (backface ? geo.b * 0.2 : geo.b);
            dist = min(dist, maxRayDistance);
            sumW += w;
            mean += w * dist;
            mean2 += w * dist * dist;
        }
    }

    vec2 prev = texelFetch(u_PrevVisibility, atlasTexel, 0).rg;
    if (sumW < 1e-6)
    {
        o_MeanMean2 = vec4(prev, 0.0, 1.0);
        return;
    }

    vec2 newV = vec2(mean / sumW, mean2 / sumW);

    // SEED THE FIRST SAMPLE INSTEAD OF EMA-ING UP FROM THE CLEARED ATLAS.
    //
    // Visibility, unlike irradiance, is blended ONLY when a probe is captured
    // (ADR 0007: hit distances change at capture time, not per frame). At
    // hysteresis 0.9 an EMA needs ~40 samples to converge — and in a cascaded
    // field a probe is recaptured only when the refresh tier reaches it, which
    // at budget/8 over hundreds of live probes is once every ~130 frames. That
    // is thousands of frames to a correct Chebyshev term, during which the
    // atlas is permanently mid-transient: measured as a converged field that
    // would not stop churning at 3-7 RMSE under a still camera.
    //
    // An EMA whose history is the CLEAR VALUE is not carrying information, it
    // is carrying a placeholder — so the first sample replaces it outright and
    // the EMA starts doing its job from a real value. (0,0) is unambiguous as
    // "never blended": a probe that sees nothing at all still stores
    // mean = MaxRayDistance, not zero.
    //
    // This is a #632 characteristic that cascades EXPOSE rather than introduce
    // — the authored path has the same slow start, just with few enough probes
    // per unit of capture budget that it converges before anyone looks.
    const bool neverBlended = (prev.r <= 0.0 && prev.g <= 0.0);
    // Mirrors DDGI::BlendEMA; no AdjustHysteresis for visibility.
    o_MeanMean2 = vec4(neverBlended ? newV : mix(newV, prev, u_DDGIHysteresis), 0.0, 1.0);
}
