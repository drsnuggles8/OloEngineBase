// =============================================================================
// PBRCommon.glsl - Complete PBR shader include file
// Part of OloEngine Enhanced PBR System
// This file combines all PBR constants, functions, and lighting calculations
// =============================================================================

#ifndef PBR_GLSL
#define PBR_GLSL

// =============================================================================
// LIGHT TYPE CONSTANTS
// =============================================================================
#define DIRECTIONAL_LIGHT 0
#define POINT_LIGHT 1
#define SPOT_LIGHT 2
// Sphere area lights reuse the MultiLightData layout: SpotParams.z encodes the
// emitter sphere radius and the standard Position/Range fields drive distance
// falloff. Specular is shaded via the Karis 2013 representative-point trick;
// diffuse uses a solid-angle correction that collapses to a point light as
// the radius approaches zero.
#define SPHERE_AREA_LIGHT 3

// =============================================================================
// MATHEMATICAL CONSTANTS
// =============================================================================
#define PI 3.14159265359
#define TWO_PI 6.28318530718
#define HALF_PI 1.57079632679
#define INV_PI 0.31830988618
#define INV_TWO_PI 0.15915494309
#define EPSILON 0.0001
#define LARGE_EPSILON 0.001

// =============================================================================
// PBR MATERIAL CONSTANTS
// =============================================================================
#define DEFAULT_DIELECTRIC_F0 0.04
#define MAX_REFLECTION_LOD 4.0
#define MIN_ROUGHNESS 0.04  // Minimum roughness to avoid numerical issues
#define MAX_ROUGHNESS 1.0

// =============================================================================
// RENDERING CONSTANTS
// =============================================================================
#define GAMMA 2.2
#define INV_GAMMA 0.45454545455
#define MAX_LIGHTS 256
#define MAX_BONES 100

// Tone mapping constants
#define TONEMAP_NONE 0
#define TONEMAP_REINHARD 1
#define TONEMAP_ACES 2
#define TONEMAP_UNCHARTED2 3

// =============================================================================
// TEXTURE BINDING CONSTANTS (from ShaderBindingLayout.h)
// =============================================================================
#define TEX_DIFFUSE 0
#define TEX_SPECULAR 1
#define TEX_NORMAL 2
#define TEX_HEIGHT 3
#define TEX_AMBIENT 4
#define TEX_EMISSIVE 5
#define TEX_ENVIRONMENT 9
#define TEX_USER_0 10  // Irradiance map
#define TEX_USER_1 11  // Prefilter map
#define TEX_USER_2 12  // BRDF LUT

// =============================================================================
// UNIFORM BUFFER BINDING CONSTANTS
// =============================================================================
#define UBO_CAMERA 0
#define UBO_LIGHTS 1
#define UBO_MATERIAL 2
#define UBO_MODEL 3
#define UBO_BONES 4
#define UBO_MULTI_LIGHTS 5

// =============================================================================
// QUALITY SETTINGS
// =============================================================================
#define IBL_SAMPLE_COUNT_LOW 256
#define IBL_SAMPLE_COUNT_MEDIUM 512
#define IBL_SAMPLE_COUNT_HIGH 1024
#define IBL_SAMPLE_COUNT_ULTRA 2048

// =============================================================================
// COLOR SPACE CONVERSION
// =============================================================================
#define SRGB_TO_LINEAR(color) pow(color, vec3(GAMMA))
#define LINEAR_TO_SRGB(color) pow(color, vec3(INV_GAMMA))

// =============================================================================
// UTILITY MACROS
// =============================================================================
#define SATURATE(x) clamp(x, 0.0, 1.0)
#define SQUARE(x) ((x) * (x))
#define MAX3(a, b, c) max(a, max(b, c))
#define MIN3(a, b, c) min(a, min(b, c))

// Shared Pow2/4/5, bitwise RadicalInverse_VdC, Hammersley, branchless
// OrthonormalBasis and ImportanceSampleGGX. PI is already defined above, so
// MathCommon's #ifndef guard leaves it alone. Bare path: nested includes
// resolve relative to this file's own directory (assets/shaders/include),
// matching LightProbeSampling.glsl's `#include "SphericalHarmonics.glsl"`.
#include "MathCommon.glsl"

