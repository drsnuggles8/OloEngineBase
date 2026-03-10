// =============================================================================
// Water.glsl - Gerstner wave water surface rendering
// Vertex-displaced water plane with Fresnel reflection and depth-based coloring
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

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

// Water UBO (binding 23)
layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;      // x = Time, y = WaveSpeed, z = WaveAmplitude, w = WaveFrequency
    vec4 u_WaveDir0;        // xy = direction0, z = steepness0, w = wavelength0
    vec4 u_WaveDir1;        // xy = direction1, z = steepness1, w = wavelength1
    vec4 u_WaterColor;      // rgb = shallow color, a = Transparency
    vec4 u_WaterDeepColor;  // rgb = deep color,    a = Reflectivity
    vec4 u_VisualParams;    // x = FresnelPower, y = SpecularIntensity, z/w = unused
};

#include "include/WaterCommon.glsl"

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out vec3 v_ViewDir;

void main()
{
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);

    float time = u_WaveParams.x * u_WaveParams.y; // Time * WaveSpeed
    float amplitude = u_WaveParams.z;
    float frequency = u_WaveParams.w;

    vec3 displacedNormal;
    vec3 displacedPos = sumGerstnerWaves(
        worldPos.xyz, time,
        u_WaveDir0, u_WaveDir1,
        frequency, amplitude,
        displacedNormal
    );

    v_WorldPos = displacedPos;
    v_Normal = displacedNormal;
    v_TexCoord = a_TexCoord;
    v_ViewDir = normalize(u_CameraPosition - displacedPos);

    gl_Position = u_ViewProjection * vec4(displacedPos, 1.0);
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;
layout(location = 3) in vec3 v_ViewDir;

// MRT outputs matching SceneRenderPass format
layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

// Octahedral encode: unit normal -> RG16F [-1,1]^2
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Model UBO (binding 3) for entity ID
layout(std140, binding = 3) uniform ModelMatrices
{
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

// Water UBO (binding 23)
layout(std140, binding = 23) uniform WaterParams
{
    vec4 u_WaveParams;
    vec4 u_WaveDir0;
    vec4 u_WaveDir1;
    vec4 u_WaterColor;      // rgb = shallow, a = transparency
    vec4 u_WaterDeepColor;  // rgb = deep,    a = reflectivity
    vec4 u_VisualParams;    // x = FresnelPower, y = SpecularIntensity
};

// Environment map for reflection (same slot as PBR shaders)
layout(binding = 9) uniform samplerCube u_EnvironmentMap;

void main()
{
    vec3 normal = normalize(v_Normal);
    vec3 viewDir = normalize(v_ViewDir);

    // Fresnel: F0 ~ 0.02 for water (IOR ~1.33)
    vec3 F0 = vec3(0.02);
    float NdotV = max(dot(normal, viewDir), 0.0);
    vec3 fresnel = fresnelSchlick(NdotV, F0);
    float fresnelFactor = fresnel.r;

    // Boost fresnel with configurable power for artistic control
    float fresnelPower = u_VisualParams.x;
    fresnelFactor = pow(1.0 - NdotV, fresnelPower) * u_WaterDeepColor.a; // .a = reflectivity

    // Reflection from environment cubemap
    vec3 reflectDir = reflect(-viewDir, normal);
    vec3 reflectionColor = texture(u_EnvironmentMap, reflectDir).rgb;

    // Blend shallow and deep water colors based on view angle
    vec3 shallowColor = u_WaterColor.rgb;
    vec3 deepColor = u_WaterDeepColor.rgb;
    vec3 waterBaseColor = mix(shallowColor, deepColor, 1.0 - NdotV);

    // Combine reflection with water color via Fresnel
    vec3 finalColor = mix(waterBaseColor, reflectionColor, fresnelFactor);

    // Specular highlight from primary directional light direction (simplified)
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 halfVec = normalize(viewDir + lightDir);
    float specAngle = max(dot(normal, halfVec), 0.0);
    float specular = pow(specAngle, 256.0) * u_VisualParams.y; // .y = SpecularIntensity
    finalColor += vec3(specular);

    float transparency = u_WaterColor.a; // .a = transparency

    o_Color = vec4(finalColor, transparency);
    o_EntityID = u_EntityID;
    o_ViewNormal = octEncode(normalize(mat3(u_View) * normal));
}
