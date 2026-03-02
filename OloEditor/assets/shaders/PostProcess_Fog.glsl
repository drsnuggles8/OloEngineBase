#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

// Output: RGB = accumulated inscatter light, A = transmittance
// Compositing happens in the upsample/composite pass
layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

layout(binding = 19) uniform sampler2D u_DepthTexture;  // Full-res scene depth

// Shadow map for volumetric light shafts (CSM directional)
layout(binding = 8) uniform sampler2DArrayShadow u_ShadowMapCSM;

// Temporal history (previous frame's fog result at half-res)
layout(binding = 3) uniform sampler2D u_FogHistory;

// Camera UBO (binding 0)
layout(std140, binding = 0) uniform CameraMatrices
{
    mat4 u_ViewProjection;
    mat4 u_View;
    mat4 u_Projection;
    vec3 u_CameraPosition;
    float _padding0;
};

// Shadow UBO (binding 6) — cascade matrices for light shaft shadow lookup
layout(std140, binding = 6) uniform ShadowData
{
    mat4 u_DirectionalLightSpaceMatrices[4];
    vec4 u_CascadePlaneDistances;
    vec4 u_ShadowParams;
    mat4 u_SpotLightSpaceMatrices[4];
    vec4 u_PointLightShadowParams[4];
    int u_DirectionalShadowEnabled;
    int u_SpotShadowCount;
    int u_PointShadowCount;
    int u_ShadowMapResolution;
    int u_CascadeDebugEnabled;
    int _shadowPad0;
    int _shadowPad1;
    int _shadowPad2;
};

// Motion blur UBO (binding 8) — inverse VP for depth reconstruction
layout(std140, binding = 8) uniform MotionBlurUBO
{
    mat4 u_InverseViewProjection;
    mat4 u_PrevViewProjection;
};