// =============================================================================
// FRESNEL FUNCTIONS
// =============================================================================

// Schlick-Fresnel approximation. Pow5 (multiply chain) over pow(x, 5.0): this
// runs for every light at every lit pixel, so the avoided exp2/log2 pair is a
// real per-frame win, and the result is bit-near-identical for x in [0, 1].
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * Pow5(SATURATE(1.0 - cosTheta));
}

// Fresnel with roughness for IBL
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * Pow5(SATURATE(1.0 - cosTheta));
}

// (A spherical-Gaussian Fresnel approximation lived here. It was never wired up,
//  and now that Schlick uses the Pow5 multiply chain it would be slower — its
//  exp2 costs more than five MULs — so it was removed rather than kept as dead
//  code. See issue #262 — shader performance review.)

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
    // Reconstruct z from xy rather than sampling the blue channel. A tangent-space
    // unit normal always has z = sqrt(1 - x^2 - y^2), so this is correct for ordinary
    // 3-channel (RGB) normal maps AND required for 2-channel BC5/RGTC2 normal maps,
    // whose blue channel is 0 on the GPU (sampling it would give z = -1 and invert the
    // normal). Reconstruction is done after the xy intensity scale so the result stays
    // unit length. (#440)
    vec2 nxy = texture(normalMap, texCoords).xy * 2.0 - 1.0;
    nxy *= normalScale;
    float nz = sqrt(max(0.0, 1.0 - min(1.0, dot(nxy, nxy))));
    vec3 tangentNormal = vec3(nxy, nz);

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

// Linear to sRGB conversion (accurate)
vec3 linearToSRGB(vec3 color)
{
    return pow(color, vec3(INV_GAMMA));
}

// sRGB to linear conversion (accurate)
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

// Unified post-processing function - tone mapping + gamma correction in one pass
// This prevents redundant gamma correction by combining operations
vec3 postProcessColor(vec3 hdrColor, int tonemapOperator, bool applyGamma)
{
    vec3 toneMappedColor;

    // Apply tone mapping
    switch (tonemapOperator)
    {
        case TONEMAP_REINHARD:
            toneMappedColor = reinhardToneMapping(hdrColor);
            break;
        case TONEMAP_ACES:
            toneMappedColor = acesToneMapping(hdrColor);
            break;
        case TONEMAP_UNCHARTED2:
            toneMappedColor = uncharted2ToneMapping(hdrColor);
            break;
        default: // TONEMAP_NONE
            toneMappedColor = SATURATE(hdrColor);
            break;
    }

    // Apply gamma correction only if requested (prevents double application)
    if (applyGamma) {
        return linearToSRGB(toneMappedColor);
    }

    return toneMappedColor;
}

// =============================================================================
// SAMPLING UTILITIES
// =============================================================================

// camelCase aliases over the shared MathCommon primitives, kept so existing
// call sites (e.g. calculateIBLImportanceSampled) compile unchanged. The
// canonical bodies — bitwise radical inverse and the branchless orthonormal
// basis — now live in MathCommon.glsl, so they can't drift from the bake path.
float vanDerCorputSequence(uint bits)               { return RadicalInverse_VdC(bits); }
vec2  hammersleySequence(uint i, uint N)            { return Hammersley(i, N); }
vec3  importanceSampleGGX(vec2 Xi, vec3 N, float r) { return ImportanceSampleGGX(Xi, N, r); }

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
    float falloff = SATURATE(1.0 - Pow4(distance / range));
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
// SPHERE AREA LIGHT (Karis 2013 representative point)
// =============================================================================

