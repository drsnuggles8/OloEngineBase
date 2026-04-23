// =============================================================================
// Foliage_Instance_GBuffer.glsl — Deferred G-Buffer variant of
// Foliage_Instance.glsl. Same instanced VS (wind + fade + rotation); FS writes
// material data into the 4-RT G-Buffer so foliage participates in the deferred
// lighting composite. Alpha-tested cutouts are expressed as hard `discard`
// (G-Buffer has no alpha blending).
//
// `emissive.a = 0.0` → lit (full PBR + directional shadow via DeferredLightingPass).
// Zero velocity (foliage is animated by wind in VS only; per-instance velocity
// would require previous-frame instance data — future work).
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec4 a_PositionScale;
layout(location = 3) in vec4 a_RotationHeight;
layout(location = 4) in vec4 a_ColorAlpha;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

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

#include "include/WindSampling.glsl"

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_Color;
layout(location = 4) out float v_AlphaCutoff;
layout(location = 5) out float v_Fade;

void main()
{
    float scale = a_PositionScale.w;
    float rotation = a_RotationHeight.x;
    float height = a_RotationHeight.y;
    float fade = a_RotationHeight.z;

    vec3 localPos = a_Position;
    localPos.x *= scale;
    localPos.y *= height * scale;

    float cosR = cos(rotation);
    float sinR = sin(rotation);
    vec3 rotatedPos;
    rotatedPos.x = localPos.x * cosR - localPos.z * sinR;
    rotatedPos.y = localPos.y;
    rotatedPos.z = localPos.x * sinR + localPos.z * cosR;

    float windInfluence = a_Position.y;
    if (windEnabled())
    {
        vec3 bladeWorldPos = (u_Model * vec4(a_PositionScale.xyz, 1.0)).xyz;
        vec3 windVel = analyticalWind(bladeWorldPos);
        rotatedPos.xyz += windVel * u_WindStrength * windInfluence * 0.1;
    }
    else
    {
        float windPhase = (a_PositionScale.x + a_PositionScale.z) * 0.1 + u_Time * u_WindSpeed;
        float wind = sin(windPhase) * cos(windPhase * 0.7 + 1.3) * u_WindStrength * windInfluence;
        rotatedPos.x += wind;
        rotatedPos.z += wind * 0.5;
    }

    vec3 instancePos = a_PositionScale.xyz;
    vec3 worldPos = (u_Model * vec4(instancePos + rotatedPos, 1.0)).xyz;

    v_WorldPos = worldPos;
    v_Normal = normalize(mat3(u_Normal) * vec3(0.0, 1.0, 0.0));
    v_TexCoord = a_TexCoord;
    v_Color = a_ColorAlpha.rgb;
    v_AlphaCutoff = a_ColorAlpha.a;
    v_Fade = fade;

    gl_Position = u_ViewProjection * vec4(worldPos, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_Color;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Fade;

layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

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

layout(binding = 0) uniform sampler2D u_DiffuseTexture;

layout(location = 0) out vec4 o_GBufferAlbedo;
layout(location = 1) out vec4 o_GBufferNormal;
layout(location = 2) out vec4 o_GBufferEmissive;
layout(location = 3) out vec2 o_GBufferVelocity;

vec2 octEncodeGB(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                        n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

void main()
{
    vec4 texColor = texture(u_DiffuseTexture, v_TexCoord);
    vec3 albedo = texColor.rgb * v_Color;

    if (texColor.a < v_AlphaCutoff)
        discard;

    float dist = distance(v_WorldPos, u_CameraPosition);
    float fadeFactor = 1.0 - smoothstep(u_FadeStart, u_ViewDistance, dist);
    if (fadeFactor <= 0.001)
        discard;

    // G-Buffer has no alpha blending — collapse fade into a hard discard
    // threshold instead of modulating alpha.
    float alpha = texColor.a * fadeFactor * v_Fade;
    if (alpha < 0.3)
        discard;

    vec3 N = normalize(v_Normal);
    // Foliage is primarily diffuse — non-metallic, rough, full AO.
    float metallic = 0.0;
    float roughness = 0.9;
    float ao = 1.0;

    o_GBufferAlbedo   = vec4(albedo, metallic);
    o_GBufferNormal   = vec4(octEncodeGB(N), roughness, ao);
    o_GBufferEmissive = vec4(0.0, 0.0, 0.0, 0.0); // lit
    o_GBufferVelocity = vec2(0.0);
}
