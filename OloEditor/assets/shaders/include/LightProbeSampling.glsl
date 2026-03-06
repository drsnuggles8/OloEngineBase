// =============================================================================
// LightProbeSampling.glsl — Trilinear probe grid sampling with validity checks
// Depends on: SphericalHarmonics.glsl
// =============================================================================

#ifndef LIGHT_PROBE_SAMPLING_GLSL
#define LIGHT_PROBE_SAMPLING_GLSL

#include "SphericalHarmonics.glsl"

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

#endif // LIGHT_PROBE_SAMPLING_GLSL
