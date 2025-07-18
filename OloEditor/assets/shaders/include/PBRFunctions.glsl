// =============================================================================
// PBRFunctions.glsl - Common PBR functions and utilities
// Part of OloEngine Enhanced PBR System
// This file contains reusable PBR functions for BRDF calculations
// =============================================================================

#ifndef PBR_FUNCTIONS_GLSL
#define PBR_FUNCTIONS_GLSL

// Include constants
#include "PBRConstants.glsl"

// =============================================================================
// FRESNEL FUNCTIONS
// =============================================================================

// Schlick-Fresnel approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(SATURATE(1.0 - cosTheta), 5.0);
}

// Fresnel with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(SATURATE(1.0 - cosTheta), 5.0);
}

// More accurate Fresnel using Spherical Gaussian approximation
vec3 fresnelSphericalGaussian(float cosTheta, vec3 F0)
{
    return F0 + (vec3(1.0) - F0) * pow(2.0, (-5.55473 * cosTheta - 6.98316) * cosTheta);
}

// =============================================================================
// DISTRIBUTION FUNCTIONS
// =============================================================================

// GGX/Trowbridge-Reitz normal distribution function
float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / max(denom, EPSILON);
}

// Anisotropic GGX distribution
float distributionGGXAnisotropic(vec3 N, vec3 H, vec3 T, vec3 B, float roughnessX, float roughnessY)
{
    float TdotH = dot(T, H);
    float BdotH = dot(B, H);
    float NdotH = dot(N, H);
    
    float a2 = roughnessX * roughnessY;
    vec3 v = vec3(roughnessY * TdotH, roughnessX * BdotH, a2 * NdotH);
    float v2 = dot(v, v);
    float w2 = a2 / v2;
    
    return a2 * w2 * w2 * INV_PI;
}

// =============================================================================
// GEOMETRY FUNCTIONS
// =============================================================================

// Smith's method for masking-shadowing function (single direction)
float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / max(denom, EPSILON);
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

// Height-correlated Smith G function (more accurate)
float geometrySmithHeightCorrelated(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    
    float a2 = roughness * roughness;
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    
    return 0.5 / max(GGXV + GGXL, EPSILON);
}

// =============================================================================
// BRDF CALCULATIONS
// =============================================================================

// Cook-Torrance BRDF implementation
vec3 cookTorranceBRDF(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);
    
    // Calculate F0 based on metallic workflow
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);
    
    // Calculate the three components of the BRDF
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    
    // Calculate specular BRDF
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + EPSILON;
    vec3 specular = numerator / denominator;
    
    // Calculate diffuse contribution
    vec3 kS = F; // Energy of light that gets reflected
    vec3 kD = vec3(1.0) - kS; // Remaining energy for refraction
    kD *= 1.0 - metallic; // Metallic materials don't refract light
    
    return kD * albedo * INV_PI + specular;
}

// Enhanced BRDF with height-correlated Smith G function
vec3 cookTorranceBRDFEnhanced(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);
    
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);
    
    float NDF = distributionGGX(N, H, roughness);
    float G = geometrySmithHeightCorrelated(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + EPSILON;
    vec3 specular = numerator / denominator;
    
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;
    
    return kD * albedo * INV_PI + specular;
}

// =============================================================================
// NORMAL MAPPING UTILITIES
// =============================================================================

// Get normal from normal map using derivative method
vec3 getNormalFromMap(sampler2D normalMap, vec2 texCoords, vec3 worldPos, vec3 normal, float normalScale)
{
    vec3 tangentNormal = texture(normalMap, texCoords).xyz * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    
    vec3 Q1 = dFdx(worldPos);
    vec3 Q2 = dFdy(worldPos);
    vec2 st1 = dFdx(texCoords);
    vec2 st2 = dFdy(texCoords);
    
    vec3 N = normalize(normal);
    vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);
    vec3 B = -normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);
    
    return normalize(TBN * tangentNormal);
}

// =============================================================================
// COLOR UTILITIES
// =============================================================================

// Linear to sRGB conversion
vec3 linearToSRGB(vec3 color)
{
    return pow(color, vec3(INV_GAMMA));
}

// sRGB to linear conversion
vec3 sRGBToLinear(vec3 color)
{
    return pow(color, vec3(GAMMA));
}

// ACES tone mapping
vec3 acesToneMapping(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    
    return SATURATE((color * (a * color + b)) / (color * (c * color + d) + e));
}

// Reinhard tone mapping
vec3 reinhardToneMapping(vec3 color)
{
    return color / (color + vec3(1.0));
}

// Uncharted 2 tone mapping
vec3 uncharted2ToneMapping(vec3 color)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    
    return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

// =============================================================================
// SAMPLING UTILITIES
// =============================================================================

// Van der Corput sequence for low-discrepancy sampling
float vanDerCorputSequence(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

// Hammersley sequence for importance sampling
vec2 hammersleySequence(uint i, uint N)
{
    return vec2(float(i) / float(N), vanDerCorputSequence(i));
}

// Importance sample GGX distribution
vec3 importanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness * roughness;
    
    float phi = TWO_PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    
    // From spherical coordinates to cartesian coordinates
    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;
    
    // From tangent-space vector to world-space sample vector
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    
    vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

#endif // PBR_FUNCTIONS_GLSL
