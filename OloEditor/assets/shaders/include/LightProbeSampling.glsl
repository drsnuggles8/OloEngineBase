// =============================================================================
// LightProbeSampling.glsl — probe-volume irradiance sampling
//
// Two backends behind one entry point (issue #632):
//   - sampleLightProbeGrid: the baked path — trilinear L2-SH blend with a
//     binary validity skip. NO visibility term: it leaks light through walls,
//     which is why Realtime/Hybrid volumes exist.
//   - sampleDDGIIrradiance (include/DDGICommon.glsl): the realtime path —
//     trilinear × wrap-shading × Chebyshev-visibility weighting against the
//     GPU-relit octahedral atlases. This is the leak-fixed sampler.
// Lit passes call sampleProbeVolumeIrradiance(), which routes/blends per the
// bound volume's mode (u_DDGIEnabled / u_DDGIHybridBlend from UBO 51).
//
// Depends on: SphericalHarmonics.glsl, DDGICommon.glsl
// =============================================================================

#ifndef LIGHT_PROBE_SAMPLING_GLSL
#define LIGHT_PROBE_SAMPLING_GLSL

#include "SphericalHarmonics.glsl"

// Lit-pass consumers sample the engine-global DDGI atlas bindings (56/57/58).
// The DDGI update passes bind their own units and must NOT include this file.
#define DDGI_GLOBAL_SAMPLERS
#include "DDGICommon.glsl"

// Light probe volume UBO (binding 22)
layout(std140, binding = 22) uniform LightProbeVolume {
    vec4  u_ProbeBoundsMin;      // xyz = min corner
    vec4  u_ProbeBoundsMax;      // xyz = max corner
    ivec4 u_ProbeGridDimensions; // xyz = count per axis, w = total
    vec4  u_ProbeSpacing;        // xyz = spacing per axis
    int   u_ProbesEnabled;
    float u_ProbeIntensity;
    int   _probePad0;
    int   _probePad1;
};

// Light probe SH coefficient SSBO (binding 8)
// Each probe stores SH_COEFFICIENT_COUNT (9) vec4 values contiguously.
// First coefficient's .w is the validity flag (1.0 = valid, 0.0 = invalid).
layout(std430, binding = 8) readonly buffer LightProbeSHData {
    vec4 u_ProbeSHCoeffs[];
};

// Convert world position to fractional grid coordinates
vec3 worldToProbeGrid(vec3 worldPos)
{
    vec3 extent = u_ProbeBoundsMax.xyz - u_ProbeBoundsMin.xyz;
    extent = max(extent, vec3(1e-6)); // Guard against zero extent
    vec3 normalized = (worldPos - u_ProbeBoundsMin.xyz) / extent;
    return normalized * vec3(u_ProbeGridDimensions.xyz - ivec3(1));
}

// Check if a world position is inside the probe volume
bool isInsideProbeVolume(vec3 worldPos)
{
    return all(greaterThanEqual(worldPos, u_ProbeBoundsMin.xyz)) &&
           all(lessThanEqual(worldPos, u_ProbeBoundsMax.xyz));
}

// Linearize 3D grid index
int probeLinearIndex(int x, int y, int z)
{
    return z * u_ProbeGridDimensions.y * u_ProbeGridDimensions.x
         + y * u_ProbeGridDimensions.x
         + x;
}

// Fetch SH coefficients for a single probe by its linear index
// Returns the validity flag (0.0 or 1.0)
float fetchProbeSH(int linearIdx, out vec3 coeffs[SH_COEFFICIENT_COUNT])
{
    int base = linearIdx * SH_COEFFICIENT_COUNT;
    float validity = u_ProbeSHCoeffs[base].w;
    for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
    {
        coeffs[i] = u_ProbeSHCoeffs[base + i].xyz;
    }
    return validity;
}

