// Deferred-path normal-map decal. Writes into G-Buffer RT1 (octahedral
// view-space normal + roughness + AO). The caller gates draw-buffers to
// attachment 1 only and disables writes to zw via glColorMaski so the
// underlying surface's roughness / AO are preserved.
//
// Projection + world-position reconstruction mirrors Decal_GBuffer.glsl;
// the only difference is the output: we sample a tangent-space normal map,
// reconstruct a tangent frame from screen-space depth derivatives
// (no per-vertex TBN is available from a decal proxy cube), transform
// the tangent-space normal into view space, oct-encode, and output.

#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;

#include "include/CameraCommon.glsl"

layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_NormalMatrix;
    int u_EntityID;
    int _modelPad0;
    int _modelPad1;
    int _modelPad2;
};

layout(location = 0) out vec4 v_ClipPos;

void main()
{
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_ClipPos = u_ViewProjection * worldPos;
    gl_Position = v_ClipPos;
}

#type fragment
#version 450 core

layout(location = 0) in vec4 v_ClipPos;

// Writes only RT1 — caller binds only attachment 1 via glNamedFramebufferDrawBuffers.
layout(location = 0) out vec4 gNormalRoughAO;

#include "include/CameraCommon.glsl"

layout(std140, binding = 21) uniform DecalParams {
    mat4 u_InverseDecalTransform;
    mat4 u_InverseViewProjection;
    vec4 u_DecalColor;
    vec4 u_DecalParams; // x = fadeDistance, y = normalAngleThreshold
};

layout(binding = 11) uniform sampler2D u_DecalNormal;
layout(binding = 19) uniform sampler2D u_SceneDepth;

// Oct encode to match PBR_GBuffer.glsl / DeferredLighting.glsl.
vec2 OctWrap(vec2 v)
{
    return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 EncodeOct(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = (n.z >= 0.0) ? n.xy : OctWrap(n.xy);
    return n.xy * 0.5 + 0.5;
}

void main()
{
    vec2 screenUV = (v_ClipPos.xy / v_ClipPos.w) * 0.5 + 0.5;
    float depth = texture(u_SceneDepth, screenUV).r;

    vec4 clipPos = vec4(screenUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos4 = u_InverseViewProjection * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    vec3 localPos = (u_InverseDecalTransform * vec4(worldPos, 1.0)).xyz;

    if (abs(localPos.x) > 0.5 || abs(localPos.y) > 0.5 || abs(localPos.z) > 0.5)
        discard;

    vec2 decalUV = localPos.xz + 0.5;

    // Tangent-space normal (two-channel encoding safe even if blue channel is present).
    vec3 tangentNormal = texture(u_DecalNormal, decalUV).rgb * 2.0 - 1.0;

    // Reconstruct a surface frame from screen-space derivatives of worldPos.
    // This gives us a per-fragment TBN without relying on the decal cube
    // geometry's normals (which are the cube faces, not the receiving surface).
    vec3 dpdx = dFdx(worldPos);
    vec3 dpdy = dFdy(worldPos);
    vec3 surfaceNormalWS = normalize(cross(dpdx, dpdy));

    vec3 tangentWS   = normalize(dpdx - surfaceNormalWS * dot(dpdx, surfaceNormalWS));
    vec3 bitangentWS = cross(surfaceNormalWS, tangentWS);

    vec3 perturbedWS = normalize(
        tangentNormal.x * tangentWS +
        tangentNormal.y * bitangentWS +
        tangentNormal.z * surfaceNormalWS);

    // Project into view space for octahedral encoding (G-Buffer stores VS normals).
    vec3 perturbedVS = normalize((u_View * vec4(perturbedWS, 0.0)).xyz);

    vec2 octEncoded = EncodeOct(perturbedVS);

    // Hard fade threshold: blending oct-encoded normals is mathematically
    // wrong, so discard outside the falloff region instead of blending.
    float fadeDistance = max(u_DecalParams.x, 0.001);
    float fadeX = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.x));
    float fadeY = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.y));
    float fadeZ = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.z));
    float fade = fadeX * fadeY * fadeZ * u_DecalColor.a;

    if (fade < 0.5)
        discard;

    // Write encoded normal into xy; zw (roughness/AO) preserved via colorMask.
    gNormalRoughAO = vec4(octEncoded, 0.0, 1.0);
}
