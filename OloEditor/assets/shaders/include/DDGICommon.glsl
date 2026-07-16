// =============================================================================
// DDGICommon.glsl — realtime DDGI probe math + visibility-weighted sampler
// (issue #632, docs/adr/0006-ddgi-hit-point-cache-gather.md)
//
// Every function here mirrors a C++ counterpart in
// OloEngine/src/OloEngine/Renderer/DDGI/DDGICommon.h one-for-one (the C++ name
// is quoted above each function). The L1 tests pin the C++ header; the
// shaderpipe parity tests pin GLSL == C++. If you change a formula here,
// change the mirror too.
//
// Consumers that want the engine-global atlas bindings (deferred lighting,
// forward PBR) must `#define DDGI_GLOBAL_SAMPLERS` before including this file;
// the DDGI update passes bind their own units and call the sampler-
// parameterized functions directly.
// =============================================================================

#ifndef DDGI_COMMON_GLSL
#define DDGI_COMMON_GLSL

// DDGI probe volume UBO (binding 51) — must match
// ShaderBindingLayout::DDGIVolumeUBO (112 bytes) exactly.
layout(std140, binding = 51) uniform DDGIVolume {
    vec4  u_DDGIBoundsMin;          // xyz = min corner (render-origin-relative)
    vec4  u_DDGIBoundsMax;          // xyz = max corner
    ivec4 u_DDGIGridDimensions;     // xyz = probe counts, w = total
    vec4  u_DDGIProbeSpacing;       // xyz = per-axis spacing, w = min axial spacing
    int   u_DDGIEnabled;
    float u_DDGIIntensity;
    float u_DDGIHysteresis;
    float u_DDGISelfShadowBias;
    int   u_DDGIHitCacheTexels;
    int   u_DDGIFrameIndex;
    float u_DDGIHybridBlend;        // 0 = baked SH only .. 1 = DDGI only
    float u_DDGIEnergyConservation; // bounce-feedback albedo clamp
    float u_DDGIMaxRayDistance;     // visibility clamp = 1.5 * |spacing|
    float _ddgiPad0;
    float _ddgiPad1;
    float _ddgiPad2;
};

// Atlas layout constants — mirror DDGI::kIrradiance*/kVisibility* in DDGICommon.h.
const int DDGI_IRRADIANCE_INTERIOR = 6;
const int DDGI_IRRADIANCE_TILE = 8;
const int DDGI_VISIBILITY_INTERIOR = 14;
const int DDGI_VISIBILITY_TILE = 16;

// Probe states — mirror DDGI::ProbeState.
const float DDGI_PROBE_UNCAPTURED = 0.0;
const float DDGI_PROBE_ACTIVE = 1.0;
const float DDGI_PROBE_INACTIVE = 2.0;

// Hit-cache flags stored in the hit-geo atlas .a channel (capture contract,
// consumed by the resample/relight passes).
const float DDGI_HIT_SKY = 0.0;
const float DDGI_HIT_BACKFACE = 0.5;
const float DDGI_HIT_FRONTFACE = 1.0;

// -----------------------------------------------------------------------------
// Octahedral mapping. Mirrors DDGI::SignNotZero / OctEncode / OctDecode.
// NOTE: uses signNotZero, deliberately unlike the G-buffer octEncode's plain
// sign() — probe texel directions land exactly on fold seams.
// -----------------------------------------------------------------------------

// Mirrors DDGI::SignNotZero.
vec2 ddgiSignNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Mirrors DDGI::OctEncode. Unit direction -> [-1,1]^2.
vec2 ddgiOctEncode(vec3 n)
{
    float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    vec2 uv = n.xy / max(l1, 1e-8);
    if (n.z < 0.0)
    {
        uv = (1.0 - abs(uv.yx)) * ddgiSignNotZero(uv);
    }
    return uv;
}

