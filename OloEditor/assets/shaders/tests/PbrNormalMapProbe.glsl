// =============================================================================
// PbrNormalMapProbe.glsl
//
// Test-only probe for getNormalFromMap(). Verifies the "flat normal map"
// identity: when the normal map samples to (0.5, 0.5, 1.0) — i.e. a tangent-
// space normal of (0, 0, 1) after *2-1 decoding — the returned world-space
// normal must equal the input geometric normal N exactly.
//
// This catches sign flips in the TBN construction, swapped T/B, missing
// normalisation, and wrong-handedness bugs.
//
// Setup:
//   * worldPos = (uv.x, uv.y, 0) so dFdx(worldPos) = (1/width, 0, 0) and
//                                   dFdy(worldPos) = (0, 1/height, 0)
//   * texCoords = uv so derivatives are the canonical (1/w, 0) / (0, 1/h)
//   * N = (0, 0, 1) (surface facing camera)
//   * Normal map is bound at unit 0 and must sample to (0.5, 0.5, 1.0)
//
// Expected output (pre-encoded back to [0,1] for RGBA8 readback):
//     encoded = N * 0.5 + 0.5 = (0.5, 0.5, 1.0)
// Any TBN bug (sign flip, swap) shifts the output away from (0.5, 0.5, 1.0).
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec3 v_WorldPos;

void main()
{
    v_TexCoord = a_TexCoord;
    // World position matches UV so dFdx/dFdy yield clean, nonzero derivatives.
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
    // Encode world-space normal back into [0,1] for RGBA8 readback.
    o_Color = vec4(n * 0.5 + 0.5, 1.0);
}
