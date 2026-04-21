// =============================================================================
// PbrReciprocityProbe.glsl
//
// Test-only probe verifying Helmholtz reciprocity of cookTorranceBRDF:
//     f(N, V, L, ...) == f(N, L, V, ...)
//
// This is a fundamental physical correctness property of any BRDF: swapping
// view and light directions must not change the reflected radiance. Violations
// indicate asymmetric math (e.g. using NdotL where NdotV was intended, or a
// non-symmetric G term).
//
// Parameterisation:
//   uv.x -> NdotL at V held along Z
//   uv.y -> NdotV (when we later swap V/L, this becomes the new NdotL input)
//
// Output channels:
//   .r = |f(N,V,L) - f(N,L,V)|  (max across RGB)  — should be ~0
//   .g = f(N,V,L).x
//   .b = f(N,L,V).x
//   .a = 1
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
    float ndl = clamp(v_TexCoord.x, 0.05, 1.0);
    float ndv = clamp(v_TexCoord.y, 0.05, 1.0);

    vec3 N = vec3(0.0, 0.0, 1.0);
    // L sits in the x-z plane, V in the y-z plane so they're linearly
    // independent and neither degenerates as we sweep.
    vec3 L = normalize(vec3(sqrt(max(1.0 - ndl * ndl, 0.0)), 0.0, ndl));
    vec3 V = normalize(vec3(0.0, sqrt(max(1.0 - ndv * ndv, 0.0)), ndv));

    float roughness = 0.5;
    vec3 albedo = vec3(0.8, 0.7, 0.6);
    float metallic = 0.3;

    vec3 fwd = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
    vec3 rev = cookTorranceBRDF(N, L, V, albedo, metallic, roughness);
    vec3 diff = abs(fwd - rev);
    float maxDiff = max(max(diff.r, diff.g), diff.b);

    o_Color = vec4(maxDiff, fwd.x, rev.x, 1.0);
}
