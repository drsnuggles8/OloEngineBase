// =============================================================================
// PbrBrdfProbe.glsl
//
// Test-only probe for the full Cook-Torrance BRDF. Exercises the production
// implementation in include/PBRCommon.glsl via cookTorranceBRDF().
//
// Parameterisation:
//   uv.x -> NdotL (via L tilted in x-z plane; NdotV held at 1)
//   uv.y -> roughness (clamped >= MIN_ROUGHNESS)
// Fixed:   N = (0,0,1), V = (0,0,1), albedo = (1,1,1), metallic = 0
//
// Output:
//   .rgb = cookTorranceBRDF(...)
//   .a   = 1
//
// Invariants the caller asserts:
//   * BRDF is non-negative and finite (positivity / no NaNs / no infs)
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

void main()
{
    float ndl = clamp(v_TexCoord.x, 0.02, 1.0);
    float theta = acos(ndl);
    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 V = vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(vec3(sin(theta), 0.0, cos(theta)));

    float roughness = max(clamp(v_TexCoord.y, 0.0, 1.0), MIN_ROUGHNESS);
    vec3 albedo = vec3(1.0);
    float metallic = 0.0;

    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
    o_Color = vec4(brdf, 1.0);
}
