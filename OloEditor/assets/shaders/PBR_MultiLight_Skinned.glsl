// =============================================================================
// PBR_MultiLight_Skinned.glsl - PBR Shader with Multi-Light Support for Skinned Meshes
// Part of OloEngine Enhanced PBR System
// Supports skeletal animation with bone matrices and multi-light PBR rendering
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in vec4 a_BoneIDs;
layout(location = 4) in vec4 a_BoneWeights;

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

// Bone Matrices UBO (binding 4)
layout(std140, binding = 4) uniform BoneMatrices {
    mat4 u_BoneTransforms[100];
};

// Output to fragment shader
layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    // Calculate bone transformation
    mat4 boneTransform = mat4(0.0);
    for (int i = 0; i < 4; ++i)
    {
        int boneID = int(a_BoneIDs[i]);
        if (boneID >= 0 && boneID < 100)
        {
            boneTransform += u_BoneTransforms[boneID] * a_BoneWeights[i];
        }
    }
    
    // Transform position and normal by bones
    vec4 localPosition = boneTransform * vec4(a_Position, 1.0);
    vec3 localNormal = mat3(boneTransform) * a_Normal;
    
    // Transform to world space
    v_WorldPos = vec3(u_Model * localPosition);
    v_Normal = mat3(u_Normal) * localNormal;
    v_TexCoord = a_TexCoord;
    
    gl_Position = u_ViewProjection * vec4(v_WorldPos, 1.0);
}

#type fragment
#version 460 core

#include "include/PBRCommon.glsl"

// =============================================================================
// UNIFORM BUFFER OBJECTS
// =============================================================================

// Camera UBO (binding 0) - for view position
layout(std140, binding = 0) uniform CameraMatrices {
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Multi-Light UBO (binding 5)
layout(std140, binding = 5) uniform MultiLightBuffer {
    int u_LightCount;
    int _padding[3];
    LightData u_Lights[MAX_LIGHTS];
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
    int _padding2[2];
};

// =============================================================================
// TEXTURE BINDINGS
// =============================================================================

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

// =============================================================================
// INPUT/OUTPUT
// =============================================================================

// Input from vertex shader
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

// Output
layout(location = 0) out vec4 o_Color;

// =============================================================================
// NORMAL MAPPING
// =============================================================================

vec3 getNormalFromMapLocal()
{
    if (u_UseNormalMap == 0)
    {
        return normalize(v_Normal);
    }
    
    return getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
}

// =============================================================================
// MAIN FRAGMENT SHADER
// =============================================================================

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
    
    // Calculate direct lighting from all lights
    vec3 Lo = vec3(0.0);
    for (int i = 0; i < min(u_LightCount, MAX_LIGHTS); ++i)
    {
        Lo += calculateLightContribution(u_Lights[i], N, V, albedo, metallic, roughness, v_WorldPos);
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