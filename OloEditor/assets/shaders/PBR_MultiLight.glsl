// =============================================================================
// PBR_MultiLight.glsl - Physically Based Rendering Shader with Multi-Light Support
// Part of OloEngine Enhanced PBR System
// Supports metallic-roughness workflow (glTF 2.0 standard) with multiple lights
// =============================================================================

#type vertex
#version 460 core

// Include shared constants from ShaderConstants.h
#define DIRECTIONAL_LIGHT 0
#define POINT_LIGHT 1
#define SPOT_LIGHT 2
#define PI 3.14159265359
#define EPSILON 0.0001
#define DEFAULT_DIELECTRIC_F0 0.04
#define MAX_REFLECTION_LOD 4.0
#define GAMMA 2.2
#define MAX_LIGHTS 32

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

// Include shared constants
#define DIRECTIONAL_LIGHT 0
#define POINT_LIGHT 1
#define SPOT_LIGHT 2
#define PI 3.14159265359
#define EPSILON 0.0001
#define DEFAULT_DIELECTRIC_F0 0.04
#define MAX_REFLECTION_LOD 4.0
#define GAMMA 2.2
#define MAX_LIGHTS 32

// =============================================================================
// LIGHT DATA STRUCTURE
// =============================================================================

struct LightData {
    vec4 position;         // Position in world space (w = light type)
    vec4 direction;        // Direction for directional/spot lights
    vec4 color;            // Light color and intensity (w = intensity)
    vec4 attenuationParams; // (constant, linear, quadratic, range)
    vec4 spotParams;       // (inner_cutoff, outer_cutoff, falloff, enabled)
};

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
// BRDF FUNCTIONS
// =============================================================================

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
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0); // Dielectric F0 for most materials
    F0 = mix(F0, albedo, metallic);
    
    // Calculate the three components of the BRDF
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    
    // Calculate specular BRDF
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + EPSILON; // Prevent division by zero
    vec3 specular = numerator / denominator;
    
    // Calculate diffuse contribution
    vec3 kS = F; // Fresnel term represents the ratio of light that gets reflected
    vec3 kD = vec3(1.0) - kS; // Remaining light gets refracted
    kD *= 1.0 - metallic; // Metallic materials don't refract light
    
    return kD * albedo / PI + specular;
}

// =============================================================================
// IBL FUNCTIONS
// =============================================================================

// Calculate ambient lighting using IBL
vec3 calculateIBL(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, 
                  samplerCube irradianceMap, samplerCube prefilterMap, sampler2D brdfLUT)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
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
// NORMAL MAPPING
// =============================================================================

vec3 getNormalFromMap()
{
    if (u_UseNormalMap == 0)
    {
        return normalize(v_Normal);
    }
    
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

// =============================================================================
// MULTI-LIGHT CALCULATIONS
// =============================================================================

// Calculate attenuation for point and spot lights
float calculateAttenuation(vec3 lightPos, vec3 fragPos, vec4 attenuationParams)
{
    float distance = length(lightPos - fragPos);
    float range = attenuationParams.w;
    
    if (distance > range) return 0.0;
    
    float constant = attenuationParams.x;
    float linear = attenuationParams.y;
    float quadratic = attenuationParams.z;
    
    return 1.0 / (constant + linear * distance + quadratic * (distance * distance));
}

// Calculate spot light intensity
float calculateSpotIntensity(vec3 lightDir, vec3 spotDir, vec4 spotParams)
{
    float innerCutoff = spotParams.x;
    float outerCutoff = spotParams.y;
    
    float theta = dot(lightDir, normalize(-spotDir));
    float epsilon = innerCutoff - outerCutoff;
    float intensity = clamp((theta - outerCutoff) / epsilon, 0.0, 1.0);
    
    return intensity;
}

// Calculate contribution from a single light
vec3 calculateLightContribution(LightData light, vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 worldPos)
{
    int lightType = int(light.position.w);
    vec3 lightColor = light.color.rgb;
    float lightIntensity = light.color.w;
    
    vec3 L;
    float attenuation = 1.0;
    
    if (lightType == DIRECTIONAL_LIGHT)
    {
        L = normalize(-light.direction.xyz);
    }
    else if (lightType == POINT_LIGHT)
    {
        L = normalize(light.position.xyz - worldPos);
        attenuation = calculateAttenuation(light.position.xyz, worldPos, light.attenuationParams);
    }
    else if (lightType == SPOT_LIGHT)
    {
        L = normalize(light.position.xyz - worldPos);
        attenuation = calculateAttenuation(light.position.xyz, worldPos, light.attenuationParams);
        float spotIntensity = calculateSpotIntensity(L, light.direction.xyz, light.spotParams);
        attenuation *= spotIntensity;
    }
    else
    {
        return vec3(0.0); // Unknown light type
    }
    
    // Early exit if light has no contribution
    if (attenuation <= EPSILON) return vec3(0.0);
    
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= EPSILON) return vec3(0.0);
    
    vec3 radiance = lightColor * lightIntensity * attenuation;
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
    
    return brdf * radiance * NdotL;
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
    vec3 N = getNormalFromMap();
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
    
    // HDR tonemapping (simple Reinhard)
    color = color / (color + vec3(1.0));
    
    // Gamma correction
    color = pow(color, vec3(1.0/GAMMA));
    
    o_Color = vec4(color, u_BaseColorFactor.a);
}
