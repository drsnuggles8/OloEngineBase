// =============================================================================
// FogCommon.glsl — Shared fog, atmospheric scattering & volumetric functions
//
// Provides:
//   - Distance fog (linear / exponential / exponential²)
//   - Height fog (Unreal Engine-style exponential integral along view ray)
//   - Physically-based Rayleigh + Mie atmospheric inscattering
//   - Volumetric ray-marching with noise-modulated density
//   - Volumetric light shafts via shadow map sampling
//
// For analytical fog (particles):
//   #include "include/FogCommon.glsl"
//   color = applyFog(color, worldPos, cameraPos);
//
// For volumetric post-process:
//   color = applyVolumetricFog(color, worldPos, cameraPos, depth, texCoord, ...);
// =============================================================================

#ifndef FOG_COMMON_GLSL
#define FOG_COMMON_GLSL

#ifndef PI
#define PI 3.14159265359
#endif

// Fog mode constants (match FogMode enum on CPU side)
#define FOG_LINEAR             0
#define FOG_EXPONENTIAL        1
#define FOG_EXPONENTIAL_SQ     2

// Fog & Atmospheric Scattering UBO (std140, binding 17)
layout(std140, binding = 17) uniform FogData
{
    vec4 u_FogColorAndDensity;            // rgb = fog color, a = density
    vec4 u_FogDistanceParams;             // x = start, y = end, z = heightFalloff, w = heightOffset
    vec4 u_FogScatterParams;              // x = rayleighStrength, y = mieStrength, z = mieG, w = sunIntensity
    vec4 u_FogRayleighColorAndMaxOpacity; // rgb = rayleigh color, a = maxOpacity
    vec4 u_FogSunDirection;               // xyz = normalized sun direction, w = fogFrameIndex (temporal jitter)
    vec4 u_FogFlags;                      // x = enabled, y = mode, z = scatteringEnabled, w = volumetricEnabled
    vec4 u_FogNoiseParams;                // x = noiseScale, y = noiseSpeed, z = noiseIntensity, w = time
    vec4 u_FogVolumetricParams;           // x = volumetricSamples, y = absorptionCoeff, z = lightShaftIntensity, w = lightShaftsEnabled
};

