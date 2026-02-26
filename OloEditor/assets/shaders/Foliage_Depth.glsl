// =============================================================================
// Foliage_Depth.glsl - Shadow depth pass for instanced foliage
// Matches Foliage_Instance.glsl vertex layout with alpha test support
// =============================================================================

#type vertex
#version 460 core

// Per-vertex attributes (unit quad)
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

// Per-instance attributes
layout(location = 2) in vec4 a_PositionScale;   // xyz = world pos, w = scale
layout(location = 3) in vec4 a_RotationHeight;  // x = Y rotation (rad), y = height, z = fade, w = unused
layout(location = 4) in vec4 a_ColorAlpha;       // rgb = tint, a = alpha cutoff

// Camera UBO (binding 0) — contains light VP during shadow pass
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Model UBO (binding 3)
layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

// Foliage UBO (binding 12)
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float _foliagePad0;
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
};

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out float v_AlphaCutoff;

void main()
{
    float scale = a_PositionScale.w;
    float rotation = a_RotationHeight.x;
    float height = a_RotationHeight.y;

    vec3 localPos = a_Position;
    localPos.x *= scale;
    localPos.y *= height * scale;

    float cosR = cos(rotation);
    float sinR = sin(rotation);
    vec3 rotatedPos;
    rotatedPos.x = localPos.x * cosR - localPos.z * sinR;
    rotatedPos.y = localPos.y;
    rotatedPos.z = localPos.x * sinR + localPos.z * cosR;

    // Wind (must match main shader for consistent shadows)
    float windInfluence = a_Position.y;
    float windPhase = (a_PositionScale.x + a_PositionScale.z) * 0.1 + u_Time * u_WindSpeed;
    float wind = sin(windPhase) * cos(windPhase * 0.7 + 1.3) * u_WindStrength * windInfluence;
    rotatedPos.x += wind;
    rotatedPos.z += wind * 0.5;

    vec3 instancePos = a_PositionScale.xyz;
    vec3 worldPos = (u_Model * vec4(instancePos + rotatedPos, 1.0)).xyz;

    v_TexCoord = a_TexCoord;
    v_AlphaCutoff = a_ColorAlpha.a;

    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in float v_AlphaCutoff;

layout(binding = 0) uniform sampler2D u_DiffuseTexture;

// Foliage UBO (binding 12) — shared with vertex stage
layout(std140, binding = 12) uniform FoliageParams
{
    float u_Time;
    float u_WindStrength;
    float u_WindSpeed;
    float u_ViewDistance;
    float u_FadeStart;
    float u_AlphaCutoff;
    float _foliagePad0;
    float _foliagePad1;
    vec3  u_FoliageBaseColor;
    float _foliagePad2;
};

void main()
{
    float alpha = texture(u_DiffuseTexture, v_TexCoord).a;
    if (alpha < v_AlphaCutoff)
        discard;
}
