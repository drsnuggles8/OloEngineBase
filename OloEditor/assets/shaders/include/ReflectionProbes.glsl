// =============================================================================
// ReflectionProbes.glsl — distance-impostor reflection probes (issue #705).
//
// Per-pixel selection + raymarched parallax correction against each probe's
// radial-distance cubemap (Szirmay-Kalos et al., "Approximate Ray-Tracing on
// the GPU with Distance Impostors", Eurographics 2005 — re-derived). The
// ENCODING CONTRACT (units, miss sentinel, max-mips, march budget) lives in
// OloEngine/Renderer/ReflectionProbeDistanceField.h; the constants below
// mirror it and the CPU reference raymarch expression-for-expression.
//
// Usage (fragment stages only — the cluster lookup reads gl_FragCoord):
//   #define OLO_REFLECTION_PROBE_SAMPLERS
//   #include "include/ReflectionProbes.glsl"
//   ...
//   vec4 probe = oloSampleReflectionProbes(worldPos, N, R, lod, viewDepth);
//   prefiltered = mix(prefiltered, probe.rgb, probe.a);
//
// The two samplerCubeArray slots are declared SLOT-BASED on purpose (the
// DDGICommon.glsl shape): ReflectionProbeArray::BindForShading publishes them
// through HeapBinding::PublishTextureOffsetAndBind, which stages the heap
// offset AND always issues a real bind — so bindless-route and slot-route
// consumers both work without per-shader #ifdef branches.
// =============================================================================

#ifndef REFLECTION_PROBES_GLSL
#define REFLECTION_PROBES_GLSL

#ifdef OLO_REFLECTION_PROBE_SAMPLERS

// ---- Contract constants (mirror ReflectionProbeDistanceField.h) ----
#define OLO_PROBE_MARCH_ITERATIONS 8
#define OLO_PROBE_MARCH_TAPS 4
#define OLO_PROBE_REFINE_STEPS 6
#define OLO_PROBE_INSIDE_BIAS_ABS 0.02
#define OLO_PROBE_INSIDE_BIAS_REL 0.01
// March-range slack: the crossing test carries the inside bias, so the plain
// |p0| + dMax bound ends a bias short of the crossing on head-on rays.
#define OLO_PROBE_MARCH_SLACK_REL 1.05
#define OLO_PROBE_MARCH_SLACK_ABS 0.1
#define OLO_PROBE_REJECT_REL_MARGIN 1.05
#define OLO_PROBE_REJECT_ABS_MARGIN 0.2
#define OLO_PROBE_REJECT_MIP 3.0
// 0.999 * kProbeDistanceFar (1000): stored distances at/above this are sky.
#define OLO_PROBE_MISS_THRESHOLD 999.0
// Start-point offset along the surface normal — keeps the march start
// strictly inside the captured surface (the shading point IS on it).
#define OLO_PROBE_NORMAL_OFFSET 0.05

// Slots mirror ShaderBindingLayout::TEX_REFLECTION_PROBE_* — published once
// per frame by ReflectionProbeArray::BindForShading.
layout(binding = 14) uniform samplerCubeArray u_ProbeRadianceArray; // prefilter chains (roughness mips)
layout(binding = 15) uniform samplerCubeArray u_ProbeDistanceArray; // radial distance + max-mips

// Mirrors UBOStructures::ReflectionProbeUBO (and ReflectionProbeCull.comp).
struct OloReflectionProbe
{
    vec4 PositionRadius; // xyz = render-relative position, w = influence radius
    vec4 Params;         // x = blend distance, y = intensity, z = dMax, w = array layer
};

layout(std140, binding = 58) uniform ReflectionProbeData
{
    uvec4 u_ProbeCounts;      // x = probe count, y = cluster grid valid, z = clusterCountX, w = clusterCountY
    vec4 u_ProbeTileScale;    // xy = clusterCount / screenSize, z = clusterCountZ, w = unused
    vec4 u_ProbeDepthSlicing; // x = sliceScale, y = sliceBias, z = zNear, w = zFar
    OloReflectionProbe u_Probes[32];
};

layout(std430, binding = 53) readonly buffer ReflectionProbeGridBuf
{
    uint probeGrid[];
};

