// =============================================================================
// PbrDegenerateUvProbe.glsl
//
// Test-only probe for getNormalFromMap() on a UV-DEGENERATE triangle — one
// whose three corners carry the SAME texture coordinate, so the interpolated
// UV is constant across the primitive and dFdx(texCoords) == dFdy(texCoords)
// == 0. Real assets contain these: Sponza's source mesh has 314 of them
// (zero UV area, non-zero 3D area), and mesh simplification (the Nanite-style
// cluster-LOD DAG, issue #629) creates more.
//
// With a zero UV gradient the derivative-based tangent
//     T = normalize(Q1 * st2.t - Q2 * st1.t)
// is normalize(vec3(0)) => NaN, which poisons the whole TBN and writes a NaN
// world normal into the G-Buffer. Deferred lighting then produces a blown-out
// white pixel (issue #629: Sponza's potted vines rendered with a white
// lacework along every leaf silhouette).
//
// Contract: with no usable tangent frame, getNormalFromMap must FALL BACK to
// the geometric normal — never return NaN/Inf.
//
// Setup mirrors PbrNormalMapProbe.glsl except v_TexCoord is a compile-time
// constant (so its screen-space derivatives are exactly zero) while
// v_WorldPos keeps clean nonzero derivatives.
//
// Expected output (encoded back to [0,1] for readback):
//     encoded = N * 0.5 + 0.5 = (0.5, 0.5, 1.0)   with N = (0, 0, 1)
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec3 v_WorldPos;

void main()
{
    // UV-degenerate: every corner of the primitive gets the SAME texcoord, so
    // the interpolated value is constant and both screen-space derivatives are 0.
    v_TexCoord = vec2(0.5, 0.5);
    // World position still varies normally — only the UV gradient is degenerate.
    v_WorldPos = vec3(a_TexCoord, 0.0);
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec3 v_WorldPos;

#include "include/PBRCommon.glsl"

layout(binding = 0) uniform sampler2D u_NormalMap;

void main()
{
    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 n = getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, N, 1.0);
    o_Color = vec4(n * 0.5 + 0.5, 1.0);
}