// ---------------------------------------------------------------------------
// Shadow lookup — single-tap CSM for volumetric light shafts
// ---------------------------------------------------------------------------
float sampleShadowForFog(vec3 worldPos)
{
    if (u_DirectionalShadowEnabled == 0)
        return 1.0;

    vec4 viewPos = u_View * vec4(worldPos, 1.0);
    int cascadeIndex = 3;
    for (int i = 0; i < 4; ++i)
    {
        if (-viewPos.z < u_CascadePlaneDistances[i])
        {
            cascadeIndex = i;
            break;
        }
    }

    vec4 lightSpacePos = u_DirectionalLightSpaceMatrices[cascadeIndex] * vec4(worldPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
        return 1.0;

    float bias = u_ShadowParams.x * float(cascadeIndex + 1) * 2.0;
    return texture(u_ShadowMapCSM, vec4(projCoords.xy, float(cascadeIndex), projCoords.z - bias));
}

// Include fog UBO, noise functions, phase functions
#include "include/FogCommon.glsl"

// Include local fog volume evaluation
#include "include/FogVolumeCommon.glsl"

// ---------------------------------------------------------------------------
// Dual-lobe Henyey-Greenstein phase function — more realistic than single HG.
// Blends forward and back-scatter lobes (Frostbite/UE5 style).
// ---------------------------------------------------------------------------
float dualLobeHGPhase(float cosTheta, float g)
{
    float forwardLobe = henyeyGreensteinPhase(cosTheta, g);
    float backLobe = henyeyGreensteinPhase(cosTheta, -g * 0.3);
    return mix(backLobe, forwardLobe, 0.7);
}

// ---------------------------------------------------------------------------
// Multi-scattering energy approximation (Frostbite 2015).
// Accounts for light that scatters multiple times within the medium.
// ---------------------------------------------------------------------------
vec3 multiScatterApprox(vec3 singleScatter, float density)
{
    // Each successive scatter order is dimmer and more isotropic.
    // Approximate infinite series: S_total ≈ S_1 * (1 + 0.5*d + 0.25*d² + ...)
    float d = clamp(density * 10.0, 0.0, 1.0);
    float multiScatterFactor = 1.0 + d * 0.5 + d * d * 0.25;
    return singleScatter * multiScatterFactor;
}

// ---------------------------------------------------------------------------
// Reconstruct world position from depth
// ---------------------------------------------------------------------------
vec3 worldPosFromDepth(vec2 uv, float depth)
{
    vec4 ndcPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldPos4 = u_InverseViewProjection * ndcPos;
    return worldPos4.xyz / worldPos4.w;
}

// ---------------------------------------------------------------------------
// Temporal reprojection — reproject current pixel to previous frame's UV
// ---------------------------------------------------------------------------
vec2 reprojectUV(vec3 worldPos)
{
    vec4 prevClip = u_PrevViewProjection * vec4(worldPos, 1.0);
    vec2 prevNDC = prevClip.xy / prevClip.w;
    return prevNDC * 0.5 + 0.5;
}

// ---------------------------------------------------------------------------
// Main volumetric ray-march
// ---------------------------------------------------------------------------
void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;

    // Early out for disabled fog
    if (u_FogFlags.x < 0.5)
    {
        o_Color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 worldPos = worldPosFromDepth(v_TexCoord, depth);
    vec3 cameraPos = u_CameraPosition;

    // Unpack fog parameters
    float baseDensity    = u_FogColorAndDensity.a;
    float heightFalloff  = u_FogDistanceParams.z;
    float heightOffset   = u_FogDistanceParams.w;
    float maxOpacity     = u_FogRayleighColorAndMaxOpacity.a;
    vec3  fogColor       = u_FogColorAndDensity.rgb;
    vec3  sunDir         = normalize(u_FogSunDirection.xyz);
    float absorptionCoeff = u_FogVolumetricParams.y;

    float noiseScale     = u_FogNoiseParams.x;
    float noiseSpeed     = u_FogNoiseParams.y;
    float noiseIntensity = u_FogNoiseParams.z;
    float time           = u_FogNoiseParams.w;
    bool  noiseEnabled   = noiseIntensity > 0.001;

    int   numSamples     = clamp(int(u_FogVolumetricParams.x + 0.5), 4, 128);
    float lightShaftInt  = u_FogVolumetricParams.z;
    bool  lightShafts    = u_FogVolumetricParams.w > 0.5;
    bool  scatterEnabled = u_FogFlags.z > 0.5;
    bool  volumetric     = u_FogFlags.w > 0.5;

    // Determine mode: volumetric or analytical
    if (!volumetric)
    {
        // Analytical fog — simple per-pixel (no ray-march)
        vec3 analyticFog = applyFog(vec3(0.0), worldPos, cameraPos);
        // Compute fog factor for transmittance
        int mode = int(u_FogFlags.y + 0.5);
        float dist = distance(worldPos, cameraPos);
        float distFog = computeDistanceFog(dist, mode, baseDensity, u_FogDistanceParams.x, u_FogDistanceParams.y);
        float heightFog = computeHeightFog(worldPos, cameraPos, baseDensity, heightFalloff, heightOffset);
        float fogFactor = clamp(max(distFog, heightFog), 0.0, maxOpacity);

        // Add local fog volume contribution at the fragment position
        FogVolumeResult volResult = evaluateFogVolumesAtPoint(worldPos);
        if (volResult.density > 0.001)
        {
            float volFogFactor = clamp(volResult.density, 0.0, 1.0);
            fogFactor = clamp(fogFactor + volFogFactor, 0.0, maxOpacity);
            analyticFog = mix(analyticFog, volResult.color * volFogFactor, volFogFactor / max(fogFactor, 0.001));
        }

        o_Color = vec4(analyticFog, 1.0 - fogFactor);
        return;
    }

    // ===== VOLUMETRIC RAY-MARCH =====

    // Ray setup
    vec3 rayDir = worldPos - cameraPos;
    float rayLength = length(rayDir);

    float maxRayDist = max(u_FogDistanceParams.y, 500.0);
    if (depth > 0.9999)
        rayLength = maxRayDist;
    else
        rayLength = min(rayLength, maxRayDist);

    rayDir = normalize(rayDir);

    // Exponential step distribution — more samples near camera for detail,
    // fewer far away where fog is smoother. Based on log distribution.
    float logMaxDist = log(rayLength + 1.0);

    // Frame index packed into FogSunDirection.w for temporal jitter
    int frameIndex = int(u_FogSunDirection.w + 0.5);

    // Temporal jitter with per-frame offset to accumulate into temporal filter
    float jitter = interleavedGradientNoise(gl_FragCoord.xy + float(frameIndex % 16) * vec2(7.23, 3.79));

    // Precompute phase function (constant along the ray)
    float cosTheta = dot(rayDir, -sunDir);
    vec3 scatterCoeff = vec3(0.0);
    if (scatterEnabled)
    {
        float rPhase = rayleighPhase(cosTheta);
        float mPhase = dualLobeHGPhase(cosTheta, u_FogScatterParams.z);
        vec3 rayleigh = u_FogScatterParams.x * rPhase * u_FogRayleighColorAndMaxOpacity.rgb;
        vec3 mie = u_FogScatterParams.y * mPhase * vec3(1.0);
        scatterCoeff = u_FogScatterParams.w * (rayleigh + mie);
    }

    // Ray-march accumulation (Beer-Lambert with energy-conserving integration)
    vec3  accumulatedLight = vec3(0.0);
    float transmittance = 1.0;
    float prevT = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        // Exponential step distribution: t_i = exp(i/N * log(maxDist+1)) - 1
        float normalizedStep = (float(i) + jitter) / float(numSamples);
        float t = exp(normalizedStep * logMaxDist) - 1.0;
        float stepSize = t - prevT;
        prevT = t;

        if (stepSize < 0.001)
            continue;

        vec3 samplePos = cameraPos + rayDir * t;

        // Height-modulated global density
        float density = fogDensityAtPoint(samplePos, baseDensity, heightFalloff, heightOffset);

        // Animated noise modulation — multi-octave FBM with wind drift
        if (noiseEnabled && density > 0.0001)
        {
            vec3 noisePos = samplePos * noiseScale +
                            vec3(time * noiseSpeed * 0.6, time * noiseSpeed * 0.15,
                                 time * noiseSpeed * 0.4);
            float noiseFactor = fogFBM(noisePos);
            // Remap noise from [0, 1] to density multiplier — preserves volume shape
            density *= mix(1.0, clamp(noiseFactor * 2.0, 0.0, 2.5), noiseIntensity);
        }

        // Evaluate local fog volumes at this sample point
        FogVolumeResult volResult = evaluateFogVolumesAtPoint(samplePos);
        float combinedDensity = max(density, 0.0) + volResult.density;

        // Blend fog color: mix global color with volume color based on relative contribution
        vec3 sampleFogColor = fogColor;
        if (volResult.density > 0.001 && combinedDensity > 0.001)
        {
            float volumeRatio = volResult.density / combinedDensity;
            sampleFogColor = mix(fogColor, volResult.color, volumeRatio);
        }

        density = combinedDensity;

        // Beer-Lambert extinction for this step
        float extinction = (density + absorptionCoeff) * stepSize;
        float stepTransmittance = exp(-extinction);

        // In-scattering: ambient fog color + directional atmospheric scattering
        vec3 stepLight = sampleFogColor * density;
        if (scatterEnabled)
        {
            vec3 scatter = scatterCoeff * density;
            // Multi-scattering approximation (Frostbite)
            stepLight += multiScatterApprox(scatter, density);
        }

        // Volumetric light shafts — shadow map visibility
        if (lightShafts && density > 0.0001)
        {
            float shadow = sampleShadowForFog(samplePos);
            stepLight *= mix(0.15, 1.0, shadow) * lightShaftInt;
        }

        // Energy-conserving accumulation (Sébastien Hillaire's improved integration)
        vec3 integScatter = stepLight * (1.0 - stepTransmittance) / max(extinction, 0.00001);
        accumulatedLight += transmittance * integScatter;
        transmittance *= stepTransmittance;

        // Early termination when fully opaque
        if (transmittance < 0.005)
        {
            transmittance = 0.0;
            break;
        }
    }

    // Clamp by max opacity
    transmittance = max(transmittance, 1.0 - maxOpacity);

    // Clamp inscatter to prevent fireflies
    accumulatedLight = min(accumulatedLight, vec3(10.0));

    // ===== TEMPORAL REPROJECTION =====
    // Blend with previous frame's result to accumulate samples over time.
    // This effectively multiplies quality by ~4-8x without extra ray-march cost.
    vec4 currentResult = vec4(accumulatedLight, transmittance);

    if (frameIndex > 0)
    {
        // Find world position at a mid-point along the ray for reprojection
        float midT = min(rayLength * 0.3, maxRayDist * 0.3);
        vec3 midWorldPos = cameraPos + rayDir * midT;
        vec2 prevUV = reprojectUV(midWorldPos);

        if (prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
            prevUV.y >= 0.0 && prevUV.y <= 1.0)
        {
            vec4 history = texture(u_FogHistory, prevUV);

            // Neighborhood clamping: clamp history to a range around current
            // to reject ghosting from disoccluded regions
            vec4 minBound = currentResult - vec4(0.3, 0.3, 0.3, 0.15);
            vec4 maxBound = currentResult + vec4(0.3, 0.3, 0.3, 0.15);
            vec4 clampedHistory = clamp(history, minBound, maxBound);

            // Blend: ~10% current, ~90% history = smooth temporal accumulation
            float blendFactor = 0.1;
            currentResult = mix(clampedHistory, currentResult, blendFactor);
        }
    }

    o_Color = currentResult;
}
