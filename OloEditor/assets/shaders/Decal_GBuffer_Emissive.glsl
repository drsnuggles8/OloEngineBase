// Deferred-path emissive decal. Writes into the G-Buffer emissive attachment
// (RT2) so the projected emissive pattern participates in the deferred
// composite as self-lit light — bloom/tone-mapping then process it like any
// other emissive surface. The caller gates draw-buffers to attachment 2 only
// and keeps RT0/RT1/RT3 untouched via glColorMaski.
//
// Blending: DecalRenderPass enables additive blending on RT2 for Emissive
// decals (glBlendFunci(2, GL_ONE, GL_ONE) + glEnablei(GL_BLEND, 2)), so
// stacking multiple emissive decals SUMS their contributions (HDR RT2 is
// RGBA16F so there is ample headroom). Non-emissive decal modes leave blend
// off on RT2, preserving their overwrite behaviour.
//
// Input texture layout: RGB = emissive colour (HDR-capable), A = fade mask.
// The emissive sample is multiplied by u_DecalColor.rgb to tint it; the
// w component of u_DecalColor is applied to the fade mask (like every other
// decal variant). Writing `emissive.a = 0.0` preserves the underlying unlit
// flag so the lighting pass continues to evaluate PBR on the base surface.

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

// Single attachment — caller binds { GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT2, GL_NONE }.
layout(location = 2) out vec4 gEmissive;

#include "include/CameraCommon.glsl"

layout(std140, binding = 21) uniform DecalParams {
    mat4 u_InverseDecalTransform;
    mat4 u_InverseViewProjection;
    vec4 u_DecalColor;
    vec4 u_DecalParams;
};

// Reuses the TEX_USER_0 (=10) slot — the DrawDecalCommand::albedoTextureID
// field carries the emissive texture for Emissive-mode decals, matching the
// existing "primary texture" binding convention.
layout(binding = 10) uniform sampler2D u_DecalEmissive;
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
    vec4 decalSample = texture(u_DecalEmissive, decalUV);

    float fadeDistance = max(u_DecalParams.x, 0.001);
    float fadeX = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.x));
    float fadeY = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.y));
    float fadeZ = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.z));
    float fade  = fadeX * fadeY * fadeZ * decalSample.a * u_DecalColor.a;

    if (fade < 0.001)
        discard;

    // Tint the sampled emissive by the decal colour; fade modulates intensity.
    // RT2.a = 0 so the underlying lit/unlit flag is preserved (caller masks
    // off .a via glColorMaski).
    vec3 emissive = decalSample.rgb * u_DecalColor.rgb * fade;
    gEmissive = vec4(emissive, 0.0);
}