// Sample the probe grid with trilinear interpolation and validity weighting
// Returns vec3 irradiance for the given world position and surface normal
vec3 sampleLightProbeGrid(vec3 worldPos, vec3 normal)
{
    if (u_ProbesEnabled == 0)
        return vec3(0.0);

    if (!isInsideProbeVolume(worldPos))
        return vec3(0.0);

    vec3 gridPos = worldToProbeGrid(worldPos);

    // Integer corners and fractional offset
    ivec3 p0 = ivec3(floor(gridPos));
    ivec3 p1 = min(p0 + ivec3(1), u_ProbeGridDimensions.xyz - ivec3(1));
    p0 = max(p0, ivec3(0));
    vec3 frac = gridPos - vec3(p0);

    // Accumulate weighted SH from 8 corner probes
    vec3 blendedCoeffs[SH_COEFFICIENT_COUNT];
    for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
        blendedCoeffs[i] = vec3(0.0);

    float totalWeight = 0.0;

    for (int dz = 0; dz <= 1; ++dz)
    {
        for (int dy = 0; dy <= 1; ++dy)
        {
            for (int dx = 0; dx <= 1; ++dx)
            {
                ivec3 corner = ivec3(
                    dx == 0 ? p0.x : p1.x,
                    dy == 0 ? p0.y : p1.y,
                    dz == 0 ? p0.z : p1.z
                );

                int idx = probeLinearIndex(corner.x, corner.y, corner.z);

                vec3 probeCoeffs[SH_COEFFICIENT_COUNT];
                float validity = fetchProbeSH(idx, probeCoeffs);

                // Skip invalid probes (inside geometry)
                if (validity < 0.5)
                    continue;

                // Trilinear weight
                float wx = dx == 0 ? (1.0 - frac.x) : frac.x;
                float wy = dy == 0 ? (1.0 - frac.y) : frac.y;
                float wz = dz == 0 ? (1.0 - frac.z) : frac.z;
                float w = wx * wy * wz;

                for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
                    blendedCoeffs[i] += probeCoeffs[i] * w;

                totalWeight += w;
            }
        }
    }

    // Renormalize if some probes were skipped
    if (totalWeight < 0.001)
        return vec3(0.0);

    float invWeight = 1.0 / totalWeight;
    for (int i = 0; i < SH_COEFFICIENT_COUNT; ++i)
        blendedCoeffs[i] *= invWeight;

    // Evaluate SH for the surface normal
    vec3 irradiance = evaluateSH(blendedCoeffs, normal);
    return irradiance * u_ProbeIntensity;
}

// Unified probe-volume irradiance for the lit passes (issue #632).
// viewDir points FROM the surface TOWARD the camera (needed by the DDGI
// self-shadow bias). Returns vec3(0) when no probe volume applies — callers
// keep their existing IBL/simple-ambient fallback ladder.
//
// Hybrid mode: u_DDGIHybridBlend ramps 0 -> 1 with DDGI capture coverage, so
// a freshly-enabled volume shows baked SH instantly and hands over to the
// realtime atlases as they fill; DDGI rejection (outside volume, every corner
// probe uncaptured) falls back to baked entirely rather than blending in
// black.
vec3 sampleProbeVolumeIrradiance(vec3 worldPos, vec3 normal, vec3 viewDir)
{
    vec3 baked = vec3(0.0);
    bool bakedNeeded = (u_DDGIEnabled == 0) || (u_DDGIHybridBlend < 0.999);
    if (bakedNeeded)
    {
        baked = sampleLightProbeGrid(worldPos, normal);
    }
    if (u_DDGIEnabled == 0)
    {
        return baked;
    }

    vec3 ddgi = sampleDDGIIrradiance(worldPos, normal, viewDir);
    if (dot(ddgi, ddgi) <= 0.0)
    {
        return baked;
    }
    return mix(baked, ddgi, u_DDGIHybridBlend);
}

#endif // LIGHT_PROBE_SAMPLING_GLSL
