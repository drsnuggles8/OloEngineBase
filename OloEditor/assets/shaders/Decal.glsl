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

layout(location = 0) out vec4 FragColor;
layout(location = 1) out int EntityID;

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
    // Reconstruct screen UV from clip-space position
    vec2 screenUV = (v_ClipPos.xy / v_ClipPos.w) * 0.5 + 0.5;

    // Sample scene depth
    float depth = texture(u_SceneDepth, screenUV).r;

    // Reconstruct world position from depth
    vec4 clipPos = vec4(screenUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos4 = u_InverseViewProjection * clipPos;
    vec3 worldPos = worldPos4.xyz / worldPos4.w;

    // Transform world position into decal local space
    vec3 localPos = (u_InverseDecalTransform * vec4(worldPos, 1.0)).xyz;

    // Discard fragments outside the unit cube [-0.5, 0.5]
    if (abs(localPos.x) > 0.5 || abs(localPos.y) > 0.5 || abs(localPos.z) > 0.5)
        discard;

    // Use XZ coordinates as UV (remap from [-0.5, 0.5] to [0, 1])
    vec2 decalUV = localPos.xz + 0.5;

    // Sample decal albedo texture
    vec4 decalColor = texture(u_DecalAlbedo, decalUV) * u_DecalColor;

    // Edge fade based on distance from box boundaries
    float fadeDistance = max(u_DecalParams.x, 0.001);
    float fadeX = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.x));
    float fadeY = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.y));
    float fadeZ = smoothstep(0.0, fadeDistance, 0.5 - abs(localPos.z));
    float fade = fadeX * fadeY * fadeZ;

    decalColor.a *= fade;

    // Discard fully transparent fragments
    if (decalColor.a < 0.001)
        discard;

    FragColor = decalColor;
    EntityID = v_EntityID;
}
