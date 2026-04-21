// =============================================================================
// PbrNdfProbe.glsl
//
// Test-only probe for GGX distribution function distributionGGX().
// Parameterization:
//     uv.x -> NdotH, realized as H = (sin(theta), 0, cos(theta))
//             with theta = (1 - uv.x) * (PI / 2). uv.x = 1 → H = N.
//     uv.y -> roughness (0.04 .. 1 — clamped to MIN_ROUGHNESS lower bound)
//
// Output channel usage:
//   .r = distributionGGX(N, H, roughness)
//   .g = NdotH (for caller sanity check)
//   .b = roughness
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
    float theta = (1.0 - clamp(v_TexCoord.x, 0.0, 1.0)) * HALF_PI;
    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 H = normalize(vec3(sin(theta), 0.0, cos(theta)));
    float roughness = max(clamp(v_TexCoord.y, 0.0, 1.0), MIN_ROUGHNESS);
    float D = distributionGGX(N, H, roughness);

    o_Color = vec4(D, dot(N, H), roughness, 1.0);
}