// Compute the representative light direction for a sphere area light's specular
// term. Conceptually: find the point on the emitter sphere that the surface's
// reflection ray would hit, then shade as if the light were that point.
// fragPos     — surface position (world space)
// N           — surface normal (world space)
// V           — view vector (world space, from surface to camera)
// lightPos    — sphere center (world space)
// sphereRadius — physical emitter radius
//
// Reference: Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013, eq. 12.
vec3 calculateSphereAreaLightRepresentativePoint(vec3 fragPos, vec3 N, vec3 V,
                                                  vec3 lightPos, float sphereRadius)
{
    vec3 r = reflect(-V, N);
    vec3 L = lightPos - fragPos;
    vec3 centerToRay = dot(L, r) * r - L;
    vec3 closestPoint = L + centerToRay * clamp(sphereRadius / max(length(centerToRay), EPSILON), 0.0, 1.0);
    return normalize(closestPoint);
}

// Energy-conservation rescale for the GGX normalization when the light has
// a physical radius. Without this, larger radii produce brighter highlights.
// (Karis 2013, eq. 14 — derived from the analytic solid angle of a sphere.)
float sphereAreaLightNormalization(float roughness, float distance, float sphereRadius)
{
    float alpha = roughness * roughness;
    float alphaPrime = clamp(alpha + sphereRadius / max(2.0 * distance, EPSILON), 0.0, 1.0);
    // Squared ratio so the BRDF integrates to 1 as radius -> 0 (point light).
    float ratio = alpha / max(alphaPrime, EPSILON);
    return ratio * ratio;
}

// Evaluate a sphere area light at the surface.
// Returns the radiance contribution (radiance * NdotL * BRDF).
vec3 calculateSphereAreaLightContribution(vec3 N, vec3 V, vec3 lightPos, float sphereRadius,
                                           vec3 lightColor, float lightIntensity, float range,
                                           vec3 albedo, float metallic, float roughness, vec3 worldPos)
{
    vec3 toLight = lightPos - worldPos;
    float distance = length(toLight);

    // Early-out: outside range. Range is measured from the centre, matching
    // the way light culling treats the bounding sphere.
    if (distance > range) return vec3(0.0);

    // Standard L for diffuse — use the light centre, not the representative
    // point (the diffuse term integrates over the full hemisphere already).
    vec3 Ldiff = toLight / max(distance, EPSILON);
    float NdotL = max(dot(N, Ldiff), 0.0);
    if (NdotL <= EPSILON) return vec3(0.0);

    // Smooth distance attenuation matching the Forward+ point-light falloff.
    float distRatio = distance / max(range, EPSILON);
    float distAtten = max(1.0 - distRatio * distRatio, 0.0);
    distAtten = distAtten * distAtten / (distance * distance + 1.0);

    // Representative point for specular. Closer-to-zero radius converges to
    // the centre direction, recovering point-light behaviour.
    vec3 Lspec = calculateSphereAreaLightRepresentativePoint(worldPos, N, V, lightPos, sphereRadius);

    // Split BRDF: diffuse uses Ldiff (centre), specular uses Lspec (rep point).
    // We compute diffuse + specular separately to avoid double-counting fresnel
    // off the wrong half-vector.
    vec3 H = normalize(V + Lspec);
    float NdotV = max(dot(N, V), 0.0);
    float NdotLspec = max(dot(N, Lspec), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    vec3 F0 = mix(vec3(DEFAULT_DIELECTRIC_F0), albedo, metallic);
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, Lspec, roughness);
    vec3  F = fresnelSchlick(VdotH, F0);

    // Energy-conservation rescale (Karis eq. 14).
    float normFactor = sphereAreaLightNormalization(roughness, distance, sphereRadius);
    D *= normFactor;

    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotLspec, EPSILON);

    // Diffuse uses the centre direction; energy lost to specular is removed.
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 diffuse = kD * albedo * INV_PI;

    vec3 radiance = lightColor * lightIntensity * distAtten;
    // Diffuse term scales by NdotL (Lambertian); specular by NdotLspec.
    return (diffuse * NdotL + specular * NdotLspec) * radiance;
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

    // Sphere area lights take a dedicated evaluator — the representative-point
    // trick splits the BRDF differently, so we cannot route it through the
    // common L + cookTorranceBRDF path below.
    if (lightType == SPHERE_AREA_LIGHT)
    {
        float sphereRadius = light.spotParams.z;       // Packed by Scene::ProcessScene3DSharedLogic
        float range        = light.attenuationParams.w;
        return calculateSphereAreaLightContribution(N, V, light.position.xyz, sphereRadius,
                                                    lightColor, lightIntensity, range,
                                                    albedo, metallic, roughness, worldPos);
    }

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

    // Sphere area lights split the BRDF differently; use the dedicated evaluator.
    if (lightType == SPHERE_AREA_LIGHT)
    {
        float sphereRadius = light.spotParams.z;
        float range        = light.attenuationParams.w;
        return calculateSphereAreaLightContribution(N, V, light.position.xyz, sphereRadius,
                                                    lightColor, lightIntensity, range,
                                                    albedo, metallic, roughness, worldPos);
    }

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
// SHADOW FUNCTIONS
// =============================================================================

