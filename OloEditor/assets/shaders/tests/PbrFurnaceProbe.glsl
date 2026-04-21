// =============================================================================
// PbrFurnaceProbe.glsl
//
// Test probe for the full Cook-Torrance BRDF in a white-furnace setup.
//
// Each texel performs ONE Monte Carlo sample of the hemisphere integral
//
//     I(roughness) = ∫ f(l, v) * (n · l) dω_i
//
// assuming incoming white radiance L_i = 1. Using uniform hemisphere
// sampling with pdf = 1/(2π), the per-sample estimate is
//
//     2π * f(l, v) * (n · l)
//
// which is written to .r. The caller averages over a column (many samples,
// one roughness value) to obtain I(roughness), which for an energy-conserving
// BRDF should stay within [~0.6, 1.05]. Any value > 1 means the BRDF is
// *creating* energy — a hard bug.
//
// Parameterisation:
//   uv.x -> roughness ∈ [1/(2N), 1 - 1/(2N)]  (pixel-center sampling)
//   uv.y -> sample index — used to derive a pseudo-random (ξ1, ξ2) pair
// Fixed:  N = (0, 0, 1), V = (0, 0, 1), albedo = (1,1,1), metallic = 0
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

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/PBRCommon.glsl"

// Cheap deterministic hash → [0, 1). Good enough for Monte Carlo estimates
// averaged over thousands of samples.
float hash11(float x)
{
    x = fract(x * 0.1031);
    x *= x + 33.33;
    x *= x + x;
    return fract(x);
}

vec2 hash22(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

void main()
{
    // uv.x in [0.5/N, 1 - 0.5/N] due to pixel-center sampling — perfect for
    // driving roughness directly.
    float roughness = max(clamp(v_TexCoord.x, 0.0, 1.0), MIN_ROUGHNESS);

    // Decorrelate per-texel by combining x,y into the hash seed.
    vec2 xi = hash22(v_TexCoord * vec2(4096.0, 8192.0));

    // Uniform hemisphere sampling (NOT cosine-weighted):
    //   z    = ξ1,       (n · l)
    //   r    = sqrt(1 - ξ1^2)
    //   phi  = 2π ξ2
    float cosTheta = xi.x;
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = TWO_PI * xi.y;
    vec3 L = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 V = vec3(0.0, 0.0, 1.0);
    vec3 albedo = vec3(1.0);
    float metallic = 0.0;

    vec3 fr = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);

    // pdf = 1/(2π), integrand = f * (n·l). Estimator = integrand / pdf.
    float estimate = (fr.x + fr.y + fr.z) * (1.0 / 3.0) * cosTheta * TWO_PI;

    o_Color = vec4(estimate, cosTheta, roughness, 1.0);
}