// Per-cluster probe bitmask for this fragment. Same tile/slice math as
// ForwardPlusCommon.glsl's fplusClusterIndex, driven from the probe UBO's own
// copy of the parameters so probes keep working when Forward+ is inactive.
uint oloProbeClusterMask(float viewDepth)
{
    if (u_ProbeCounts.y == 0u)
    {
        return 0xFFFFFFFFu; // no grid this frame — test every probe
    }
    uvec2 tileCoord = uvec2(gl_FragCoord.xy * u_ProbeTileScale.xy);
    tileCoord = min(tileCoord, uvec2(u_ProbeCounts.z - 1u, u_ProbeCounts.w - 1u));
    float z = max(viewDepth, 1e-4);
    int sliceCount = int(u_ProbeTileScale.z);
    int slice = int(floor(log2(z) * u_ProbeDepthSlicing.x + u_ProbeDepthSlicing.y));
    slice = clamp(slice, 0, sliceCount - 1);
    uint clusterIndex = (uint(slice) * u_ProbeCounts.w + tileCoord.y) * u_ProbeCounts.z + tileCoord.x;
    return probeGrid[clusterIndex];
}

// Raymarch one probe's distance field. Mirrors the CPU reference
// (RaymarchProbeDistanceField): the environment is the star-shaped surface
// dist(u)*u around the probe centre; a point p is INSIDE while
// |p| < dist(p/|p|) + bias. March t over (0, dMax + |p0|], bracket the first
// inside->outside transition, bisect, and return the parallax-corrected
// lookup direction. Returns false on a miss (ray leaves through sky).
bool oloProbeRaymarch(vec3 originProbeSpace, vec3 dir, float dMax, float layer, out vec3 hitDirection)
{
    float tMax = (dMax + length(originProbeSpace)) * OLO_PROBE_MARCH_SLACK_REL + OLO_PROBE_MARCH_SLACK_ABS;
    float stepT = tMax / float(OLO_PROBE_MARCH_ITERATIONS * OLO_PROBE_MARCH_TAPS);

    // An outside sample only brackets a crossing after the march has SEEN an
    // inside sample. A shading point the probe cannot see (the far side of a
    // captured occluder) starts outside that occluder's radial cone, and
    // accepting the first outside sample there reports a bogus hit ON the
    // occluder's shell — leading outside samples are skipped instead until
    // the ray re-enters the visible region (mirrors the CPU reference).
    float tInside = -1.0;
    {
        vec3 p0 = originProbeSpace + dir * stepT;
        float r0 = length(p0);
        float d0 = textureLod(u_ProbeDistanceArray, vec4(p0, layer), 0.0).r;
        float bias0 = max(OLO_PROBE_INSIDE_BIAS_ABS, r0 * OLO_PROBE_INSIDE_BIAS_REL);
        if (r0 < d0 + bias0)
        {
            tInside = 0.0; // the t = 0 on-surface contract holds
        }
    }
    float tOutside = -1.0;
    for (int it = 0; it < OLO_PROBE_MARCH_ITERATIONS && tOutside < 0.0; ++it)
    {
        for (int tap = 0; tap < OLO_PROBE_MARCH_TAPS; ++tap)
        {
            float t = stepT * float(it * OLO_PROBE_MARCH_TAPS + tap + 1);
            vec3 p = originProbeSpace + dir * t;
            float r = length(p);
            float d = textureLod(u_ProbeDistanceArray, vec4(p, layer), 0.0).r;
            float bias = max(OLO_PROBE_INSIDE_BIAS_ABS, r * OLO_PROBE_INSIDE_BIAS_REL);
            if (r < d + bias)
            {
                tInside = t;
            }
            else if (tInside >= 0.0)
            {
                tOutside = t;
                break;
            }
        }
    }
    if (tOutside < 0.0)
    {
        return false;
    }

    for (int i = 0; i < OLO_PROBE_REFINE_STEPS; ++i)
    {
        float tMid = 0.5 * (tInside + tOutside);
        vec3 p = originProbeSpace + dir * tMid;
        float r = length(p);
        float d = textureLod(u_ProbeDistanceArray, vec4(p, layer), 0.0).r;
        float bias = max(OLO_PROBE_INSIDE_BIAS_ABS, r * OLO_PROBE_INSIDE_BIAS_REL);
        if (r < d + bias)
        {
            tInside = tMid;
        }
        else
        {
            tOutside = tMid;
        }
    }

    vec3 hitPoint = originProbeSpace + dir * (0.5 * (tInside + tOutside));
    float hitRadius = length(hitPoint);
    if (hitRadius <= 0.0)
    {
        return false;
    }
    hitDirection = hitPoint / hitRadius;

    // A crossing whose stored distance is the miss sentinel is the ray
    // leaving through SKY, not a surface hit — report a miss so the caller
    // falls back to the live global sky (mirrors the CPU reference).
    if (textureLod(u_ProbeDistanceArray, vec4(hitDirection, layer), 0.0).r >= OLO_PROBE_MISS_THRESHOLD)
    {
        return false;
    }
    return true;
}

