// =============================================================================
// PBR.glsl - Physically Based Rendering Shader
// Part of OloEngine PBR System
// Supports metallic-roughness workflow (glTF 2.0 standard)
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Model UBO (binding 3)
layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
};

// Output to fragment shader
layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    v_WorldPos = vec3(u_Model * vec4(a_Position, 1.0));
    v_Normal = mat3(u_Normal) * a_Normal;
    v_TexCoord = a_TexCoord;
    
    gl_Position = u_ViewProjection * vec4(v_WorldPos, 1.0);
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"

// Input from vertex shader
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

// Output
layout(location = 0) out vec4 o_Color;

// Light UBO (binding 1)
layout(std140, binding = 1) uniform LightProperties {
    vec4 u_LightPosition;
    vec4 u_LightDirection;
    vec4 u_LightAmbient;
    vec4 u_LightDiffuse;
    vec4 u_LightSpecular;
    vec4 u_LightAttParams;      // (constant, linear, quadratic, _)
    vec4 u_LightSpotParams;     // (cutOff, outerCutOff, _, _)
    vec4 u_ViewPosAndLightType; // (viewPos.xyz, lightType)
};

// Camera UBO (binding 0) - for view position
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// PBR Material UBO (binding 2)
layout(std140, binding = 2) uniform PBRMaterialProperties {
    vec4 u_BaseColorFactor;     // Base color (albedo) with alpha
    vec4 u_EmissiveFactor;      // Emissive color
    float u_MetallicFactor;     // Metallic factor
    float u_RoughnessFactor;    // Roughness factor
    float u_NormalScale;        // Normal map scale
    float u_OcclusionStrength;  // AO strength
    int u_UseAlbedoMap;         // Use albedo texture
    int u_UseNormalMap;         // Use normal map
    int u_UseMetallicRoughnessMap; // Use metallic-roughness texture
    int u_UseAOMap;             // Use ambient occlusion map
    int u_UseEmissiveMap;       // Use emissive map
    int u_EnableIBL;            // Enable IBL
    int _padding[2];
};

// Texture bindings following ShaderBindingLayout
layout(binding = 0) uniform sampler2D u_AlbedoMap;          // TEX_DIFFUSE
layout(binding = 1) uniform sampler2D u_MetallicRoughnessMap; // TEX_SPECULAR (repurposed)
layout(binding = 2) uniform sampler2D u_NormalMap;          // TEX_NORMAL
layout(binding = 4) uniform sampler2D u_AOMap;              // TEX_AMBIENT
layout(binding = 5) uniform sampler2D u_EmissiveMap;        // TEX_EMISSIVE
layout(binding = 9) uniform samplerCube u_EnvironmentMap;   // TEX_ENVIRONMENT

// IBL textures (if available)
layout(binding = 10) uniform samplerCube u_IrradianceMap;   // TEX_USER_0
layout(binding = 11) uniform samplerCube u_PrefilterMap;    // TEX_USER_1
layout(binding = 12) uniform sampler2D u_BRDFLutMap;        // TEX_USER_2

// Normal mapping function
vec3 getNormalFromMapLocal()
{
    if (u_UseNormalMap == 0)
    {
        return normalize(v_Normal);
    }
    
    return getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
}

// Calculate directional light contribution
vec3 calculateDirectionalLight(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness)
{
    vec3 L = normalize(-u_LightDirection.xyz);
    vec3 radiance = u_LightDiffuse.rgb;
    
    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
    
    return brdf * radiance * NdotL;
}

// Calculate point light contribution
vec3 calculatePointLight(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness)
{
    vec3 L = normalize(u_LightPosition.xyz - v_WorldPos);
    float distance = length(u_LightPosition.xyz - v_WorldPos);
    float attenuation = 1.0 / (u_LightAttParams.x + u_LightAttParams.y * distance + u_LightAttParams.z * distance * distance);
    vec3 radiance = u_LightDiffuse.rgb * attenuation;
    
    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
    
    return brdf * radiance * NdotL;
}

// Calculate spot light contribution
vec3 calculateSpotLight(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness)
{
    vec3 L = normalize(u_LightPosition.xyz - v_WorldPos);
    float distance = length(u_LightPosition.xyz - v_WorldPos);
    float attenuation = 1.0 / (u_LightAttParams.x + u_LightAttParams.y * distance + u_LightAttParams.z * distance * distance);
    
    // Spot light calculation
    float theta = dot(L, normalize(-u_LightDirection.xyz));
    float epsilon = u_LightSpotParams.x - u_LightSpotParams.y;
    float intensity = clamp((theta - u_LightSpotParams.y) / epsilon, 0.0, 1.0);
    
    vec3 radiance = u_LightDiffuse.rgb * attenuation * intensity;
    
    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
    
    return brdf * radiance * NdotL;
}

void main()
{
    // Sample material properties
    vec3 albedo = u_BaseColorFactor.rgb;
    if (u_UseAlbedoMap == 1) {
        albedo *= texture(u_AlbedoMap, v_TexCoord).rgb;
    }
    
    float metallic = u_MetallicFactor;
    float roughness = u_RoughnessFactor;
    if (u_UseMetallicRoughnessMap == 1) {
        vec3 metallicRoughness = texture(u_MetallicRoughnessMap, v_TexCoord).rgb;
        metallic *= metallicRoughness.b;  // Blue channel = metallic
        roughness *= metallicRoughness.g; // Green channel = roughness
    }
    
    float ao = 1.0;
    if (u_UseAOMap == 1)
    {
        ao = texture(u_AOMap, v_TexCoord).r;
        ao = mix(1.0, ao, u_OcclusionStrength);
    }
    
    vec3 emissive = u_EmissiveFactor.rgb;
    if (u_UseEmissiveMap == 1) {
        emissive *= texture(u_EmissiveMap, v_TexCoord).rgb;
    }
    
    // Calculate normal
    vec3 N = getNormalFromMapLocal();
    vec3 V = normalize(u_CameraPosition - v_WorldPos);
    
    // Calculate direct lighting
    vec3 Lo = vec3(0.0);
    int lightType = int(u_ViewPosAndLightType.w);
    
    if (lightType == DIRECTIONAL_LIGHT) {
        Lo = calculateDirectionalLight(N, V, albedo, metallic, roughness);
    } else if (lightType == POINT_LIGHT) {
        Lo = calculatePointLight(N, V, albedo, metallic, roughness);
    } else if (lightType == SPOT_LIGHT) {
        Lo = calculateSpotLight(N, V, albedo, metallic, roughness);
    }
    
    // Calculate ambient lighting
    vec3 ambient = vec3(0.0);
    if (u_EnableIBL == 1) {
        ambient = calculateIBL(N, V, albedo, metallic, roughness, u_IrradianceMap, u_PrefilterMap, u_BRDFLutMap);
    } else {
        ambient = calculateSimpleAmbient(albedo, metallic, ao);
    }
    
    // Combine lighting
    vec3 color = ambient + Lo + emissive;
    
    // Apply ambient occlusion to ambient lighting only
    color = mix(color, color * ao, 0.5);
    
    // HDR tonemapping
    color = reinhardToneMapping(color);
    
    // Gamma correction
    color = linearToSRGB(color);
    
    o_Color = vec4(color, u_BaseColorFactor.a);
}