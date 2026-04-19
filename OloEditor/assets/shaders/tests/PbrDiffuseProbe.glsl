// =============================================================================
// PbrDiffuseProbe.glsl
//
// Test-only probe that isolates the diffuse (Lambertian) contribution of the
// Cook-Torrance BRDF. Uses the exact same formulas as cookTorranceBRDF() in
// PBRCommon.glsl for kS/kD, but skips the specular term.
//
// Parameterization:
//     uv.x -> metallic (0 .. 1)
//     uv.y -> NdotV / NdotL (0 .. 1) — used both as viewing angle cosine for
//             Fresnel and as light cosine. Kept symmetric so the test can use
//             H = N and the Fresnel term matches the one cookTorranceBRDF would
//             compute for the same geometry.
//
// Albedo is fixed at (0.8, 0.8, 0.8) so the diffuse contribution is non-zero at
// metallic = 0 and visibly drops to exactly 0 as metallic → 1.
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
    float metallic = clamp(v_TexCoord.x, 0.0, 1.0);
    float cosTheta = clamp(v_TexCoord.y, 0.0, 1.0);

    vec3 albedo = vec3(0.8);
    vec3 F0 = mix(vec3(DEFAULT_DIELECTRIC_F0), albedo, metallic);
    vec3 F = fresnelSchlick(cosTheta, F0);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 diffuse = kD * albedo * INV_PI;

    o_Color = vec4(diffuse, 1.0);
}
