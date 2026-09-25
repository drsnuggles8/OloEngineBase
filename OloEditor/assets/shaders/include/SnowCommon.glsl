// =============================================================================
// SnowCommon.glsl - snow's procedural helpers: the crystal hash / noise, the
// sparkle glint, the crystalline normal perturbation and the coverage weight.
// =============================================================================
// Self-contained: no PBR helpers, no UBOs. How a surface USES these — snow as a
// material layer, one definition for every path — is include/SnowLayer.glsl.
//
// --- THE SNOW CONTRACT (issue #1451) -----------------------------------------
// Scene-colour alpha is NOT a snow channel. It used to carry the subsurface
// blur's mask, which every non-snow writer set to 1 (opaque PBR, the deferred
// lighting pass) or to its own blend alpha (foliage, glass, particles), so the
// blur covered the whole frame. The mask now travels in the diffusion hand-off
// lane, scene attachment 4, in a range disjoint from skin's:
//   1. PRODUCED: every lit pass that shades snow writes (diffuse half, -weight)
//      there — PBR_MultiLight(_Skinned), Terrain_PBR and DeferredLighting(_MSAA)
//      — and every other writer writes what it always did (0, or a skin slot).
//   2. CONSUMED: SSS_Blur.glsl adds strength * (blur(diffuse) - diffuse) into
//      scene colour for snow pixels only; SkinDiffusion.glsl reads the same
//      value as "names no profile".
//   3. DEFERRED: G-Buffer RT3.a (the material profile) carries the weight from
//      the G-Buffer writers to the lighting pass.
// See include/SnowDiffusionCommon.glsl for the encoding.
// =============================================================================

#ifndef SNOW_COMMON_GLSL
#define SNOW_COMMON_GLSL

// The wind block is needed only by the drift-aware coverage overload below. A
// shader that only READS a snow weight (the deferred lighting pass) defines
// OLO_SNOW_COMMON_NO_WIND and pays for neither.
#ifndef OLO_SNOW_COMMON_NO_WIND
#include "WindSampling.glsl"
#endif

// =============================================================================
// HASH NOISE UTILITIES
// =============================================================================

// High-quality 3D hash for sparkle (based on pcg3d)
vec3 hash33(vec3 p)
{
    uvec3 q = uvec3(ivec3(p * 1000.0)) * uvec3(1597334673u, 3812015801u, 2798796415u);
    q = (q.x ^ q.y ^ q.z) * uvec3(1597334673u, 3812015801u, 2798796415u);
    return vec3(q) * (1.0 / float(0xFFFFFFFFu));
}

float hash13(vec3 p)
{
    uvec3 q = uvec3(ivec3(p * 1000.0)) * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint h = (q.x ^ q.y ^ q.z) * 1597334673u;
    return float(h) * (1.0 / float(0xFFFFFFFFu));
}

// Smooth 3D value noise via trilinear interpolation of hash values
// Returns vec3 in [0, 1] — continuous and differentiable
vec3 smoothNoise3(vec3 p)
{
    vec3 pf = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // Hermite smoothstep interpolant

    vec3 n000 = hash33(pf);
    vec3 n100 = hash33(pf + vec3(1.0, 0.0, 0.0));
    vec3 n010 = hash33(pf + vec3(0.0, 1.0, 0.0));
    vec3 n110 = hash33(pf + vec3(1.0, 1.0, 0.0));
    vec3 n001 = hash33(pf + vec3(0.0, 0.0, 1.0));
    vec3 n101 = hash33(pf + vec3(1.0, 0.0, 1.0));
    vec3 n011 = hash33(pf + vec3(0.0, 1.0, 1.0));
    vec3 n111 = hash33(pf + vec3(1.0, 1.0, 1.0));

    vec3 nx00 = mix(n000, n100, f.x);
    vec3 nx10 = mix(n010, n110, f.x);
    vec3 nx01 = mix(n001, n101, f.x);
    vec3 nx11 = mix(n011, n111, f.x);

    vec3 nxy0 = mix(nx00, nx10, f.y);
    vec3 nxy1 = mix(nx01, nx11, f.y);

    return mix(nxy0, nxy1, f.z);
}

// =============================================================================
// ICE CRYSTAL SPARKLE / GLINT
// =============================================================================

