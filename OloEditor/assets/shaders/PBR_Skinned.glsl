// =============================================================================
// PBR_Skinned.glsl - Physically Based Rendering Shader with Skeletal Animation
// Part of OloEngine PBR System
// Supports metallic-roughness workflow (glTF 2.0 standard) with bone animation
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in ivec4 a_BoneIndices;
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
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

// Animation UBO (binding 4)
layout(std140, binding = 4) uniform AnimationMatrices {
    mat4 u_BoneMatrices[100];
};

// Output to fragment shader
layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;

void main()
{
    // Calculate bone transformation
    mat4 boneTransform = u_BoneMatrices[a_BoneIndices[0]] * a_BoneWeights[0];
    boneTransform += u_BoneMatrices[a_BoneIndices[1]] * a_BoneWeights[1];
    boneTransform += u_BoneMatrices[a_BoneIndices[2]] * a_BoneWeights[2];
    boneTransform += u_BoneMatrices[a_BoneIndices[3]] * a_BoneWeights[3];

    // Transform vertex position and normal with bone transformation
    vec4 animatedPosition = boneTransform * vec4(a_Position, 1.0);
    vec3 animatedNormal = mat3(boneTransform) * a_Normal;

    // Transform to world space
    v_WorldPos = vec3(u_Model * animatedPosition);
    v_Normal = mat3(u_Normal) * animatedNormal;
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
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;

// Octahedral encode: unit normal → RG16F [-1,1]²
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return n.xy;
}

// Model UBO (binding 3) for entity ID access
layout(std140, binding = 3) uniform ModelMatrices {
    mat4 u_Model;
    mat4 u_Normal;
    int u_EntityID;
    int _paddingEntity0;
    int _paddingEntity1;
    int _paddingEntity2;
};

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
    int u_ApplyGammaCorrection; // Apply gamma correction in this pass
    int u_AlphaCutoff;          // Alpha cutoff for transparency
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

// Shadow map textures
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM; // TEX_SHADOW (CSM)

// Shadow UBO (binding 6)
layout(std140, binding = 6) uniform ShadowData {
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;  // x=bias, y=normalBias, z=softness, w=maxShadowDistance
    mat4 u_SpotLightSpaceMatrices[4];
    vec4 u_PointLightShadowParams[4]; // xyz=position, w=farPlane
    int u_DirectionalShadowEnabled;
    int u_SpotShadowCount;
    int u_PointShadowCount;
    int u_ShadowMapResolution;
    int u_CascadeDebugEnabled;
    int _shadowPad0;
    int _shadowPad1;
    int _shadowPad2;
};

void main()
{
    // Sample material properties
    vec3 albedo = sampleAlbedo(u_AlbedoMap, v_TexCoord, u_BaseColorFactor.rgb, bool(u_UseAlbedoMap));
    vec2 metallicRoughness = sampleMetallicRoughness(u_MetallicRoughnessMap, v_TexCoord,
                                                     u_MetallicFactor, u_RoughnessFactor,
                                                     bool(u_UseMetallicRoughnessMap));
    float metallic = metallicRoughness.x;
    float roughness = metallicRoughness.y;

    float ao = sampleAO(u_AOMap, v_TexCoord, u_OcclusionStrength, bool(u_UseAOMap));
    vec3 emissive = sampleEmissive(u_EmissiveMap, v_TexCoord, u_EmissiveFactor.rgb, bool(u_UseEmissiveMap));

    // Calculate normal
    vec3 N = normalize(v_Normal);
    if (u_UseNormalMap == 1)
    {
        N = getNormalFromMap(u_NormalMap, v_TexCoord, v_WorldPos, v_Normal, u_NormalScale);
    }
    vec3 V = normalize(u_CameraPosition - v_WorldPos);

    // Calculate direct lighting
    vec3 Lo = vec3(0.0);
    int lightType = int(u_ViewPosAndLightType.w);

    if (lightType == DIRECTIONAL_LIGHT)
    {
        Lo = calculateDirectionalLightUniform(N, V, albedo, metallic, roughness,
                                             u_LightDirection.xyz, u_LightDiffuse.rgb);

        // Apply CSM shadow factor for directional light
        if (u_DirectionalShadowEnabled != 0)
        {
            vec4 viewSpacePos = u_View * vec4(v_WorldPos, 1.0);
            float viewDepth = viewSpacePos.z;
            float shadow = calculateCascadedShadowFactorCSM(
                u_ShadowMapCSM, v_WorldPos, viewDepth,
                u_DirectionalLightSpaceMatrices, u_CascadePlaneDistances,
                u_ShadowParams, u_ShadowMapResolution
            );
            Lo *= shadow;
        }
    }
    else if (lightType == POINT_LIGHT)
    {
        Lo = calculatePointLightUniform(N, V, albedo, metallic, roughness, v_WorldPos,
                                       u_LightPosition.xyz, u_LightDiffuse.rgb, u_LightAttParams);
    }
    else if (lightType == SPOT_LIGHT)
    {
        Lo = calculateSpotLightUniform(N, V, albedo, metallic, roughness, v_WorldPos,
                                      u_LightPosition.xyz, u_LightDirection.xyz,
                                      u_LightDiffuse.rgb, u_LightAttParams, u_LightSpotParams);
    }

    // Calculate ambient lighting
    vec3 ambient = vec3(0.0);
    if (u_EnableIBL == 1)
    {
        ambient = calculateIBL(N, V, albedo, metallic, roughness, u_IrradianceMap, u_PrefilterMap, u_BRDFLutMap);
    }
    else
    {
        ambient = calculateSimpleAmbient(albedo, metallic, ao);
    }

    // Combine lighting
    vec3 color = ambient + Lo + emissive;

    // Apply ambient occlusion to ambient lighting only
    color = mix(color, color * ao, 0.5);

    // Output linear HDR color — tone mapping and gamma handled in post-process pass
    o_Color = vec4(color, u_BaseColorFactor.a);
    o_EntityID = u_EntityID;
    o_ViewNormal = octEncode(normalize(mat3(u_View) * N));
}
