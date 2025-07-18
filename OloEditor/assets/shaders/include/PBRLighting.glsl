// =============================================================================
// PBRLighting.glsl - Advanced lighting calculations for PBR
// Part of OloEngine Enhanced PBR System
// This file contains lighting functions for multiple light types
// =============================================================================

#ifndef PBR_LIGHTING_GLSL
#define PBR_LIGHTING_GLSL

// Include dependencies
#include "PBRConstants.glsl"
#include "PBRFunctions.glsl"

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
// ATTENUATION FUNCTIONS
// =============================================================================

// Calculate physically-based attenuation for point and spot lights
float calculateAttenuation(vec3 lightPos, vec3 fragPos, vec4 attenuationParams)
{
    float distance = length(lightPos - fragPos);
    float range = attenuationParams.w;
    
    // Early exit if beyond range
    if (distance > range) return 0.0;
    
    float constant = attenuationParams.x;
    float linear = attenuationParams.y;
    float quadratic = attenuationParams.z;
    
    // Standard attenuation formula
    float attenuation = 1.0 / (constant + linear * distance + quadratic * (distance * distance));
    
    // Smooth cutoff at range boundary
    float falloff = SATURATE(1.0 - pow(distance / range, 4.0));
    return attenuation * falloff * falloff;
}

// Epic Games' physically-based attenuation
float calculateAttenuationEpic(vec3 lightPos, vec3 fragPos, float lightRadius)
{
    float distance = length(lightPos - fragPos);
    float falloff = SQUARE(SATURATE(1.0 - SQUARE(SQUARE(distance / lightRadius))));
    return falloff / (SQUARE(distance) + 1.0);
}

// =============================================================================
// SPOT LIGHT FUNCTIONS
// =============================================================================

// Calculate spot light intensity with smooth falloff
float calculateSpotIntensity(vec3 lightDir, vec3 spotDir, vec4 spotParams)
{
    float innerCutoff = spotParams.x;
    float outerCutoff = spotParams.y;
    
    float theta = dot(lightDir, normalize(-spotDir));
    float epsilon = innerCutoff - outerCutoff;
    float intensity = SATURATE((theta - outerCutoff) / epsilon);
    
    // Smooth falloff function
    return intensity * intensity;
}

// Advanced spot light with custom falloff curve
float calculateSpotIntensityAdvanced(vec3 lightDir, vec3 spotDir, vec4 spotParams)
{
    float innerCutoff = spotParams.x;
    float outerCutoff = spotParams.y;
    float falloffExponent = spotParams.z;
    
    float theta = dot(lightDir, normalize(-spotDir));
    
    if (theta > innerCutoff)
        return 1.0;
    else if (theta > outerCutoff)
    {
        float t = (theta - outerCutoff) / (innerCutoff - outerCutoff);
        return pow(t, falloffExponent);
    }
    
    return 0.0;
}

// =============================================================================
// AREA LIGHT FUNCTIONS
// =============================================================================

// Representative point technique for area lights
vec3 calculateAreaLightContribution(vec3 N, vec3 V, vec3 lightPos, vec3 lightSize, 
                                   vec3 albedo, float metallic, float roughness, vec3 worldPos)
{
    vec3 centerToRay = dot(lightPos - worldPos, N) * N - (lightPos - worldPos);
    vec3 closestPoint = lightPos + centerToRay * SATURATE(length(centerToRay) / lightSize.x);
    
    vec3 L = normalize(closestPoint - worldPos);
    float distance = length(closestPoint - worldPos);
    
    // Use standard BRDF calculation
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
    
    // Area light attenuation
    float attenuation = 1.0 / (distance * distance + 1.0);
    
    return brdf * attenuation;
}

// =============================================================================
// MULTI-LIGHT CALCULATION
// =============================================================================

// Calculate contribution from a single light
vec3 calculateLightContribution(LightData light, vec3 N, vec3 V, vec3 albedo, 
                               float metallic, float roughness, vec3 worldPos)
{
    int lightType = int(light.position.w);
    vec3 lightColor = light.color.rgb;
    float lightIntensity = light.color.w;
    
    vec3 L;
    float attenuation = 1.0;
    
    // Calculate light direction and attenuation based on type
    if (lightType == DIRECTIONAL_LIGHT)
    {
        L = normalize(-light.direction.xyz);
        // No attenuation for directional lights
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
    
    // Calculate BRDF
    vec3 radiance = lightColor * lightIntensity * attenuation;
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);
    
    return brdf * radiance * NdotL;
}

// Enhanced light contribution with better energy conservation
vec3 calculateLightContributionEnhanced(LightData light, vec3 N, vec3 V, vec3 albedo, 
                                       float metallic, float roughness, vec3 worldPos)
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
        attenuation = calculateAttenuationEpic(light.position.xyz, worldPos, light.attenuationParams.w);
    }
    else if (lightType == SPOT_LIGHT)
    {
        L = normalize(light.position.xyz - worldPos);
        attenuation = calculateAttenuationEpic(light.position.xyz, worldPos, light.attenuationParams.w);
        float spotIntensity = calculateSpotIntensityAdvanced(L, light.direction.xyz, light.spotParams);
        attenuation *= spotIntensity;
    }
    else
    {
        return vec3(0.0);
    }
    
    if (attenuation <= EPSILON) return vec3(0.0);
    
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= EPSILON) return vec3(0.0);
    
    // Use enhanced BRDF with height-correlated Smith G function
    vec3 radiance = lightColor * lightIntensity * attenuation;
    vec3 brdf = cookTorranceBRDFEnhanced(N, V, L, albedo, metallic, roughness);
    
    return brdf * radiance * NdotL;
}

// =============================================================================
// SHADOW FUNCTIONS (placeholder for future implementation)
// =============================================================================

// Calculate shadow factor (placeholder)
float calculateShadowFactor(vec3 worldPos, mat4 lightSpaceMatrix, sampler2D shadowMap)
{
    // TODO: Implement shadow mapping
    return 1.0; // No shadows for now
}

// Calculate cascaded shadow factor for directional lights
float calculateCascadedShadowFactor(vec3 worldPos, vec3 viewPos)
{
    // TODO: Implement cascaded shadow mapping
    return 1.0; // No shadows for now
}

// =============================================================================
// IBL LIGHTING FUNCTIONS
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

// Enhanced IBL with importance sampling (for real-time global illumination)
vec3 calculateIBLImportanceSampled(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                                  samplerCube environmentMap, int sampleCount)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);
    
    vec3 color = vec3(0.0);
    float totalWeight = 0.0;
    
    // Sample environment using importance sampling
    for (int i = 0; i < sampleCount; ++i)
    {
        vec2 Xi = hammersleySequence(uint(i), uint(sampleCount));
        vec3 H = importanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            vec3 sampleColor = texture(environmentMap, L).rgb;
            
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);
            
            float D = distributionGGX(N, H, roughness);
            float G = geometrySmith(N, V, L, roughness);
            vec3 F = fresnelSchlick(VdotH, F0);
            
            vec3 numerator = D * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + EPSILON;
            vec3 specular = numerator / denominator;
            
            color += sampleColor * specular * NdotL;
            totalWeight += NdotL;
        }
    }
    
    return color / max(totalWeight, EPSILON);
}

#endif // PBR_LIGHTING_GLSL