// Mirrors DDGI::OctDecode. [-1,1]^2 -> unit direction.
vec3 ddgiOctDecode(vec2 f)
{
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Mirrors DDGI::TexelDirection. Center direction of an interior texel.
vec3 ddgiTexelDirection(ivec2 interiorTexel, int interiorResolution)
{
    vec2 uv01 = (vec2(interiorTexel) + 0.5) / float(interiorResolution);
    return ddgiOctDecode(uv01 * 2.0 - 1.0);
}

// -----------------------------------------------------------------------------
// Probe grid / atlas layout. Linear index matches
// LightProbeVolumeComponent::GridIndex (z-major) and LightProbeSampling.glsl.
// -----------------------------------------------------------------------------

// Mirrors DDGI::ProbeLinearIndex.
int ddgiProbeLinearIndex(ivec3 coord)
{
    return coord.z * u_DDGIGridDimensions.y * u_DDGIGridDimensions.x
         + coord.y * u_DDGIGridDimensions.x
         + coord.x;
}

// Mirrors DDGI::ProbeGridCoord.
ivec3 ddgiProbeGridCoord(int linearIndex)
{
    int planeSize = u_DDGIGridDimensions.x * u_DDGIGridDimensions.y;
    return ivec3(linearIndex % u_DDGIGridDimensions.x,
                 (linearIndex / u_DDGIGridDimensions.x) % u_DDGIGridDimensions.y,
                 linearIndex / planeSize);
}

// Mirrors DDGI::ProbeTileCoord. Column = x, row = z * dimY + y.
ivec2 ddgiProbeTileCoord(int linearIndex)
{
    ivec3 c = ddgiProbeGridCoord(linearIndex);
    return ivec2(c.x, c.z * u_DDGIGridDimensions.y + c.y);
}

// Mirrors DDGI::ProbeAtlasTexel, then normalizes by the atlas size.
// Border-safe continuous UV for bilinear-sampling `direction` from a tile.
vec2 ddgiProbeAtlasUV(int linearIndex, vec3 direction, int interiorResolution, vec2 atlasTexelSize)
{
    int tileTexels = interiorResolution + 2;
    vec2 tileOrigin = vec2(ddgiProbeTileCoord(linearIndex) * tileTexels);
    vec2 uv01 = ddgiOctEncode(direction) * 0.5 + 0.5;
    vec2 texel = tileOrigin + 1.0 + uv01 * float(interiorResolution);
    return texel / atlasTexelSize;
}

// Mirrors DDGI::BorderSourceTexel. For a border texel of a tile (local
// coords), the interior texel whose value it copies. Interior texels return
// themselves.
ivec2 ddgiBorderSourceTexel(ivec2 localTexel, int tileTexels)
{
    int maxT = tileTexels - 1;
    bool onLeft = (localTexel.x == 0);
    bool onRight = (localTexel.x == maxT);
    bool onBottom = (localTexel.y == 0);
    bool onTop = (localTexel.y == maxT);

    if ((onLeft || onRight) && (onBottom || onTop))
    {
        return ivec2(onLeft ? (maxT - 1) : 1, onBottom ? (maxT - 1) : 1);
    }
    if (onBottom || onTop)
    {
        return ivec2(maxT - localTexel.x, onBottom ? 1 : (maxT - 1));
    }
    if (onLeft || onRight)
    {
        return ivec2(onLeft ? 1 : (maxT - 1), maxT - localTexel.y);
    }
    return localTexel;
}

// -----------------------------------------------------------------------------
// Probe placement. Mirrors DDGI::ProbeSpacing / ProbeGridPosition /
// ProbeWorldPosition (spacing comes precomputed in the UBO).
// -----------------------------------------------------------------------------

// Mirrors DDGI::ProbeGridPosition (corner-anchored lattice).
vec3 ddgiProbeGridPosition(ivec3 coord)
{
    return u_DDGIBoundsMin.xyz + u_DDGIProbeSpacing.xyz * vec3(coord);
}

// Mirrors DDGI::ProbeWorldPosition (offset stored normalized by spacing).
vec3 ddgiProbeWorldPosition(ivec3 coord, vec3 offsetNormalized)
{
    return ddgiProbeGridPosition(coord) + offsetNormalized * u_DDGIProbeSpacing.xyz;
}

// Continuous grid coordinates of a world position (same mapping as
// LightProbeSampling.glsl::worldToProbeGrid, against the DDGI UBO).
vec3 ddgiWorldToProbeGrid(vec3 worldPos)
{
    return (worldPos - u_DDGIBoundsMin.xyz) / max(u_DDGIProbeSpacing.xyz, vec3(1e-6));
}

bool ddgiIsInsideVolume(vec3 worldPos)
{
    return all(greaterThanEqual(worldPos, u_DDGIBoundsMin.xyz)) &&
           all(lessThanEqual(worldPos, u_DDGIBoundsMax.xyz));
}

// -----------------------------------------------------------------------------
// Sampler weights (the wall-leak fix).
// -----------------------------------------------------------------------------

// Mirrors DDGI::ChebyshevWeight.
float ddgiChebyshevWeight(float mean, float meanSquared, float r)
{
    if (r <= mean)
    {
        return 1.0;
    }
    float variance = max(meanSquared - mean * mean, 1e-6);
    float d = r - mean;
    float p = variance / (variance + d * d);
    return max(p * p * p, 0.05);
}

// Mirrors DDGI::WrapShadingWeight.
float ddgiWrapShadingWeight(vec3 dirToProbe, vec3 normal)
{
    float wrapped = (dot(dirToProbe, normal) + 1.0) * 0.5;
    return wrapped * wrapped + 0.2;
}

// Mirrors DDGI::CrushWeight.
float ddgiCrushWeight(float w)
{
    const float threshold = 0.2;
    if (w < threshold)
    {
        return w * (w * w) / (threshold * threshold);
    }
    return w;
}

// Mirrors DDGI::SelfShadowBias.
vec3 ddgiSelfShadowBias(vec3 normal, vec3 viewDir)
{
    return (0.2 * normal + 0.8 * viewDir) * (0.75 * u_DDGIProbeSpacing.w) * u_DDGISelfShadowBias;
}

// -----------------------------------------------------------------------------
// The visibility-weighted trilinear sampler.
//
// Returns irradiance (linear, full E — the blend pass stores pi-normalized
// ratio estimates, so no 2*pi restore factor exists in this pipeline; see
// DDGIProbeBlend.comp). Callers convert to diffuse exitance with albedo/PI.
// Returns vec3(0) outside the volume or when every corner probe is rejected —
// callers fall back to the existing ambient ladder (baked SH / IBL).
//
// worldPos/normal in (render-origin-relative) world space; viewDir points
// FROM the surface TOWARD the camera.
// -----------------------------------------------------------------------------
vec3 ddgiSampleIrradiance(sampler2D irradianceAtlas, sampler2D visibilityAtlas, sampler2D probeData,
                          vec3 worldPos, vec3 normal, vec3 viewDir)
{
    if (u_DDGIEnabled == 0)
    {
        return vec3(0.0);
    }
    if (!ddgiIsInsideVolume(worldPos))
    {
        return vec3(0.0);
    }

    vec2 irradianceAtlasSize = vec2(textureSize(irradianceAtlas, 0));
    vec2 visibilityAtlasSize = vec2(textureSize(visibilityAtlas, 0));

    // Self-shadow bias: probe lookups happen slightly off the surface, toward
    // the viewer, so a probe just behind the wall a texel sits on does not
    // shadow-test against that same wall (JCGT 2021).
    vec3 biasedPos = worldPos + ddgiSelfShadowBias(normal, viewDir);

    vec3 gridPos = ddgiWorldToProbeGrid(biasedPos);
    ivec3 dims = u_DDGIGridDimensions.xyz;
    ivec3 p0 = clamp(ivec3(floor(gridPos)), ivec3(0), dims - ivec3(1));
    vec3 frac = clamp(gridPos - vec3(p0), vec3(0.0), vec3(1.0));

    vec3 sumIrradiance = vec3(0.0);
    float sumWeight = 0.0;

    for (int i = 0; i < 8; ++i)
    {
        ivec3 cornerOffset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 coord = min(p0 + cornerOffset, dims - ivec3(1));
        int probeIdx = ddgiProbeLinearIndex(coord);

        vec4 pdata = texelFetch(probeData, ddgiProbeTileCoord(probeIdx), 0);
        // Uncaptured probes have no atlas content yet — skip and renormalize.
        // Inactive (in-wall) probes keep participating: their visibility data
        // says "occluder everywhere", so Chebyshev de-weights them naturally.
        if (pdata.w < 0.5)
        {
            continue;
        }

        vec3 probePos = ddgiProbeWorldPosition(coord, pdata.xyz);

        // Backface wrap weight uses the TRUE surface position.
        vec3 dirToProbe = normalize(probePos - worldPos);
        float weight = ddgiWrapShadingWeight(dirToProbe, normal);

        // Chebyshev visibility uses the BIASED position: distance atlas holds
        // mean/mean^2 occluder distance from the probe along the direction
        // toward the sample.
        vec3 probeToBiased = biasedPos - probePos;
        float r = length(probeToBiased);
        vec3 dirProbeToPoint = probeToBiased / max(r, 1e-6);
        vec2 vis = textureLod(visibilityAtlas,
                              ddgiProbeAtlasUV(probeIdx, dirProbeToPoint, DDGI_VISIBILITY_INTERIOR, visibilityAtlasSize),
                              0.0).rg;
        weight *= ddgiChebyshevWeight(vis.r, vis.g, r);

        // Crush before trilinear so a tiny visibility x backface product dies
        // smoothly instead of being renormalized back up (RTXGI order).
        weight = ddgiCrushWeight(max(weight, 1e-6));

        // Trilinear weight, per-axis floored so a sample exactly on a probe
        // plane never zeroes the opposite corner entirely.
        vec3 tri = mix(1.0 - frac, frac, vec3(cornerOffset));
        weight *= max(tri.x, 0.001) * max(tri.y, 0.001) * max(tri.z, 0.001);

        vec3 irradiance = textureLod(irradianceAtlas,
                                     ddgiProbeAtlasUV(probeIdx, normal, DDGI_IRRADIANCE_INTERIOR, irradianceAtlasSize),
                                     0.0).rgb;

        sumIrradiance += irradiance * weight;
        sumWeight += weight;
    }

    if (sumWeight < 1e-4)
    {
        return vec3(0.0);
    }

    return (sumIrradiance / sumWeight) * u_DDGIIntensity;
}

// -----------------------------------------------------------------------------
// Engine-global atlas bindings for lit-pass consumers.
// -----------------------------------------------------------------------------
#ifdef DDGI_GLOBAL_SAMPLERS

// Slots mirror ShaderBindingLayout::TEX_DDGI_* — bound once per frame by the
// DDGI update pass to the CURRENT (just-blended) atlases.
layout(binding = 56) uniform sampler2D u_DDGIIrradianceAtlas;
layout(binding = 57) uniform sampler2D u_DDGIVisibilityAtlas;
layout(binding = 58) uniform sampler2D u_DDGIProbeData;

vec3 sampleDDGIIrradiance(vec3 worldPos, vec3 normal, vec3 viewDir)
{
    return ddgiSampleIrradiance(u_DDGIIrradianceAtlas, u_DDGIVisibilityAtlas, u_DDGIProbeData,
                                worldPos, normal, viewDir);
}

#endif // DDGI_GLOBAL_SAMPLERS

#endif // DDGI_COMMON_GLSL
