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
#define MAX_LIGHTS 32
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
    vec3 worldPos,
    float viewDepth,
    mat4 lightSpaceMatrices[4],
    vec4 cascadePlaneDistances,
    vec4 shadowParams,
    int shadowMapResolution)
{
    float maxShadowDistance = shadowParams.w;
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

    float shadow = sampleShadowPCF(shadowMap, projCoords, float(cascadeIndex), cascadeBias, shadowMapResolution);

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
            float nextShadow = sampleShadowPCF(shadowMap, nextProjCoords, float(cascadeIndex + 1), nextBias, shadowMapResolution);
            shadow = mix(shadow, nextShadow, blendFactor);
        }
    }

    // Distance fade: smoothly transition to no shadow at max shadow distance
    float fadeStart = maxShadowDistance * 0.8;
    float distanceFade = 1.0 - smoothstep(fadeStart, maxShadowDistance, -viewDepth);
    shadow = mix(1.0, shadow, distanceFade);

    return shadow;
}

// Simple shadow factor for a single light (spot light)
float calculateShadowFactor(vec3 worldPos, mat4 lightSpaceMatrix, sampler2DArrayShadow shadowMap, float layer, float bias, int resolution)
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

    return sampleShadowPCF(shadowMap, projCoords, layer, bias, resolution);
}

// Point light shadow factor using depth cubemap
// Uses samplerCubeShadow with hardware depth comparison (GL_LEQUAL)
// The cubemap stores linear depth ( distance / farPlane ) written by ShadowDepthPoint.glsl
float calculatePointShadowFactor(samplerCubeShadow shadowCubemap, vec3 worldPos, vec3 lightPos, float farPlane, float bias)
{
    vec3 fragToLight = worldPos - lightPos;
    float currentDepth = length(fragToLight) / farPlane;

    // samplerCubeShadow: texture(sampler, vec4(direction.xyz, refDepth))
    // Returns 1.0 if refDepth <= stored depth (not in shadow), 0.0 otherwise
    float shadow = texture(shadowCubemap, vec4(fragToLight, currentDepth - bias));
    return shadow;
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
