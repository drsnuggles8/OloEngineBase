// =============================================================================
// DDGI_BlendIrradiance.glsl — radiance cache -> irradiance atlas (issue #632)
//
// Fullscreen over the CURRENT (write) irradiance atlas every frame. Each 8x8
// probe tile texel cosine-convolves the probe's relit radiance cache and
// EMA-blends against the PREVIOUS atlas under the adjusted hysteresis.
// Border texels copy their interior source (ddgiBorderSourceTexel) so the
// whole tile — gutter included — is produced in this one draw.
//
// STORAGE CONVENTION (pinned here): the atlas stores full irradiance E.
// The convolution is the cosine-weighted ratio estimator
//   E = PI * sum(w * L) / sum(w),  w = max(0, dot(texelDir, hitDir))
// so a constant-radiance field L over the sphere produces E = PI * L exactly
// — no 2*pi appears anywhere in this pipeline, and the sampler
// (ddgiSampleIrradiance) returns E directly (consumers apply albedo/PI).
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

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691 Phase 3, bucket 1). Each name maps to
// the SAME binding number the pass binds with, so the two variants cannot
// disagree; the shader BODY is byte-identical between them.
#ifdef OLO_BINDLESS
#define u_Radiance OLO_HEAP_TEX_2D(0)   // relit hit radiance cache
#define u_HitGeo OLO_HEAP_TEX_2D(1)   // a = DDGI_HIT_* flag
#define u_PrevIrradiance OLO_HEAP_TEX_2D(2)   // previous frame's atlas (EMA history)
#define u_ProbeData OLO_HEAP_TEX_2D(3)   // w = probe state
#else
layout(binding = 0) uniform sampler2D u_Radiance;       // relit hit radiance cache
layout(binding = 1) uniform sampler2D u_HitGeo;         // a = DDGI_HIT_* flag
layout(binding = 2) uniform sampler2D u_PrevIrradiance; // previous frame's atlas (EMA history)
layout(binding = 3) uniform sampler2D u_ProbeData;      // w = probe state
#endif

layout(location = 0) in vec2 v_TexCoord;

layout(location = 0) out vec4 o_Irradiance;

const float DDGI_PI = 3.14159265359;

void main()
{
    ivec2 atlasTexel = ivec2(gl_FragCoord.xy);
    ivec2 tile = atlasTexel / DDGI_IRRADIANCE_TILE;
    ivec2 local = atlasTexel % DDGI_IRRADIANCE_TILE;

    vec4 prev = texelFetch(u_PrevIrradiance, atlasTexel, 0);
    vec4 pdata = texelFetch(u_ProbeData, tile, 0);

    // Uncaptured: no atlas content yet — hold zero so the sampler's
    // state gate and a cleared atlas agree.
    if (pdata.w < 0.5)
    {
        o_Irradiance = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    // Inactive (in-wall): frozen — copy the previous texel through unchanged.
    if (pdata.w > 1.5)
    {
        o_Irradiance = prev;
        return;
    }

    // Border texels replicate their interior source so cross-tile bilinear
    // taps stay inside the probe (RTXGI gutter convention).
    ivec2 src = ddgiBorderSourceTexel(local, DDGI_IRRADIANCE_TILE);
    vec3 dir = ddgiTexelDirection(src - ivec2(1), DDGI_IRRADIANCE_INTERIOR);

    int t = u_DDGIHitCacheTexels;
    ivec2 hitOrigin = tile * t;

    vec3 sum = vec3(0.0);
    float sumW = 0.0;
    for (int y = 0; y < t; ++y)
    {
        for (int x = 0; x < t; ++x)
        {
            ivec2 hitTexel = hitOrigin + ivec2(x, y);
            float flag = texelFetch(u_HitGeo, hitTexel, 0).a;
            // Backface hits contribute NOTHING (weight 0, RTXGI convention) —
            // their zero radiance must not drag the estimator down.
            if (flag > 0.25 && flag < 0.75)
            {
                continue;
            }
            vec3 hitDir = ddgiTexelDirection(ivec2(x, y), t);
            // Mirrors DDGI::IrradianceBlendWeight (Lambertian cosine lobe).
            float w = max(0.0, dot(dir, hitDir));
            if (w <= 0.0)
            {
                continue;
            }
            sum += w * texelFetch(u_Radiance, hitTexel, 0).rgb;
            sumW += w;
        }
    }

    // Degenerate lobe (shouldn't happen with a full sphere of directions,
    // but a backface-heavy hemisphere can starve it): keep history.
    vec3 eNew = (sumW < 1e-6) ? prev.rgb : DDGI_PI * (sum / sumW);

    // Mirrors DDGI::AdjustHysteresis (DDGICommon.h) — the C++ header is the
    // pinned home of this formula; it is not in DDGICommon.glsl because only
    // this blend stage uses it. Thresholds: JCGT 2021 (>0.25 -> cut 0.15,
    // >0.8 -> drop history) so lights snapping on/off do not smear.
    vec3 delta = abs(eNew - prev.rgb);
    float maxComponentDelta = max(delta.x, max(delta.y, delta.z));
    float h = u_DDGIHysteresis;
    if (maxComponentDelta > 0.8)
    {
        h = 0.0;
    }
    else if (maxComponentDelta > 0.25)
    {
        h = max(h - 0.15, 0.0);
    }

    // Mirrors DDGI::BlendEMA — hysteresis is the fraction of HISTORY kept.
    o_Irradiance = vec4(mix(eNew, prev.rgb, h), 1.0);
}
