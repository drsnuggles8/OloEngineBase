// =============================================================================
// Foliage_Instance.glsl - Instanced foliage rendering with wind animation
// Uses per-instance data for position, scale, rotation, and tint
// Supports alpha-to-coverage for grass/vegetation cutouts
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

// Camera UBO (binding 0)
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

// Wind field (optional — provides direction-aware wind when enabled)
#include "include/WindSampling.glsl"

// Outputs
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

    // Scale the quad by instance scale and height
    vec3 localPos = a_Position;
    localPos.x *= scale;
    localPos.y *= height * scale;

    // Apply Y-axis rotation
    float cosR = cos(rotation);
    float sinR = sin(rotation);
    vec3 rotatedPos;
    rotatedPos.x = localPos.x * cosR - localPos.z * sinR;
    rotatedPos.y = localPos.y;
    rotatedPos.z = localPos.x * sinR + localPos.z * cosR;

    // Wind animation — direction-aware when WindSystem is enabled,
    // otherwise falls back to legacy sine-wave model.
    float windInfluence = a_Position.y; // 0 at base, 1 at top

    if (windEnabled())
    {
        // Sample wind field at blade root world position
        vec3 bladeWorldPos = (u_Model * vec4(a_PositionScale.xyz, 1.0)).xyz;
        vec3 windVel = analyticalWind(bladeWorldPos); // Fast analytical path for vertex shader
        // Displace blade tip along wind direction, scaled by per-layer strength
        rotatedPos.xyz += windVel * u_WindStrength * windInfluence * 0.1;
    }
    else
    {
        // Legacy sine-wave wind
        float windPhase = (a_PositionScale.x + a_PositionScale.z) * 0.1 + u_Time * u_WindSpeed;
        float wind = sin(windPhase) * cos(windPhase * 0.7 + 1.3) * u_WindStrength * windInfluence;
        rotatedPos.x += wind;
        rotatedPos.z += wind * 0.5;
    }

    // World position
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

layout(location = 0) out vec4 FragColor;

// Inputs
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_Color;
layout(location = 4) in float v_AlphaCutoff;
layout(location = 5) in float v_Fade;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Multi-light UBO (binding 5)
layout(std140, binding = 5) uniform MultiLightData
{
    int u_NumLights;
    int _ml_pad0;
    int _ml_pad1;
    int _ml_pad2;
    // Light[0]
    vec4 u_Light0_Position;
    vec4 u_Light0_Direction;
    vec4 u_Light0_ColorIntensity;
    vec4 u_Light0_Params;
    vec4 u_Light0_Params2;
};

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
    // Sample albedo
    vec4 texColor = texture(u_DiffuseTexture, v_TexCoord);
    vec4 color = vec4(texColor.rgb * v_Color, texColor.a);

    // Alpha test
    if (color.a < v_AlphaCutoff)
        discard;

    // Distance fade
    float dist = distance(v_WorldPos, u_CameraPosition);
    float fadeFactor = 1.0 - smoothstep(u_FadeStart, u_ViewDistance, dist);
    if (fadeFactor <= 0.0)
        discard;

    color.a *= fadeFactor * v_Fade;

    // Simple directional lighting (first light assumed directional)
    vec3 normal = normalize(v_Normal);
    vec3 lightDir = normalize(-u_Light0_Direction.xyz);
    float NdotL = max(dot(normal, lightDir), 0.0);

    // Two-sided lighting for foliage
    if (NdotL < 0.01)
    {
        NdotL = max(dot(-normal, lightDir), 0.0) * 0.5;
    }

    vec3 lightColor = u_Light0_ColorIntensity.rgb * u_Light0_ColorIntensity.w;
    vec3 ambient = color.rgb * 0.3;
    vec3 diffuse = color.rgb * lightColor * NdotL;

    FragColor = vec4(ambient + diffuse, color.a);
}
