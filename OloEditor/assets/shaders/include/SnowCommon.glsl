// =============================================================================
// SnowCommon.glsl - Snow BRDF, SSS, sparkle, and procedural coverage
// =============================================================================
// Include after PBRCommon.glsl (uses distributionGGX, geometrySmith, fresnelSchlick)

#ifndef SNOW_COMMON_GLSL
#define SNOW_COMMON_GLSL

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

// =============================================================================
// SUBSURFACE SCATTERING APPROXIMATION
// =============================================================================

// Christensen-Burley wrap-lighting SSS approximation
// Wraps NdotL to allow light to bleed into shadow side, tinted by sssColor
vec3 subsurfaceDiffuse(vec3 N, vec3 L, vec3 V, vec3 sssColor, float sssIntensity)
{
    // Wrap factor: how far light wraps around the surface (0 = Lambert, 1 = full wrap)
    float wrapFactor = 0.5 * sssIntensity;

    float NdotL = dot(N, L);
    // Wrapped diffuse: remap [-1,1] to [0,1] with wrap
    float wrappedDiffuse = max(0.0, (NdotL + wrapFactor) / ((1.0 + wrapFactor) * (1.0 + wrapFactor)));

    // Back-scattered light (transmitted through thin features)
    float NdotV = max(dot(N, V), 0.0);
    vec3 H = normalize(L + V);
    float VdotH = max(dot(V, H), 0.001);

    // Forrest/Burley scattering profile approximation
    float scatter = smoothstep(0.0, 1.0, wrappedDiffuse);

    // Transmitted light: strongest on shadow side, tinted by sssColor
    float transmission = max(0.0, -NdotL) * sssIntensity * 0.3;

    // Combine: standard wrapped diffuse + colored transmission
    vec3 diffuse = vec3(wrappedDiffuse) + sssColor * transmission;

    return diffuse * INV_PI;
}

// =============================================================================
// ICE CRYSTAL SPARKLE / GLINT
// =============================================================================

// View-dependent sparkle simulating individual ice crystal reflections
// Returns additive specular intensity
float snowSparkle(vec3 V, vec3 N, vec3 worldPos, float sparkleIntensity,
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

            // Reflection direction from this micro-facet
            vec3 R = reflect(-V, microNormal);

            // Check if any light direction aligns with reflection
            // Use a sharp threshold to create point-like sparkles
            float alignment = max(dot(R, N), 0.0);
            float glint = pow(alignment, 256.0) * crystalHash.z;

            // View-dependent masking: sparkles appear/disappear as camera moves
            float viewMask = step(0.92, crystalHash.x + dot(V, microNormal) * 0.15);

            sparkle += glint * viewMask;
        }
    }

    return sparkle * sparkleIntensity;
}

// =============================================================================
// SNOW NORMAL PERTURBATION
// =============================================================================

// Subtle noise-based normal perturbation for crystalline micro-surface detail
vec3 perturbSnowNormal(vec3 N, vec3 worldPos, float strength)
{
    if (strength < 0.001)
        return N;

    // Multi-frequency noise for natural crystal patterns
    vec3 noise1 = hash33(worldPos * 50.0) * 2.0 - 1.0;
    vec3 noise2 = hash33(worldPos * 150.0) * 2.0 - 1.0;

    vec3 perturbation = (noise1 * 0.6 + noise2 * 0.4) * strength;

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
    // slopeStart = normal.y below which snow starts reducing (e.g., 0.7)
    // slopeFull = normal.y below which no snow at all (e.g., 0.3)
    float slopeWeight = smoothstep(slopeFull, slopeStart, normalY);
    // Invert: we want MORE snow on flat (high normalY)
    slopeWeight = 1.0 - slopeWeight;

    return heightWeight * slopeWeight;
}

// =============================================================================
// COMBINED SNOW BRDF
// =============================================================================

// Full snow shading: SSS diffuse + Cook-Torrance GGX specular + sparkle
vec3 snowBRDF(vec3 N, vec3 V, vec3 L, vec3 albedo, float roughness,
              vec3 sssColor, float sssIntensity, float sparkleIntensity,
              float sparkleDensity, float sparkleScale, vec3 worldPos)
{
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Snow is dielectric: F0 = 0.04 (ice/snow typical)
    vec3 F0 = vec3(DEFAULT_DIELECTRIC_F0);

    // Specular: standard Cook-Torrance
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(HdotV, F0);

    vec3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    vec3 specular = numerator / denominator;

    // Diffuse: SSS wrap lighting instead of Lambert
    vec3 kD = (vec3(1.0) - F);
    vec3 diffuse = kD * albedo * subsurfaceDiffuse(N, L, V, sssColor, sssIntensity);

    // Sparkle: additive specular glint
    float sparkle = snowSparkle(V, N, worldPos, sparkleIntensity, sparkleDensity, sparkleScale);
    vec3 sparkleContrib = vec3(sparkle) * F;

    return (diffuse + specular + sparkleContrib) * NdotL;
}

#endif // SNOW_COMMON_GLSL