// Blended parallax-corrected specular radiance from every probe covering the
// shading point. Returns rgb = intensity-weighted radiance, a = coverage in
// [0,1] — the caller mixes toward the global prefilter map by (1 - a), which
// is the "falls back cleanly to sky" half of the #705 acceptance criteria.
//
//   worldPos    — render-relative world position (G-Buffer reconstruction)
//   N           — shading normal (world)
//   R           — unit reflection direction (world)
//   radianceLod — prefilter mip to sample (roughness * MAX_REFLECTION_LOD)
//   viewDepth   — positive view-space depth for the cluster lookup
vec4 oloSampleReflectionProbes(vec3 worldPos, vec3 N, vec3 R, float radianceLod, float viewDepth)
{
    uint count = min(u_ProbeCounts.x, 32u);
    if (count == 0u)
    {
        return vec4(0.0);
    }

    uint validBits = (count >= 32u) ? 0xFFFFFFFFu : ((1u << count) - 1u);
    uint mask = oloProbeClusterMask(viewDepth) & validBits;

    vec3 radianceSum = vec3(0.0);
    float weightSum = 0.0;

    while (mask != 0u)
    {
        int i = findLSB(mask);
        mask &= mask - 1u;

        OloReflectionProbe probe = u_Probes[i];
        vec3 toPoint = worldPos - probe.PositionRadius.xyz;
        float pointDistance = length(toPoint);
        float radius = probe.PositionRadius.w;
        if (pointDistance >= radius)
        {
            continue;
        }

        // Influence weight: 1 deep inside, fading to 0 across BlendDistance
        // at the sphere edge (m_BlendDistance finally consumed — it was
        // authored but unread before #705).
        float weight = clamp((radius - pointDistance) / probe.Params.x, 0.0, 1.0);
        if (weight <= 0.0)
        {
            continue;
        }

        // Cheap reject (max-mip = conservative upper bound): if even the
        // largest distance in the cone toward the point is closer than the
        // point, the probe cannot see it — occluded, skip before marching.
        float coneMax = textureLod(u_ProbeDistanceArray, vec4(toPoint, probe.Params.w), OLO_PROBE_REJECT_MIP).r;
        if (pointDistance > coneMax * OLO_PROBE_REJECT_REL_MARGIN + OLO_PROBE_REJECT_ABS_MARGIN)
        {
            continue;
        }

        vec3 origin = (worldPos + N * OLO_PROBE_NORMAL_OFFSET) - probe.PositionRadius.xyz;
        vec3 hitDirection;
        if (!oloProbeRaymarch(origin, R, probe.Params.z, probe.Params.w, hitDirection))
        {
            continue; // miss — this probe defers to the sky fallback
        }

        vec3 radiance = textureLod(u_ProbeRadianceArray, vec4(hitDirection, probe.Params.w), radianceLod).rgb;
        radianceSum += radiance * probe.Params.y * weight;
        weightSum += weight;
    }

    if (weightSum <= 0.0)
    {
        return vec4(0.0);
    }
    return vec4(radianceSum / weightSum, min(weightSum, 1.0));
}

#endif // OLO_REFLECTION_PROBE_SAMPLERS

#endif // REFLECTION_PROBES_GLSL
