// =============================================================================
// PbrFresnelProbe.glsl
//
// Test-only shader that samples the production fresnelSchlick() function over
// a 2D parameter grid:
//     uv.x -> cosTheta   (0 = grazing, 1 = normal incidence)
//     uv.y -> F0 scalar  (0 .. 1, used for all 3 channels)
//
// Each output pixel is fresnelSchlick(cosTheta, vec3(F0)). Property-based
// tests read back the resulting framebuffer and assert:
//   - At cosTheta = 1.0 (normal incidence), output == F0
//   - At cosTheta = 0.0 (grazing),          output == 1.0
//   - Monotonic non-increasing as cosTheta increases (for fixed F0)
//
// This is NOT used by the renderer at runtime.
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
    float cosTheta = clamp(v_TexCoord.x, 0.0, 1.0);
    float F0Scalar = clamp(v_TexCoord.y, 0.0, 1.0);
    vec3 F0 = vec3(F0Scalar);
    vec3 F = fresnelSchlick(cosTheta, F0);
    o_Color = vec4(F, 1.0);
}