// PCF (Percentage Closer Filtering) for soft shadow edges
float sampleShadowPCF(sampler2DArrayShadow shadowMap, vec3 projCoords, float layer, float bias, int resolution)
{
    float shadow = 0.0;
    float texelSize = 1.0 / float(resolution);

    // 3x3 PCF kernel
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            // sampler2DArrayShadow: texture(sampler, vec4(uv.x, uv.y, layer, compareRef))
            shadow += texture(shadowMap, vec4(projCoords.xy + offset, layer, projCoords.z - bias));
        }
    }
    return shadow / 9.0;
}

// =============================================================================
// PCSS — Percentage-Closer Soft Shadows (contact-hardening variable penumbra)
// =============================================================================
// Sharp where an occluder meets the receiver, softening with separation. Two
// stages: (1) a blocker search over the RAW depth array (a comparison-OFF view
// bound alongside the hardware-comparison array — the comparison sampler can't
// return raw occluder depth) to find the average occluder depth, then (2) a
// variable-radius PCF whose radius is the estimated penumbra. Gated by
// u_SoftShadowMode (passed in as softMode) so the legacy fixed PCF stays the
// default fallback.

// Shared 16-tap Poisson disk (unit disk) for both the blocker search and PCF.
const vec2 POISSON_DISK_16[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);

// Per-fragment rotation angle to decorrelate the Poisson pattern (cheap hash of
// world position — turns sampling banding into noise the eye tolerates better).
float pcssRotationAngle(vec3 worldPos)
{
    float h = fract(sin(dot(worldPos.xy + worldPos.yz, vec2(12.9898, 78.233))) * 43758.5453);
    return h * 6.2831853; // 0..2pi
}

// Blocker search on the raw (comparison-OFF) array. Returns the average blocker
// depth in shadow-map NDC depth space, or -1.0 if no blocker is found.
// searchRadiusUV / depths are in shadow-map UV / NDC units.
float pcssBlockerSearch(sampler2DArray rawMap, vec2 uv, float layer, float zReceiver,
                        float searchRadiusUV, mat2 rot, out int numBlockers)
{
    float blockerSum = 0.0;
    numBlockers = 0;
    for (int i = 0; i < 16; ++i)
    {
        vec2 offs = (rot * POISSON_DISK_16[i]) * searchRadiusUV;
        float d = texture(rawMap, vec3(uv + offs, layer)).r;
        if (d < zReceiver) // closer to the light than the receiver -> occluder
        {
            blockerSum += d;
            numBlockers += 1;
        }
    }
    return (numBlockers == 0) ? -1.0 : (blockerSum / float(numBlockers));
}

// Variable-radius PCF using the hardware comparison sampler (free 2x2 bilinear
// PCF per tap). filterRadiusUV in shadow-map UV units.
float pcssVariablePCF(sampler2DArrayShadow shadowMap, vec2 uv, float layer, float zReceiver,
                      float bias, float filterRadiusUV, mat2 rot)
{
    float sum = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        vec2 offs = (rot * POISSON_DISK_16[i]) * filterRadiusUV;
        sum += texture(shadowMap, vec4(uv + offs, layer, zReceiver - bias));
    }
    return sum / 16.0;
}