// ---------------------------------------------------------------------------
// Interleaved Gradient Noise — high quality temporal dithering without a
// texture. Returns [0, 1) given a screen-space fragment coordinate.
// Reference: Jimenez 2014, "Next Generation Post Processing in Call of Duty"
// ---------------------------------------------------------------------------
float interleavedGradientNoise(vec2 screenPos)
{
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

// ---------------------------------------------------------------------------
// Simple 3D hash — procedural noise without a texture lookup.
// Returns [0, 1) given a world-space position.
// ---------------------------------------------------------------------------
float hash3D(vec3 p)
{
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

// ---------------------------------------------------------------------------
// Value noise 3D — smooth procedural noise via trilinear interpolation
// of hashed lattice points. No texture required.
// ---------------------------------------------------------------------------
float valueNoise3D(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    // Smooth Hermite interpolation
    vec3 u = f * f * (3.0 - 2.0 * f);

    return mix(mix(mix(hash3D(i + vec3(0,0,0)), hash3D(i + vec3(1,0,0)), u.x),
                   mix(hash3D(i + vec3(0,1,0)), hash3D(i + vec3(1,1,0)), u.x), u.y),
               mix(mix(hash3D(i + vec3(0,0,1)), hash3D(i + vec3(1,0,1)), u.x),
                   mix(hash3D(i + vec3(0,1,1)), hash3D(i + vec3(1,1,1)), u.x), u.y), u.z);
}

// ---------------------------------------------------------------------------
// FBM (Fractal Brownian Motion) — layered noise for organic fog density.
// 3 octaves gives good visual quality at reasonable cost.
// ---------------------------------------------------------------------------
float fogFBM(vec3 p)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for (int i = 0; i < 3; ++i)
    {
        value += amplitude * valueNoise3D(p * frequency);
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

// ---------------------------------------------------------------------------
// Distance fog factor (0 = no fog, 1 = fully fogged)
// ---------------------------------------------------------------------------
float computeDistanceFog(float dist, int mode, float density, float fogStart, float fogEnd)
{
    if (mode == FOG_LINEAR)
    {
        return 1.0 - clamp((fogEnd - dist) / max(fogEnd - fogStart, 0.0001), 0.0, 1.0);
    }
    else if (mode == FOG_EXPONENTIAL)
    {
        return 1.0 - exp(-density * dist);
    }
    else // FOG_EXPONENTIAL_SQ
    {
        float dd = density * dist;
        return 1.0 - exp(-dd * dd);
    }
}

// ---------------------------------------------------------------------------
// Height-dependent fog density at a single world-space point.
// density(h) = baseDensity * exp(-heightFalloff * (h - heightOffset))
// ---------------------------------------------------------------------------
float fogDensityAtPoint(vec3 pos, float baseDensity, float heightFalloff, float heightOffset)
{
    float h = pos.y - heightOffset;
    return baseDensity * exp(-heightFalloff * max(h, 0.0));
}

// ---------------------------------------------------------------------------
// Height fog — closed-form exponential integral along the camera→fragment ray.
// Based on the standard volumetric height-fog model used in AAA engines.
// ---------------------------------------------------------------------------
float computeHeightFog(vec3 worldPos, vec3 cameraPos, float density,
                       float heightFalloff, float heightOffset)
{
    float rayLength = distance(worldPos, cameraPos);
    if (rayLength < 0.001 || heightFalloff < 0.0001)
        return 0.0;

    float camH = cameraPos.y - heightOffset;
    float fragH = worldPos.y - heightOffset;
    float deltaH = camH - fragH;

    float camDensity = exp(-heightFalloff * camH);
    float fragDensity = exp(-heightFalloff * fragH);

    float fogIntegral;
    if (abs(deltaH) > 0.001)
    {
        fogIntegral = (camDensity - fragDensity) / (heightFalloff * deltaH);
    }
    else
    {
        fogIntegral = camDensity;
    }

    return 1.0 - exp(-density * fogIntegral * rayLength);
}

// ---------------------------------------------------------------------------
// Rayleigh phase function
// ---------------------------------------------------------------------------
float rayleighPhase(float cosTheta)
{
    return (3.0 / (16.0 * PI)) * (1.0 + cosTheta * cosTheta);
}

// ---------------------------------------------------------------------------
// Henyey-Greenstein Mie phase function
// g: asymmetry parameter (0 = isotropic, ~0.76 = forward-dominated)
// ---------------------------------------------------------------------------
float henyeyGreensteinPhase(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * PI * denom * sqrt(denom));
}

// ---------------------------------------------------------------------------
// Compute atmospheric inscattering color (Rayleigh + Mie)
// ---------------------------------------------------------------------------
vec3 computeInscattering(vec3 viewDir, vec3 sunDir,
                         float rayleighStrength, float mieStrength, float mieG,
                         vec3 rayleighColor, float sunIntensity)
{
    float cosTheta = dot(viewDir, -sunDir);

    float rPhase = rayleighPhase(cosTheta);
    float mPhase = henyeyGreensteinPhase(cosTheta, mieG);

    vec3 rayleigh = rayleighStrength * rPhase * rayleighColor;
    vec3 mie = mieStrength * mPhase * vec3(1.0);

    return sunIntensity * (rayleigh + mie);
}

// ---------------------------------------------------------------------------
// Analytical fog — simple per-fragment fog for lightweight use (particles etc.)
// Returns the fogged color.
// ---------------------------------------------------------------------------
vec3 applyFog(vec3 fragColor, vec3 worldPos, vec3 cameraPos)
{
    if (u_FogFlags.x < 0.5)
        return fragColor;

    int mode = int(u_FogFlags.y + 0.5);
    float density = u_FogColorAndDensity.a;
    float fogStart = u_FogDistanceParams.x;
    float fogEnd = u_FogDistanceParams.y;
    float heightFalloff = u_FogDistanceParams.z;
    float heightOffset = u_FogDistanceParams.w;
    float maxOpacity = u_FogRayleighColorAndMaxOpacity.a;
    vec3 fogColor = u_FogColorAndDensity.rgb;

    float dist = distance(worldPos, cameraPos);

    float distFog = computeDistanceFog(dist, mode, density, fogStart, fogEnd);
    float heightFog = computeHeightFog(worldPos, cameraPos, density, heightFalloff, heightOffset);
    float fogFactor = clamp(max(distFog, heightFog), 0.0, maxOpacity);

    vec3 finalFogColor = fogColor;
    if (u_FogFlags.z > 0.5)
    {
        vec3 viewDir = normalize(worldPos - cameraPos);
        vec3 sunDir = normalize(u_FogSunDirection.xyz);
        vec3 inscatter = computeInscattering(
            viewDir, sunDir,
            u_FogScatterParams.x, u_FogScatterParams.y, u_FogScatterParams.z,
            u_FogRayleighColorAndMaxOpacity.rgb, u_FogScatterParams.w);
        finalFogColor = fogColor + inscatter * fogFactor;
        finalFogColor = min(finalFogColor, vec3(4.0));
    }

    return mix(fragColor, finalFogColor, fogFactor);
}

// ---------------------------------------------------------------------------
// Volumetric ray-marched fog — full-quality post-process fog with:
//   - Beer-Lambert transmittance accumulation
//   - Height-modulated density
//   - 3D noise density modulation (animated)
//   - Rayleigh + Mie inscattering per step
//   - Shadow map sampling for volumetric light shafts
//   - Interleaved gradient noise temporal jitter
//
#endif // FOG_COMMON_GLSL
