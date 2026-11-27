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
#define OLO_PROBE_GAP_REFINE_STEPS 6
// Radians a hidden hit's radiance lookup steps back across the silhouette.
#define OLO_PROBE_INPAINT_MARGIN 0.02
// The ring search for a hidden hit's radiance (kProbeInpaintRingCount & co.).
#define OLO_PROBE_INPAINT_RING_COUNT 4
#define OLO_PROBE_INPAINT_AZIMUTHS 8
#define OLO_PROBE_INPAINT_FIRST_RING 0.03
#define OLO_PROBE_INPAINT_MATCH_ABS 0.1
#define OLO_PROBE_INPAINT_MATCH_REL 0.03
#define OLO_PROBE_INPAINT_MATCH_SLOPE 0.5
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
layout(binding = 15) uniform samplerCubeArray u_ProbeDistanceArray; // radial d0 / d1 / d2 + max-mips; mip 0 read at texel centres (#1804)

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

// Mip-0 face resolution of the distance array (kProbeDistanceResolution).
#define OLO_PROBE_DISTANCE_RESOLUTION 128

// The direction through the centre of the mip-0 texel covering `d`: GL's
// major-axis face selection (DirectionToCubeFaceTexel), the texel snapped to
// its centre, and the direction rebuilt. A sample there returns that texel
// whatever the sampler's filter, so the march reads one direction's layers
// exactly as the CPU reference's SampleLayersNearest does (#1804): a bilinear
// blend across a silhouette builds a solid shell where no surface is.
vec3 oloProbeTexelCentre(vec3 d)
{
    vec3 a = abs(d);
    float sc;
    float tc;
    float ma;
    int face;
    if (a.x >= a.y && a.x >= a.z)
    {
        ma = a.x;
        face = d.x >= 0.0 ? 0 : 1;
        sc = d.x >= 0.0 ? -d.z : d.z;
        tc = -d.y;
    }
    else if (a.y >= a.z)
    {
        ma = a.y;
        face = d.y >= 0.0 ? 2 : 3;
        sc = d.x;
        tc = d.y >= 0.0 ? d.z : -d.z;
    }
    else
    {
        ma = a.z;
        face = d.z >= 0.0 ? 4 : 5;
        sc = d.z >= 0.0 ? d.x : -d.x;
        tc = -d.y;
    }
    float res = float(OLO_PROBE_DISTANCE_RESOLUTION);
    vec2 uv = 0.5 * (vec2(sc, tc) / max(ma, 1.0e-20) + 1.0);
    vec2 st = ((clamp(floor(uv * res), 0.0, res - 1.0) + 0.5) / res) * 2.0 - 1.0;
    if (face == 0) return vec3(1.0, -st.y, -st.x);
    if (face == 1) return vec3(-1.0, -st.y, st.x);
    if (face == 2) return vec3(st.x, 1.0, st.y);
    if (face == 3) return vec3(st.x, -1.0, -st.y);
    if (face == 4) return vec3(st.x, -st.y, 1.0);
    return vec3(-st.x, -st.y, -1.0);
}

vec3 oloProbeLayers(vec3 d, float layer)
{
    return textureLod(u_ProbeDistanceArray, vec4(oloProbeTexelCentre(d), layer), 0.0).rgb;
}

// Classification of a ray point at radius r along a texel's three layers
// (d0, d1, d2 — the encoding contract, #1804). Mirrors ClassifyProbeRayPoint.
#define OLO_PROBE_KIND_FRONT 0
#define OLO_PROBE_KIND_GAP 1
#define OLO_PROBE_KIND_SOLID 2

int oloProbeClassifyLayers(float r, vec3 layers)
{
    float bias = max(OLO_PROBE_INSIDE_BIAS_ABS, r * OLO_PROBE_INSIDE_BIAS_REL);
    if (r < layers.x + bias)
    {
        return OLO_PROBE_KIND_FRONT;
    }
    if (r > layers.y + bias && r < layers.z + bias)
    {
        return OLO_PROBE_KIND_GAP;
    }
    return OLO_PROBE_KIND_SOLID;
}

int oloProbeClassify(vec3 originProbeSpace, vec3 dir, float t, float layer)
{
    vec3 p = originProbeSpace + dir * t;
    float r = length(p);
    if (r <= 0.0)
    {
        return OLO_PROBE_KIND_FRONT; // the ray passes through the probe centre
    }
    return oloProbeClassifyLayers(r, oloProbeLayers(p, layer));
}

