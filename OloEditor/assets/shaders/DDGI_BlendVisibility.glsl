// =============================================================================
// DDGI_BlendVisibility.glsl — hit distances -> Chebyshev atlas (issue #632)
//
// Runs once per CAPTURED probe (viewport = its 16x16 visibility tile on the
// current write atlas; the pass first copies prev -> current so untouched
// tiles carry forward). Each texel power-cosine-convolves the probe's hit
// distances into a directional (mean, mean^2) estimate and EMA-blends against
// the PREVIOUS atlas texel with the plain volume hysteresis — visibility gets
// NO threshold adjust (distances pop only on recapture/relocation, which the
// EMA smooths; ADR 0006: visibility updates at capture time, not per frame).
// Border texels copy their interior source, same gutter scheme as irradiance.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

#include "include/DDGICommon.glsl"

layout(std140, binding = 7) uniform DDGIPassData
{
    mat4 u_DDGIModel;
    mat4 u_DDGINormalMatrix;
    vec4 u_DDGIBaseColor;
    vec4 u_DDGIProbePosition; // w = probe linear index
};

layout(binding = 0) uniform sampler2D u_HitGeo;         // b = hit distance (<0 sky), a = flag
layout(binding = 1) uniform sampler2D u_PrevVisibility; // previous atlas (EMA history)

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

    int t = u_DDGIHitCacheTexels;
    ivec2 hitOrigin = tile * t;

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
            float dist = sky ? u_DDGIMaxRayDistance : (backface ? geo.b * 0.2 : geo.b);
            dist = min(dist, u_DDGIMaxRayDistance);
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
    // Mirrors DDGI::BlendEMA; no AdjustHysteresis for visibility.
    o_MeanMean2 = vec4(mix(newV, prev, u_DDGIHysteresis), 0.0, 1.0);
}
