// =============================================================================
// IBL.glsl - Image Based Lighting Functions
// Part of OloEngine PBR System
// =============================================================================

#ifndef IBL_INCLUDED
#define IBL_INCLUDED

#include "BRDF.glsl"

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

#endif // IBL_INCLUDED
