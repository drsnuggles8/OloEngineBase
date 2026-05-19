// Deferred-path roughness / metallic / AO decal. Writes into two
// G-Buffer attachments: RT0.a (metallic) and RT1.zw (roughness, AO).
// The caller gates draw-buffers to attachments 0 and 1 and uses
// glColorMaski to keep the albedo (RT0.rgb) and the view-space normal
// (RT1.xy) of the underlying surface untouched.
//
// Input texture layout: R = Roughness, G = Metallic, B = AO, A = fade mask.

#type vertex
#version 450 core

layout(location = 0) in vec3 a_Position;

#include "include/CameraCommon.glsl"

#include "include/InstanceBlock_Vertex.glsl"

layout(location = 0) out vec4 v_ClipPos;

void main()
{
    OLO_INSTANCE_FORWARD();
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_ClipPos = u_ViewProjection * worldPos;
    gl_Position = v_ClipPos;
}

#type fragment
#version 450 core

layout(location = 0) in vec4 v_ClipPos;

// Two attachments — caller binds {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1}.
layout(location = 0) out vec4 gAlbedo;        // Only .a is written (metallic).
layout(location = 1) out vec4 gNormalRoughAO; // Only .zw written (roughness, AO).

#include "include/CameraCommon.glsl"

layout(std140, binding = 21) uniform DecalParams {
    mat4 u_InverseDecalTransform;
    mat4 u_InverseViewProjection;
    vec4 u_DecalColor;
    vec4 u_DecalParams;
};

layout(binding = 12) uniform sampler2D u_DecalRMA;
layout(binding = 19) uniform sampler2D u_SceneDepth;

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

    vec4 rma = texture(u_DecalRMA, decalUV);
    float roughness = rma.r * u_DecalColor.r;
    float metallic  = rma.g * u_DecalColor.g;
    float ao        = rma.b * u_DecalColor.b;

    float fadeDistance = max(u_DecalParams.x, 0.001);
    float fadeX = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.x));
    float fadeY = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.y));
    float fadeZ = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.z));
    float fade  = fadeX * fadeY * fadeZ * u_DecalColor.a;

    // Hard threshold: alpha-blending the scalar channels toward arbitrary
    // new values is not expressible with standard SRC_ALPHA blending when
    // the shader alpha encodes the fade factor (that math requires dual-
    // source blending). Discard outside the falloff so writes are always
    // at full intensity; caller's colorMask protects unrelated channels.
    if (fade < 0.5)
        discard;

    // RT0: only .a (metallic) is written — caller sets glColorMaski(0,F,F,F,T).
    gAlbedo        = vec4(0.0, 0.0, 0.0, metallic);
    // RT1: only .zw (roughness, AO) written — caller sets glColorMaski(1,F,F,T,T).
    gNormalRoughAO = vec4(0.0, 0.0, roughness, ao);
}
