// =============================================================================
// PbrNormalGuardProbe.glsl
//
// Test-only probe for the DEGENERATE-INPUT guards in PBRCommon's normal-mapping
// path. It renders four independent cases in four screen quadrants, all through
// getNormalFromMapGrad (the explicit-derivative entry point, so the software
// rasterizer's resolve is covered by the same probe):
//
//   quadrant A (left , bottom) — vertex normal is EXACTLY ZERO.
//       normalize(vec3(0)) is NaN. Real data has these: a zero-area triangle has
//       no face normal to accumulate and MeshOptimization deliberately KEEPS such
//       triangles (they carry real 3D area). Contract: fall back to the geometric
//       normal, which on this flat probe surface is +Z — i.e. quadrant A must
//       shade IDENTICALLY to the healthy control in quadrant D.
//
//   quadrant B (right, bottom) — vertex normal is NaN (uploaded as a uniform, so
//       no constant folding can remove it). Same contract as A.
//
//   quadrant C (left , top   ) — UV-COLLINEAR triangle: the texcoord's u is
//       CONSTANT while v varies, so both UV derivatives are non-zero but the UV
//       Jacobian determinant is zero. This is the case a "do the corners share a
//       texcoord?" check misses: 234 of Sponza's 314 UV-degenerate triangles are
//       collinear, not identical. It produces NO NaN — the derivative tangent is
//       finite, unit and in-plane, just ARBITRARY (there is no dP/du when u is
//       constant). Contract asserted here is therefore boundedness, not a value:
//       finite, unit-length, same hemisphere as the geometric normal.
//
//   quadrant D (right, top   ) — healthy control: unit normal, non-degenerate UVs.
//
// Output: encoded = N * 0.5 + 0.5, so a NaN normal shows up as a NaN texel.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_Region;      // [0,1]^2 screen position: picks the quadrant
layout(location = 1) out vec3 v_WorldPos;    // healthy, non-degenerate position gradient
layout(location = 2) out vec2 v_UvHealthy;   // non-degenerate UV gradient
layout(location = 3) out vec2 v_UvCollinear; // u CONSTANT, v varying => zero UV determinant

void main()
{
    v_Region = a_TexCoord;
    v_WorldPos = vec3(a_TexCoord * 4.0, 0.0);
    v_UvHealthy = a_TexCoord;
    // Constant u; v varies along BOTH screen axes, so neither UV derivative is zero
    // while their determinant is — and the resulting tangent is not accidentally the
    // same one the healthy case produces.
    v_UvCollinear = vec2(0.25, 0.35 * (a_TexCoord.x + a_TexCoord.y));
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_Region;
layout(location = 1) in vec3 v_WorldPos;
layout(location = 2) in vec2 v_UvHealthy;
layout(location = 3) in vec2 v_UvCollinear;

#include "include/PBRCommon.glsl"

layout(binding = 0) uniform sampler2D u_NormalMap;

// The four cases' INPUT vertex normals, one texel each, supplied by the host as
// RGBA32F: (0,0,0) / (NaN,NaN,NaN) / (0,0,1) / (0,0,1).
//
// Why a texture and not a uniform: this engine compiles graphics shaders through
// SPIR-V, where a non-opaque uniform outside a block is illegal. A texel also cannot
// be constant-folded, and it means the shader never has to CONSTRUCT a NaN itself —
// 0.0/0.0 is formally undefined in GLSL, so a shader-built NaN would be a test whose
// input the driver is allowed to change.
layout(binding = 1) uniform sampler2D u_CaseNormals;

void main()
{
    // Every derivative is taken in UNIFORM control flow, before the quadrant branch.
    vec3 dpdx = dFdx(v_WorldPos);
    vec3 dpdy = dFdy(v_WorldPos);
    vec2 uvHealthyDx = dFdx(v_UvHealthy);
    vec2 uvHealthyDy = dFdy(v_UvHealthy);
    vec2 uvCollinearDx = dFdx(v_UvCollinear);
    vec2 uvCollinearDy = dFdy(v_UvCollinear);

    bool right = v_Region.x >= 0.5;
    bool top = v_Region.y >= 0.5;

    // 0 = zero normal, 1 = NaN normal, 2 = UV-collinear, 3 = healthy control.
    int caseIndex = (top ? 2 : 0) + (right ? 1 : 0);

    vec3 normal = texelFetch(u_CaseNormals, ivec2(caseIndex, 0), 0).xyz;
    vec2 uv = v_UvHealthy;
    vec2 duvdx = uvHealthyDx;
    vec2 duvdy = uvHealthyDy;

    if (caseIndex == 2)
    {
        // C: UV-collinear triangle (zero UV determinant, non-zero derivatives)
        uv = v_UvCollinear;
        duvdx = uvCollinearDx;
        duvdy = uvCollinearDy;
    }

    vec3 n = getNormalFromMapGrad(u_NormalMap, uv, dpdx, dpdy, duvdx, duvdy, normal, 1.0);
    o_Color = vec4(n * 0.5 + 0.5, 1.0);
}