// View- and light-dependent sparkle simulating individual ice crystal reflections
// Returns additive specular intensity
float snowSparkle(vec3 V, vec3 N, vec3 L, vec3 worldPos, float sparkleIntensity,
                  float sparkleDensity, float sparkleScale)
{
    if (sparkleIntensity < 0.001)
        return 0.0;

    // World-space cell grid for crystal positions
    vec3 cellPos = floor(worldPos * sparkleDensity);

    float sparkle = 0.0;

    // Check neighboring cells for closest glint
    for (int dx = -1; dx <= 1; ++dx)
    {
        for (int dz = -1; dz <= 1; ++dz)
        {
            vec3 cell = cellPos + vec3(float(dx), 0.0, float(dz));
            vec3 crystalHash = hash33(cell * sparkleScale);

            // Random micro-normal for this crystal
            vec3 microNormal = normalize(N + (crystalHash * 2.0 - 1.0) * 0.3);

            // Reflection of view off micro-facet — sparkles when aligned with light
            vec3 R = reflect(-V, microNormal);
            float alignment = max(dot(R, L), 0.0);
            // alignment^128 as a squaring chain (7 mults; alignment in [0,1]).
            // This runs once per cell in the surrounding 3x3 sparkle grid, so the
            // avoided pow() exp2/log2 is paid back per snow pixel several times.
            float al2 = alignment * alignment;
            float al4 = al2 * al2;
            float al8 = al4 * al4;
            float al16 = al8 * al8;
            float al32 = al16 * al16;
            float al64 = al32 * al32;
            float glint = (al64 * al64) * crystalHash.z; // alignment^128

            // View-dependent masking: sparkles appear/disappear as camera moves
            float viewMask = step(0.85, crystalHash.x + dot(V, microNormal) * 0.2);

            sparkle += glint * viewMask;
        }
    }

    return sparkle * sparkleIntensity;
}

// =============================================================================
// SNOW NORMAL PERTURBATION
// =============================================================================

// Smooth noise-based normal perturbation for crystalline micro-surface detail
vec3 perturbSnowNormal(vec3 N, vec3 worldPos, float strength)
{
    if (strength < 0.001)
        return N;

    // Multi-frequency smooth noise for natural snow crystal patterns
    vec3 noise1 = smoothNoise3(worldPos * 8.0) * 2.0 - 1.0;   // large undulations
    vec3 noise2 = smoothNoise3(worldPos * 32.0) * 2.0 - 1.0;  // medium detail
    vec3 noise3 = smoothNoise3(worldPos * 96.0) * 2.0 - 1.0;  // fine crystalline detail

    vec3 perturbation = (noise1 * 0.5 + noise2 * 0.35 + noise3 * 0.15) * strength;

    // Project perturbation onto tangent plane to avoid flipping normal
    perturbation -= N * dot(perturbation, N);

    return normalize(N + perturbation);
}

// =============================================================================
// PROCEDURAL SNOW COVERAGE WEIGHT
// =============================================================================

// Computes how much snow covers a surface based on world height and slope
// Returns 0.0 (no snow) to 1.0 (full snow)
float computeSnowWeight(float worldPosY, float normalY, float heightStart,
                        float heightFull, float slopeStart, float slopeFull)
{
    // Height factor: snow starts at heightStart, full coverage at heightFull
    float heightWeight = smoothstep(heightStart, heightFull, worldPosY);

    // Slope factor: snow sticks to flatter surfaces, slides off steep ones
    // normalY = 1.0 for flat, 0.0 for vertical
    // slopeFull = normal.y below which no snow at all (e.g., 0.3)
    // slopeStart = normal.y above which full snow coverage (e.g., 0.7)
    // smoothstep(0.3, 0.7, 1.0) = 1.0 → flat = full snow
    // smoothstep(0.3, 0.7, 0.0) = 0.0 → vertical = no snow
    float slopeWeight = smoothstep(slopeFull, slopeStart, normalY);

    return heightWeight * slopeWeight;
}

#ifndef OLO_SNOW_COMMON_NO_WIND
// Wind-drift-aware overload: windward surfaces accumulate more snow,
// leeward surfaces accumulate less.  Requires WindSampling.glsl.
float computeSnowWeight(float worldPosY, vec3 worldNormal, float heightStart,
                        float heightFull, float slopeStart, float slopeFull,
                        float windDriftFactor)
{
    float baseWeight = computeSnowWeight(worldPosY, worldNormal.y,
                                         heightStart, heightFull,
                                         slopeStart, slopeFull);

    if (windDriftFactor > 0.001 && windEnabled())
    {
        // dot(normal, -windDir) > 0 for surfaces facing into the wind
        vec3 windDir = normalize(windDirection());
        float windFacing = dot(worldNormal, -windDir);
        // Remap [-1, 1] to a multiplicative bias around 1.0
        float driftBias = 1.0 + windFacing * windDriftFactor;
        baseWeight *= clamp(driftBias, 0.0, 2.0);
    }

    return clamp(baseWeight, 0.0, 1.0);
}
#endif

#endif // SNOW_COMMON_GLSL
