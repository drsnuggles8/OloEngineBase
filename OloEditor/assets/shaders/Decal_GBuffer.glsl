// Deferred-path decal. Writes into the G-Buffer albedo attachment
// (RT0, RGBA8) **before** the lighting pass runs, so the subsequent
// DeferredLightingPass will re-light the decal texels the same way
// it lights any other surface. Only RT0 is written; the caller must
// gate draw-buffers to attachment 0 to leave RT1/RT2/RT3 untouched.

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

// Only RT0 (albedo + metallic). Other G-Buffer attachments are
// disabled by the caller via glNamedFramebufferDrawBuffers so the
// shader does not need to redundantly write them.
layout(location = 0) out vec4 gAlbedo;

#include "include/CameraCommon.glsl"

layout(std140, binding = 21) uniform DecalParams {
    mat4 u_InverseDecalTransform;
    mat4 u_InverseViewProjection; // Precomputed on CPU to avoid per-fragment inverse()
    vec4 u_DecalColor;
    vec4 u_DecalParams; // x = fadeDistance, y = normalAngleThreshold, z = unused, w = unused
};

layout(binding = 10) uniform sampler2D u_DecalAlbedo;
layout(binding = 19) uniform sampler2D u_SceneDepth;

void main()
{
    // Reconstruct screen UV from clip-space position of the decal cube fragment.
    vec2 screenUV = (v_ClipPos.xy / v_ClipPos.w) * 0.5 + 0.5;

    // Sample underlying scene depth (G-Buffer depth attachment).
    float depth = texture(u_SceneDepth, screenUV).r;

    // Reconstruct world position from depth.
    vec4 clipPos = vec4(screenUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos4 = u_InverseViewProjection * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    // Transform world position into decal local space.
    vec3 localPos = (u_InverseDecalTransform * vec4(worldPos, 1.0)).xyz;

    // Discard fragments outside the unit cube [-0.5, 0.5].
    if (abs(localPos.x) > 0.5 || abs(localPos.y) > 0.5 || abs(localPos.z) > 0.5)
        discard;

    // Use XZ as UV (remap [-0.5, 0.5] -> [0, 1]).
    vec2 decalUV = localPos.xz + 0.5;

    vec4 decalColor = texture(u_DecalAlbedo, decalUV) * u_DecalColor;

    // Edge fade based on distance from box boundaries.
    float fadeDistance = max(u_DecalParams.x, 0.001);
    float fadeX = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.x));
    float fadeY = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.y));
    float fadeZ = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.z));
    float fade = fadeX * fadeY * fadeZ;

    decalColor.a *= fade;

    if (decalColor.a < 0.001)
        discard;

    // RT0 is RGBA8 — .rgb is albedo, .a doubles as metallic in the G-Buffer
    // layout (see PBR_GBuffer.glsl). A decal adjusts the albedo but must not
    // clobber the metallic channel of the underlying surface; we rely on
    // alpha blending (glBlendFunc SRC_ALPHA, ONE_MINUS_SRC_ALPHA) on the
    // colour channels only. The alpha component written here is consumed by
    // blending and then overwritten back to whatever the underlying surface
    // stored via a write mask (see DecalRenderPass::ExecuteOnGBuffer).
    gAlbedo = vec4(decalColor.rgb, decalColor.a);
}
