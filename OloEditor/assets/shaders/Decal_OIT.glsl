//--------------------------
// Decal_OIT.glsl
//
// Weighted-blended OIT variant of Decal.glsl. Shares the vertex stage
// verbatim and the scene-depth projection math; the fragment stage
// emits accum + revealage into the OITBuffer attachments so overlapping
// forward-path decals composite without back-to-front sorting.
//
// Selected by DecalRenderPass when `RendererSettings::OITEnabled` is true.
// In the Deferred path, opaque decals are routed through the G-Buffer decal
// variants instead (re-lit through DeferredLightingPass); only translucent
// decals reach this shader. In Forward / Forward+ all decals participate
// in OIT when enabled.
//--------------------------
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
layout(location = 1) out flat int v_EntityID;

void main()
{
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_ClipPos = u_ViewProjection * worldPos;
    v_EntityID = u_EntityID;
    gl_Position = v_ClipPos;
}

#type fragment
#version 450 core

layout(location = 0) in vec4 v_ClipPos;
layout(location = 1) in flat int v_EntityID;

// OIT attachments — must match OITBuffer layout + per-attachment blend
// funcs configured by DecalRenderPass in OIT mode.
layout(location = 0) out vec4 o_Accum;     // RGBA16F: sum(C*a*w, a*w)
layout(location = 1) out vec4 o_Revealage; // RG16F:   alpha into .r

#include "include/CameraCommon.glsl"
#include "include/OITCommon.glsl"

layout(std140, binding = 21) uniform DecalParams {
    mat4 u_InverseDecalTransform;
    mat4 u_InverseViewProjection;
    vec4 u_DecalColor;
    vec4 u_DecalParams; // x = fadeDistance, y = normalAngleThreshold, z/w = unused
};

layout(binding = 10) uniform sampler2D u_DecalAlbedo;
layout(binding = 19) uniform sampler2D u_SceneDepth;

void main()
{
    // Reconstruct screen UV from clip-space position.
    vec2 screenUV = (v_ClipPos.xy / v_ClipPos.w) * 0.5 + 0.5;

    // Sample scene depth and reconstruct world position.
    float depth = texture(u_SceneDepth, screenUV).r;
    vec4 clipPos = vec4(screenUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos4 = u_InverseViewProjection * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    // Transform into decal local space and cull outside the unit cube.
    vec3 localPos = (u_InverseDecalTransform * vec4(worldPos, 1.0)).xyz;
    if (abs(localPos.x) > 0.5 || abs(localPos.y) > 0.5 || abs(localPos.z) > 0.5)
        discard;

    vec2 decalUV = localPos.xz + 0.5;
    vec4 decalColor = texture(u_DecalAlbedo, decalUV) * u_DecalColor;

    // Edge fade (matches forward Decal.glsl).
    float fadeDistance = max(u_DecalParams.x, 0.001);
    float fadeX = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.x));
    float fadeY = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.y));
    float fadeZ = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.z));
    decalColor.a *= fadeX * fadeY * fadeZ;

    if (decalColor.a < 0.001)
        discard;

    // WB-OIT pack. v_ClipPos.w is the perspective-projected view-space
    // depth (approximately linear z in front of the camera) — exactly
    // what ComputeOITWeight expects.
    float weight = ComputeOITWeight(decalColor.a, max(v_ClipPos.w, 1e-3));
    OITPack(decalColor.rgb, decalColor.a, weight, o_Accum, o_Revealage);
}