// Full PCSS visibility for one shadow-map layer. `softness` is the light's
// apparent size knob (ShadowParams.z); larger -> softer. Returns [0,1] (1 lit).
float pcssShadowFactor(sampler2DArrayShadow shadowMap, sampler2DArray rawMap,
                       vec2 uv, float layer, float zReceiver, float bias,
                       float softness, int resolution, mat2 rot)
{
    float texelSizeUV = 1.0 / float(resolution);

    // Light apparent size in texels. Softness == 1 -> ~4-texel light.
    float lightSizeTexels = max(softness, 0.05) * 4.0;
    float searchRadiusUV = max(lightSizeTexels, 2.0) * texelSizeUV;

    int numBlockers;
    float avgBlocker = pcssBlockerSearch(rawMap, uv, layer, zReceiver, searchRadiusUV, rot, numBlockers);
    if (numBlockers == 0)
        return 1.0; // no occluder -> fully lit
    if (numBlockers == 16)
        return 0.0; // every search tap occluded -> deep umbra, skip the filter

    // Penumbra from occluder/receiver depth gap (contact-hardening). The gain
    // absorbs the cascade depth scale; tuned for a natural soft edge. Clamp to
    // [1 texel, lightSizeTexels*4] to bound cost and avoid over-blurring —
    // wide scattered Poisson taps thrash the texture cache, and this radius
    // cap (down from *8) measured as one of the biggest PCSS costs on Sponza.
    const float PCSS_PENUMBRA_GAIN = 220.0;
    float depthGap = max(zReceiver - avgBlocker, 0.0);
    float filterRadiusTexels = clamp(depthGap * lightSizeTexels * PCSS_PENUMBRA_GAIN,
                                     1.0, lightSizeTexels * 4.0);

    // Contact-hardened fragments (radius clamped to the 1-texel floor) collapse
    // to a single hardware-PCF tap — its 2x2 bilinear footprint already covers
    // a 1-texel radius, so the 16-tap Poisson disk adds cost but no quality.
    if (filterRadiusTexels <= 1.0)
        return texture(shadowMap, vec4(uv, layer, zReceiver - bias));

    float filterRadiusUV = filterRadiusTexels * texelSizeUV;

    return pcssVariablePCF(shadowMap, uv, layer, zReceiver, bias, filterRadiusUV, rot);
}

// Dispatch one layer's shadow test to PCSS (softMode==1) or the legacy 3x3 PCF.
float sampleShadowLayer(sampler2DArrayShadow shadowMap, sampler2DArray rawMap,
                        vec3 projCoords, float layer, float bias, int resolution,
                        int softMode, float softness, mat2 rot)
{
    if (softMode == 1)
        return pcssShadowFactor(shadowMap, rawMap, projCoords.xy, layer, projCoords.z, bias, softness, resolution, rot);
    return sampleShadowPCF(shadowMap, projCoords, layer, bias, resolution);
}

