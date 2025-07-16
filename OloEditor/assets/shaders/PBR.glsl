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

// =============================================================================
// BRDF Functions (inlined to avoid include issues)
// =============================================================================

const float PI = 3.14159265359;

// Schlick-Fresnel approximation for F0
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// GGX/Trowbridge-Reitz distribution function
float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

// Smith's method for masking-shadowing function
float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / denom;
}

// Smith's method considering both geometry obstruction and geometry shadowing
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Cook-Torrance BRDF implementation
vec3 cookTorranceBRDF(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);
    
    // Calculate F0 based on metallic workflow
    vec3 F0 = vec3(0.04); // Dielectric F0 for most materials
    F0 = mix(F0, albedo, metallic);
    
    // Calculate the three components of the BRDF
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    
    // Calculate specular BRDF
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001; // Prevent division by zero
    vec3 specular = numerator / denominator;
    
    // Calculate diffuse contribution
    vec3 kS = F; // Fresnel term represents the ratio of light that gets reflected
    vec3 kD = vec3(1.0) - kS; // Remaining light gets refracted
    kD *= 1.0 - metallic; // Metallic materials don't refract light
    
    return kD * albedo / PI + specular;
}

// =============================================================================
// IBL Functions (inlined to avoid include issues)
// =============================================================================

// Sample environment map for IBL
vec3 sampleEnvironmentMap(samplerCube envMap, vec3 direction, float roughness)
{
    // For now, simple sampling without importance sampling
    // In a full implementation, this would use importance sampling based on roughness
    return texture(envMap, direction).rgb;
}

// Calculate ambient lighting using IBL
vec3 calculateIBL(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, 
                  samplerCube irradianceMap, samplerCube prefilterMap, sampler2D brdfLUT)
{
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);
    
    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    
    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;
    
    // Diffuse IBL
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuse = irradiance * albedo;
    
    // Specular IBL
    vec3 R = reflect(-V, N);
    const float MAX_REFLECTION_LOD = 4.0;
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBRDF = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);
    
    return kD * diffuse + specular;
}

// Simple ambient lighting fallback when IBL is not available
vec3 calculateSimpleAmbient(vec3 albedo, float metallic, float ao)
{
    vec3 ambient = vec3(0.03) * albedo * ao;
    return ambient;
}

// =============================================================================
// Main PBR Shader
// =============================================================================

// Input from vertex shader
layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord;

// Output
layout(location = 0) out vec4 o_Color;

// Light types
const int DIRECTIONAL_LIGHT = 0;
const int POINT_LIGHT = 1;
const int SPOT_LIGHT = 2;

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
vec3 getNormalFromMap()
{
    if (u_UseNormalMap == 0)
        return normalize(v_Normal);
        
    vec3 tangentNormal = texture(u_NormalMap, v_TexCoord).xyz * 2.0 - 1.0;
    tangentNormal.xy *= u_NormalScale;
    
    vec3 Q1 = dFdx(v_WorldPos);
    vec3 Q2 = dFdy(v_WorldPos);
    vec2 st1 = dFdx(v_TexCoord);
    vec2 st2 = dFdy(v_TexCoord);
    
    vec3 N = normalize(v_Normal);
    vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);
    vec3 B = -normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);
    
    return normalize(TBN * tangentNormal);
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
    if (u_UseAOMap == 1) {
        ao = texture(u_AOMap, v_TexCoord).r;
        ao = mix(1.0, ao, u_OcclusionStrength);
    }
    
    vec3 emissive = u_EmissiveFactor.rgb;
    if (u_UseEmissiveMap == 1) {
        emissive *= texture(u_EmissiveMap, v_TexCoord).rgb;
    }
    
    // Calculate normal
    vec3 N = getNormalFromMap();
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
    
    // HDR tonemapping (simple Reinhard)
    color = color / (color + vec3(1.0));
    
    // Gamma correction
    color = pow(color, vec3(1.0/2.2));
    
    o_Color = vec4(color, u_BaseColorFactor.a);
}