// Raymarch one probe's layered distance field. Mirrors the CPU reference
// (RaymarchProbeDistanceField): a ray point is solid in [d0, d1] or beyond
// d2 of its direction, free in front of d0 or in the d1..d2 gap. March t over
// (0, dMax + |p0|], bracket the first free->solid transition (searching for
// a thin occluder between free samples of different kinds), bisect, and
// return the hit direction and the radiance lookup direction: the last
// point seen in front of d0 for a hit the probe cannot see. Returns false on
// a miss (the ray leaves through sky, or hits a hidden surface without ever
// having seen the probe's side of its occluder).
bool oloProbeRaymarch(vec3 originProbeSpace, vec3 dir, float dMax, float layer, out vec3 hitDirection,
                      out vec3 radianceDirection)
{
    hitDirection = vec3(0.0);
    radianceDirection = vec3(0.0);
    float tMax = (dMax + length(originProbeSpace)) * OLO_PROBE_MARCH_SLACK_REL + OLO_PROBE_MARCH_SLACK_ABS;
    float stepT = tMax / float(OLO_PROBE_MARCH_ITERATIONS * OLO_PROBE_MARCH_TAPS);

    // A solid sample only brackets a crossing after the march has SEEN a
    // free one. A shading point the probe cannot see, and no gap explains,
    // starts inside an occluder's radial cone, and accepting the first solid
    // sample there reports a bogus hit ON the occluder's shell — leading
    // solid samples are skipped instead (mirrors the CPU reference).
    float tFree = -1.0;
    int freeKind = OLO_PROBE_KIND_FRONT;
    float tVisible = -1.0;
    float tSolid = -1.0;
    {
        int first = oloProbeClassify(originProbeSpace, dir, stepT, layer);
        if (first != OLO_PROBE_KIND_SOLID)
        {
            tFree = 0.0; // the t = 0 on-surface contract holds
            freeKind = first;
            if (first == OLO_PROBE_KIND_FRONT)
            {
                tVisible = 0.0;
            }
        }
    }
    for (int it = 0; it < OLO_PROBE_MARCH_ITERATIONS && tSolid < 0.0; ++it)
    {
        for (int tap = 0; tap < OLO_PROBE_MARCH_TAPS && tSolid < 0.0; ++tap)
        {
            float t = stepT * float(it * OLO_PROBE_MARCH_TAPS + tap + 1);
            int kind = oloProbeClassify(originProbeSpace, dir, t, layer);
            if (kind == OLO_PROBE_KIND_SOLID)
            {
                if (tFree >= 0.0)
                {
                    tSolid = t;
                }
                continue;
            }
            if (tFree >= 0.0 && kind != freeKind)
            {
                // In front of d0 on one side, in the gap on the other: the
                // ray crossed an occluder's silhouette or went through a
                // thin one a coarse step jumped over.
                float lo = tFree;
                float hi = t;
                for (int j = 0; j < OLO_PROBE_GAP_REFINE_STEPS; ++j)
                {
                    float mid = 0.5 * (lo + hi);
                    int midKind = oloProbeClassify(originProbeSpace, dir, mid, layer);
                    if (midKind == OLO_PROBE_KIND_SOLID)
                    {
                        tSolid = mid;
                        break;
                    }
                    if (midKind == freeKind)
                    {
                        lo = mid;
                    }
                    else
                    {
                        hi = mid;
                    }
                }
                if (tSolid >= 0.0)
                {
                    tFree = lo;
                    break;
                }
                if (freeKind == OLO_PROBE_KIND_FRONT)
                {
                    tVisible = lo; // the edge of the shadow the ray enters
                }
            }
            tFree = t;
            freeKind = kind;
            if (kind == OLO_PROBE_KIND_FRONT)
            {
                tVisible = t;
            }
        }
    }
    if (tSolid < 0.0)
    {
        return false;
    }

    for (int i = 0; i < OLO_PROBE_REFINE_STEPS; ++i)
    {
        float tMid = 0.5 * (tFree + tSolid);
        int midKind = oloProbeClassify(originProbeSpace, dir, tMid, layer);
        if (midKind == OLO_PROBE_KIND_SOLID)
        {
            tSolid = tMid;
        }
        else
        {
            tFree = tMid;
            if (midKind == OLO_PROBE_KIND_FRONT)
            {
                tVisible = tMid;
            }
        }
    }

    vec3 hitPoint = originProbeSpace + dir * (0.5 * (tFree + tSolid));
    float hitRadius = length(hitPoint);
    if (hitRadius <= 0.0)
    {
        return false;
    }
    hitDirection = hitPoint / hitRadius;
    vec3 layers = oloProbeLayers(hitDirection, layer);

    // Past the middle of a gap the hit is on the d2 surface, hidden from the
    // probe; otherwise it is on the d0 shell (or the occluder's far side).
    bool occluded = layers.z > layers.y && hitRadius > 0.5 * (layers.y + layers.z);

    // A crossing whose stored distance is the miss sentinel is the ray
    // leaving through SKY, not a surface hit — report a miss so the caller
    // falls back to the live global sky (mirrors the CPU reference).
    if ((occluded ? layers.z : layers.x) >= OLO_PROBE_MISS_THRESHOLD)
    {
        return false;
    }

    radianceDirection = hitDirection;
    bool inpainted = false;
    if (occluded)
    {
        // The hit surface itself, just outside the occluder: the nearest ring
        // direction whose front distance matches the hit (mirrors the CPU
        // reference).
        vec3 axis = abs(hitDirection.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tangent = normalize(cross(hitDirection, axis));
        vec3 bitangent = cross(hitDirection, tangent);
        for (int ring = 0; ring < OLO_PROBE_INPAINT_RING_COUNT && !inpainted; ++ring)
        {
            float angle = OLO_PROBE_INPAINT_FIRST_RING * exp2(float(ring));
            // An oblique wall's distance changes with the angle off the hit.
            float tolerance =
                max(OLO_PROBE_INPAINT_MATCH_ABS, hitRadius * (OLO_PROBE_INPAINT_MATCH_REL + OLO_PROBE_INPAINT_MATCH_SLOPE * angle));
            for (int k = 0; k < OLO_PROBE_INPAINT_AZIMUTHS; ++k)
            {
                float phi = float(k) * (6.28318530718 / float(OLO_PROBE_INPAINT_AZIMUTHS));
                vec3 candidate = hitDirection * cos(angle) + (tangent * cos(phi) + bitangent * sin(phi)) * sin(angle);
                if (abs(oloProbeLayers(candidate, layer).x - hitRadius) <= tolerance)
                {
                    radianceDirection = candidate;
                    inpainted = true;
                    break;
                }
            }
        }
    }
    if (occluded && !inpainted)
    {
        if (tVisible < 0.0)
        {
            return false; // never saw the probe's side of the occluder: nothing to show
        }
        vec3 visiblePoint = originProbeSpace + dir * tVisible;
        float visibleRadius = length(visiblePoint);
        if (visibleRadius <= 0.0)
        {
            return false;
        }
        // Step the lookup a fixed angle back across the occluder's silhouette
        // (mirrors the CPU reference): the visible point is one bisection step
        // from it, inside the radiance cube's filter footprint.
        vec3 visibleDirection = visiblePoint / visibleRadius;
        radianceDirection = visibleDirection;
        vec3 ahead = originProbeSpace + dir * (tVisible + stepT / 64.0);
        float aheadRadius = length(ahead);
        if (aheadRadius > 0.0)
        {
            vec3 drift = ahead / aheadRadius - visibleDirection;
            float driftLength = length(drift);
            if (driftLength > 1.0e-7)
            {
                radianceDirection = normalize(visibleDirection - drift * (OLO_PROBE_INPAINT_MARGIN / driftLength));
            }
        }
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
        // d2 (.b), not d0: a receiver in an occluder's gap (a wall behind a
        // pillar) is past d0 but in front of d2, and the march handles it
        // (#1804). d2 = d1 >= d0 where there is no gap, so this stays an upper
        // bound and can only admit more.
        float coneMax = textureLod(u_ProbeDistanceArray, vec4(toPoint, probe.Params.w), OLO_PROBE_REJECT_MIP).b;
        if (pointDistance > coneMax * OLO_PROBE_REJECT_REL_MARGIN + OLO_PROBE_REJECT_ABS_MARGIN)
        {
            continue;
        }

        vec3 origin = (worldPos + N * OLO_PROBE_NORMAL_OFFSET) - probe.PositionRadius.xyz;
        vec3 hitDirection;
        vec3 radianceDirection;
        if (!oloProbeRaymarch(origin, R, probe.Params.z, probe.Params.w, hitDirection, radianceDirection))
        {
            continue; // miss — this probe defers to the sky fallback
        }

        vec3 radiance = textureLod(u_ProbeRadianceArray, vec4(radianceDirection, probe.Params.w), radianceLod).rgb;
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