// Calculate CSM shadow factor for directional lights
// shadowMap: sampler2DArrayShadow bound at TEX_SHADOW (binding 8)
// worldPos: fragment world position
// viewPos: fragment position in view space (needed for cascade selection)
// lightSpaceMatrices[4]: per-cascade light VP matrices
// cascadePlaneDistances: view-space far distances for each cascade
// shadowParams: x=bias, y=normalBias, z=softness, w=maxShadowDistance
// shadowMapResolution: shadow map size in pixels
float calculateCascadedShadowFactorCSM(
    sampler2DArrayShadow shadowMap,
    sampler2DArray rawShadowMap,
    vec3 worldPos,
    float viewDepth,
    mat4 lightSpaceMatrices[4],
    vec4 cascadePlaneDistances,
    vec4 shadowParams,
    int shadowMapResolution,
    int softMode)
{
    float maxShadowDistance = shadowParams.w;

    // PCSS sampling state (ignored by the legacy PCF path). softness == light
    // apparent size (ShadowParams.z); rot decorrelates the Poisson kernel.
    float softness = shadowParams.z;
    float rotAngle = pcssRotationAngle(worldPos);
    mat2 shadowRot = mat2(cos(rotAngle), -sin(rotAngle), sin(rotAngle), cos(rotAngle));
    if (-viewDepth > maxShadowDistance)
    {
        return 1.0; // Beyond shadow distance
    }

    // Select cascade based on view-space depth
    int cascadeIndex = 3; // Default to last cascade
    float cascadeDists[4] = float[4](
        cascadePlaneDistances.x,
        cascadePlaneDistances.y,
        cascadePlaneDistances.z,
        cascadePlaneDistances.w
    );

    for (int i = 0; i < 4; ++i)
    {
        if (-viewDepth < cascadeDists[i])
        {
            cascadeIndex = i;
            break;
        }
    }

    // Transform to light space
    vec4 lightSpacePos = lightSpaceMatrices[cascadeIndex] * vec4(worldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords = projCoords * 0.5 + 0.5; // NDC [-1,1] -> [0,1]

    // Out of shadow map bounds
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
    {
        return 1.0;
    }

    // Scale bias by cascade (farther cascades need more bias)
    float baseBias = shadowParams.x;
    float cascadeBias = baseBias * float(cascadeIndex + 1);

    // PCSS only for the two nearest cascades: distant fragments cover too few
    // shadow-map texels for contact hardening to read, while the blocker
    // search + wide Poisson PCF stay just as expensive. Cascades 2-3 fall back
    // to the legacy 3x3 hardware PCF (the cascade cross-fade below blends the
    // transition).
    int layerSoftMode = (cascadeIndex < 2) ? softMode : 0;

    float shadow = sampleShadowLayer(shadowMap, rawShadowMap, projCoords, float(cascadeIndex),
                                     cascadeBias, shadowMapResolution, layerSoftMode, softness, shadowRot);

    // Cascade blending: smooth cross-fade in the last 10% of each cascade
    if (cascadeIndex < 3)
    {
        float cascadeFar = cascadeDists[cascadeIndex];
        float blendZone = cascadeFar * 0.1;
        float blendFactor = smoothstep(cascadeFar - blendZone, cascadeFar, -viewDepth);

        if (blendFactor > 0.0)
        {
            // Sample next cascade
            vec4 nextLightSpacePos = lightSpaceMatrices[cascadeIndex + 1] * vec4(worldPos, 1.0);
            vec3 nextProjCoords = nextLightSpacePos.xyz / nextLightSpacePos.w;
            nextProjCoords = nextProjCoords * 0.5 + 0.5;
            float nextBias = baseBias * float(cascadeIndex + 2);
            int nextSoftMode = (cascadeIndex + 1 < 2) ? softMode : 0;
            float nextShadow = sampleShadowLayer(shadowMap, rawShadowMap, nextProjCoords, float(cascadeIndex + 1),
                                                 nextBias, shadowMapResolution, nextSoftMode, softness, shadowRot);
            shadow = mix(shadow, nextShadow, blendFactor);
        }
    }

    // Distance fade: smoothly transition to no shadow at max shadow distance.
    // The fade band is (1 - fadeStartFraction) of MaxShadowDistance — at 0.7
    // that's the last 30% of the range, wide enough that a player walking
    // toward the edge of CSM coverage sees a gradual falloff rather than a
    // hard "pop" appearing within a few meters. Tightening this fraction
    // (e.g. 0.95) compresses the fade into a narrow band and reads as a
    // sudden binary transition; loosening it (e.g. 0.5) starts the fade
    // earlier and shadows visibly thin out at mid-range.
    // TODO(olbu): future enhancements to lift the cascade-reach vs. fade-distance coupling:
    //   - Expose fadeStartFraction as a per-scene/per-light parameter instead
    //     of a shader literal.
    //   - Add a separate ShadowFadeDistance distinct from MaxShadowDistance so
    //     cascade splits don't stretch when you only want a longer fade tail.
    //   - Unreal-style Far Shadow Cascades: a second pool of low-res cascades
    //     for long-range geometry tagged with bCastFarShadow, so we get
    //     "shadows visible to the horizon" without losing near-camera resolution.
    //   - Distance Field Shadows beyond CSM range as a cheap soft-shadow fallback.
    float fadeStart = maxShadowDistance * 0.7;
    float distanceFade = 1.0 - smoothstep(fadeStart, maxShadowDistance, -viewDepth);
    shadow = mix(1.0, shadow, distanceFade);

    return shadow;
}

// Simple shadow factor for a single light (spot light). softMode/softness drive
// PCSS; rawShadowMap is the comparison-OFF view of the spot array.
float calculateShadowFactor(vec3 worldPos, mat4 lightSpaceMatrix, sampler2DArrayShadow shadowMap,
                            sampler2DArray rawShadowMap, float layer, float bias, int resolution,
                            int softMode, float softness)
{
    vec4 lightSpacePos = lightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
    {
        return 1.0;
    }

    float rotAngle = pcssRotationAngle(worldPos);
    mat2 shadowRot = mat2(cos(rotAngle), -sin(rotAngle), sin(rotAngle), cos(rotAngle));
    return sampleShadowLayer(shadowMap, rawShadowMap, projCoords, layer, bias, resolution, softMode, softness, shadowRot);
}

// 20-direction cube sampling offsets for soft point-shadow PCF.
const vec3 POINT_PCF_OFFSETS[20] = vec3[](
    vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1),
    vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
    vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
    vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
    vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
);

// Point light shadow factor using depth cubemap.
// Uses samplerCubeShadow with hardware depth comparison (GL_LEQUAL); the cubemap
// stores linear depth ( distance / farPlane ) written by ShadowDepthPoint.glsl.
// A fixed 20-tap disk PCF (radius grows mildly with distance) replaces the old
// single tap so point shadows read as soft edges rather than aliased hard ones.
float calculatePointShadowFactor(samplerCubeShadow shadowCubemap, vec3 worldPos, vec3 lightPos, float farPlane, float bias)
{
    vec3 fragToLight = worldPos - lightPos;
    float currentDepth = length(fragToLight) / farPlane;

    // World-space sample-disk radius: a small fraction of the light range that
    // widens with distance from the light for a roughly constant screen-space
    // softness. samplerCubeShadow: texture(sampler, vec4(direction.xyz, refDepth)).
    float diskRadius = farPlane * 0.0035 * (1.0 + currentDepth);
    float shadow = 0.0;
    for (int i = 0; i < 20; ++i)
    {
        shadow += texture(shadowCubemap, vec4(fragToLight + POINT_PCF_OFFSETS[i] * diskRadius, currentDepth - bias));
    }
    return shadow / 20.0;
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
// NOTE: Does NOT include AO — caller applies AO uniformly via `ambient * ao`
// to match the IBL / light-probe paths which also omit AO from their returns.
vec3 calculateSimpleAmbient(vec3 albedo, float metallic, float ao)
{
    vec3 ambient = vec3(0.03) * albedo;
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

// =============================================================================
// LIGHT PROBE AMBIENT FUNCTIONS
// =============================================================================

// Calculate ambient lighting from light probe irradiance
// Mirrors calculateIBL energy conservation: kD *= (1.0 - metallic)
vec3 calculateLightProbeAmbient(vec3 probeIrradiance, vec3 albedo, float metallic, float roughness,
                                vec3 N, vec3 V)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);

    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    return kD * probeIrradiance * albedo;
}

// Combine light probe diffuse with IBL specular
// Probes provide diffuse irradiance; IBL prefilter map provides specular reflections
vec3 calculateCombinedAmbient(vec3 probeIrradiance, vec3 N, vec3 V, vec3 albedo,
                              float metallic, float roughness,
                              samplerCube prefilterMap, sampler2D brdfLUT)
{
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);
    F0 = mix(F0, albedo, metallic);

    vec3 F = fresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);

    vec3 kS = F;
    vec3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    // Diffuse from probes
    vec3 diffuse = probeIrradiance * albedo;

    // Specular from IBL prefilter
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 envBRDF = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

    return kD * diffuse + specular;
}

// =============================================================================
// SHADER-SPECIFIC LIGHT CALCULATIONS
// =============================================================================

// Calculate directional light contribution using shader uniform
vec3 calculateDirectionalLightUniform(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                                     vec3 lightDirection, vec3 lightDiffuse)
{
    vec3 L = normalize(-lightDirection);
    vec3 radiance = lightDiffuse;

    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);

    return brdf * radiance * NdotL;
}

// Calculate point light contribution using shader uniform
vec3 calculatePointLightUniform(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                               vec3 worldPos, vec3 lightPosition, vec3 lightDiffuse, vec4 attParams)
{
    vec3 L = normalize(lightPosition - worldPos);
    float distance = length(lightPosition - worldPos);
    float attenuation = 1.0 / (attParams.x + attParams.y * distance + attParams.z * distance * distance);
    vec3 radiance = lightDiffuse * attenuation;

    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);

    return brdf * radiance * NdotL;
}

// Calculate spot light contribution using shader uniform
vec3 calculateSpotLightUniform(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness,
                              vec3 worldPos, vec3 lightPosition, vec3 lightDirection,
                              vec3 lightDiffuse, vec4 attParams, vec4 spotParams)
{
    vec3 L = normalize(lightPosition - worldPos);
    float distance = length(lightPosition - worldPos);
    float attenuation = 1.0 / (attParams.x + attParams.y * distance + attParams.z * distance * distance);

    float theta = dot(L, normalize(-lightDirection));
    float epsilon = spotParams.x - spotParams.y;
    float intensity = clamp((theta - spotParams.y) / epsilon, 0.0, 1.0);

    vec3 radiance = lightDiffuse * attenuation * intensity;

    float NdotL = max(dot(N, L), 0.0);
    vec3 brdf = cookTorranceBRDF(N, V, L, albedo, metallic, roughness);

    return brdf * radiance * NdotL;
}

// =============================================================================
// MATERIAL SAMPLING FUNCTIONS
//
// OPTIMIZATION NOTE: These functions now avoid unnecessary texture lookups
// by checking use flags before sampling. This reduces memory bandwidth usage
// and improves performance when textures are not used.
//
// GAMMA CORRECTION NOTE: Albedo textures should be in sRGB format and will be
// automatically converted to linear space by the GPU. Metallic, roughness,
// normal, and AO maps should already be in linear space.
// =============================================================================

// Sample base color/albedo (assumes sRGB texture, GPU converts to linear)
vec3 sampleAlbedo(sampler2D albedoMap, vec2 texCoord, vec3 baseColorFactor, bool useMap)
{
    if (useMap)
    {
        return baseColorFactor.rgb * texture(albedoMap, texCoord).rgb;
    }
    return baseColorFactor.rgb;
}

// Sample metallic and roughness (linear textures)
vec2 sampleMetallicRoughness(sampler2D metallicRoughnessMap, vec2 texCoord,
                             float metallicFactor, float roughnessFactor, bool useMap)
{
    if (useMap) {
        vec3 metallicRoughness = texture(metallicRoughnessMap, texCoord).rgb;
        return vec2(metallicFactor * metallicRoughness.b,   // Blue channel = metallic
                    roughnessFactor * metallicRoughness.g); // Green channel = roughness
    }
    return vec2(metallicFactor, roughnessFactor);
}

// Sample ambient occlusion (linear texture)
float sampleAO(sampler2D aoMap, vec2 texCoord, float occlusionStrength, bool useMap)
{
    if (useMap) {
        float ao = texture(aoMap, texCoord).r;
        return mix(1.0, ao, occlusionStrength);
    }
    return 1.0;
}

// Sample emissive (assumes sRGB texture if used for color, linear for HDR)
vec3 sampleEmissive(sampler2D emissiveMap, vec2 texCoord, vec3 emissiveFactor, bool useMap)
{
    if (useMap) {
        return emissiveFactor * texture(emissiveMap, texCoord).rgb;
    }
    return emissiveFactor;
}

#endif // PBR_GLSL
