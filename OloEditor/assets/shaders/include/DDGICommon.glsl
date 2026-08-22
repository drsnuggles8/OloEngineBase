// =============================================================================
// DDGICommon.glsl — realtime DDGI probe math + visibility-weighted sampler
// (issues #632 / #707, docs/adr/0007-ddgi-hit-point-cache-gather.md)
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
//
// ISSUE #707 — this file now describes a CASCADED probe field. A cascade is a
// probe LATTICE anchored at a fixed world origin, of which a Dims-sized window
// is stored TOROIDALLY; cascade N has twice cascade N-1's spacing and covers
// twice its extent. An AUTHORED single volume is the same structure with
// cascade count 1, lattice origin = BoundsMin, lattice min = 0 and blend band
// 0, which reproduces the pre-#707 behaviour and atlas layout exactly.
// =============================================================================

#ifndef DDGI_COMMON_GLSL
#define DDGI_COMMON_GLSL

// Mirrors DDGI::kMaxCascades. The UBO arrays below are sized by it, so it
// cannot be raised on one side alone.
#define DDGI_MAX_CASCADES 8

// DDGI probe volume UBO (binding 51) — must match
// ShaderBindingLayout::DDGIVolumeUBO (512 bytes) exactly.
layout(std140, binding = 51) uniform DDGIVolume {
    vec4  u_DDGIBoundsMin;          // xyz = cascade 0 window min (render-origin-relative)
    vec4  u_DDGIBoundsMax;          // xyz = cascade 0 window max
    ivec4 u_DDGIGridDimensions;     // xyz = probe counts PER CASCADE, w = total probes (all cascades)
    vec4  u_DDGIProbeSpacing;       // xyz = cascade 0 spacing, w = cascade 0 min axial spacing
    int   u_DDGIEnabled;
    float u_DDGIIntensity;
    float u_DDGIHysteresis;
    float u_DDGISelfShadowBias;
    int   u_DDGIHitCacheTexels;
    int   u_DDGIFrameIndex;
    float u_DDGIHybridBlend;        // 0 = baked SH only .. 1 = DDGI only
    float u_DDGIEnergyConservation; // bounce-feedback albedo clamp
    float u_DDGIMaxRayDistance;     // cascade 0 visibility clamp = 1.5 * |spacing|
    float u_DDGIBounceMarginScale;  // bounce-path margin, in probe spacings (#751)
    int   u_DDGICascadeCount;       // >= 1; 1 == the authored single-volume path
    float u_DDGICascadeBlendBand;   // fraction of the half-extent; 0 == hard bounds (#707)
    int   u_DDGIUpdateRateDivisor;  // 1 / 2 / 8 / 16 / 32 / 64 (#707)
    int   u_DDGIRequestLifetime;    // frames a request keeps a probe live (#707)
    int   u_DDGISparsityEnabled;    // 0 = every probe is live (#707)
    int   _ddgiPad0;
    // Per-cascade lattice description. Index 0 is the finest cascade.
    vec4  u_DDGICascadeOrigin[DDGI_MAX_CASCADES];  // xyz = world pos of lattice (0,0,0), w = max ray distance
    vec4  u_DDGICascadeSpacing[DDGI_MAX_CASCADES]; // xyz = per-axis spacing, w = min axial spacing
    ivec4 u_DDGICascadeLattice[DDGI_MAX_CASCADES]; // xyz = lattice coord stored at the window's low corner
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
//
// The un-cascaded helpers below address WITHIN one cascade (storage
// coordinates), exactly as they did before #707; the cascaded siblings add the
// level. With one cascade the two are identical.
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

// Mirrors DDGI::ProbesPerCascade.
int ddgiProbesPerCascade()
{
    return max(u_DDGIGridDimensions.x, 1) * max(u_DDGIGridDimensions.y, 1) * max(u_DDGIGridDimensions.z, 1);
}

// Mirrors DDGI::CascadedProbeIndex.
int ddgiCascadedProbeIndex(int level, ivec3 storage)
{
    return level * ddgiProbesPerCascade() + ddgiProbeLinearIndex(storage);
}

// Mirrors DDGI::CascadeOfProbeIndex.
int ddgiCascadeOfProbeIndex(int globalIndex)
{
    return globalIndex / ddgiProbesPerCascade();
}

// Mirrors DDGI::StorageCoordOfProbeIndex.
ivec3 ddgiStorageCoordOfProbeIndex(int globalIndex)
{
    return ddgiProbeGridCoord(globalIndex % ddgiProbesPerCascade());
}

// Mirrors DDGI::CascadedProbeTileCoord. Column = level * DimX + x, row = z * DimY + y.
ivec2 ddgiCascadedProbeTileCoord(int level, ivec3 storage)
{
    return ivec2(level * max(u_DDGIGridDimensions.x, 1) + storage.x,
                 storage.z * max(u_DDGIGridDimensions.y, 1) + storage.y);
}

// Mirrors DDGI::ProbeTileCoord for the single-cascade case, and the global
// (level-carrying) index form otherwise.
ivec2 ddgiProbeTileCoord(int globalIndex)
{
    return ddgiCascadedProbeTileCoord(ddgiCascadeOfProbeIndex(globalIndex),
                                      ddgiStorageCoordOfProbeIndex(globalIndex));
}

// Border-safe continuous UV for bilinear-sampling `direction` from a TILE.
// Mirrors DDGI::ProbeAtlasTexel, then normalizes by the atlas size.
vec2 ddgiTileAtlasUV(ivec2 tile, vec3 direction, int interiorResolution, vec2 atlasTexelSize)
{
    int tileTexels = interiorResolution + 2;
    vec2 tileOrigin = vec2(tile * tileTexels);
    vec2 uv01 = ddgiOctEncode(direction) * 0.5 + 0.5;
    vec2 texel = tileOrigin + 1.0 + uv01 * float(interiorResolution);
    return texel / atlasTexelSize;
}

// Global-index convenience form (kept for callers that only carry an index).
vec2 ddgiProbeAtlasUV(int globalIndex, vec3 direction, int interiorResolution, vec2 atlasTexelSize)
{
    return ddgiTileAtlasUV(ddgiProbeTileCoord(globalIndex), direction, interiorResolution, atlasTexelSize);
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
// Cascade lattices (issue #707). Mirrors DDGI::WrapIndex /
// StorageCoordForLattice / LatticeForStorageCoord / CascadeProbe*Position /
// CascadeBounds* / WorldToCascadeLattice / CascadeInteriorWeight.
// -----------------------------------------------------------------------------

// Mirrors DDGI::WrapIndex. GLSL's % truncates toward zero exactly like C++'s,
// so a negative lattice coordinate would map to a NEGATIVE storage index —
// and cascades go negative as soon as the camera moves toward -x. This is the
// single most load-bearing line in the toroidal scheme.
int ddgiWrapIndex(int value, int modulus)
{
    int m = max(modulus, 1);
    int r = value % m;
    return (r < 0) ? (r + m) : r;
}

// Mirrors DDGI::StorageCoordForLattice.
ivec3 ddgiStorageCoordForLattice(ivec3 lattice)
{
    return ivec3(ddgiWrapIndex(lattice.x, u_DDGIGridDimensions.x),
                 ddgiWrapIndex(lattice.y, u_DDGIGridDimensions.y),
                 ddgiWrapIndex(lattice.z, u_DDGIGridDimensions.z));
}

// Mirrors DDGI::LatticeForStorageCoord.
ivec3 ddgiLatticeForStorageCoord(ivec3 storage, int level)
{
    ivec3 dims = max(u_DDGIGridDimensions.xyz, ivec3(1));
    ivec3 latMin = u_DDGICascadeLattice[level].xyz;
    return ivec3(latMin.x + ddgiWrapIndex(storage.x - latMin.x, dims.x),
                 latMin.y + ddgiWrapIndex(storage.y - latMin.y, dims.y),
                 latMin.z + ddgiWrapIndex(storage.z - latMin.z, dims.z));
}

// Mirrors DDGI::CascadeProbeGridPosition (render-origin-relative, because the
// UBO's origin already is).
vec3 ddgiCascadeProbeGridPosition(ivec3 storage, int level)
{
    return u_DDGICascadeOrigin[level].xyz
         + vec3(ddgiLatticeForStorageCoord(storage, level)) * u_DDGICascadeSpacing[level].xyz;
}

// Mirrors DDGI::CascadeProbeWorldPosition.
vec3 ddgiCascadeProbeWorldPosition(ivec3 storage, int level, vec3 offsetNormalized)
{
    return ddgiCascadeProbeGridPosition(storage, level) + offsetNormalized * u_DDGICascadeSpacing[level].xyz;
}

// Mirrors DDGI::CascadeBoundsMin / CascadeBoundsMax.
vec3 ddgiCascadeBoundsMin(int level)
{
    return u_DDGICascadeOrigin[level].xyz + vec3(u_DDGICascadeLattice[level].xyz) * u_DDGICascadeSpacing[level].xyz;
}

vec3 ddgiCascadeBoundsMax(int level)
{
    ivec3 dims = max(u_DDGIGridDimensions.xyz - ivec3(1), ivec3(0));
    return ddgiCascadeBoundsMin(level) + vec3(dims) * u_DDGICascadeSpacing[level].xyz;
}

// -----------------------------------------------------------------------------
// Legacy single-volume forms.
//
// NOTHING IN THE SHIPPED SHADERS CALLS THESE any more — since #707 the passes
// go through the ddgiCascade* siblings above, which reduce to exactly these for
// a single authored cascade (lattice origin = BoundsMin, lattice min = 0). They
// are kept for two reasons, and only those two:
//
//   * they are the documented one-for-one GLSL mirrors of DDGI::ProbeGridPosition
//     / ProbeWorldPosition / IsInsideVolume / VolumeWeight / SelfShadowBias in
//     DDGICommon.h, all of which ARE live on the C++ side (LightProbeBaker, the
//     editor gizmo, the L1 contract tests and DDGIReferenceParityTest);
//   * they document, in one place, what "the authored path is unchanged" means
//     in terms a reader can check against the cascaded code above.
//
// A dead GLSL function costs nothing at runtime (the compiler drops it). If you
// remove them, remove the mirror claim in this file's header at the same time.
// -----------------------------------------------------------------------------

vec3 ddgiProbeGridPosition(ivec3 coord)
{
    return u_DDGIBoundsMin.xyz + u_DDGIProbeSpacing.xyz * vec3(coord);
}

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

// Mirrors DDGI::VolumeWeight, generalized to explicit bounds so each cascade
// can be tested against its own window. Smooth membership of `worldPos` in the
// box grown by a per-axis `margin`: exactly 1 inside, smoothstep down to 0
// across the margin band, exactly 0 beyond it.
//
// margin == 0 reproduces the hard inside-test EXACTLY (1 inside, 0 outside,
// boundary inclusive) — which is what keeps the lit-pass sampler unchanged
// while the bounce path (issue #751) passes a real margin.
float ddgiVolumeWeightBounds(vec3 worldPos, vec3 boundsMin, vec3 boundsMax, vec3 margin)
{
    vec3 outside = max(max(boundsMin - worldPos, worldPos - boundsMax), vec3(0.0));
    if (dot(outside, outside) <= 0.0)
    {
        return 1.0;
    }
    vec3 safeMargin = max(margin, vec3(0.0));
    // Beyond the margin on ANY axis -> no contribution. This also covers
    // margin == 0, so the division below never divides a positive distance by
    // zero.
    if (any(greaterThan(outside, safeMargin)))
    {
        return 0.0;
    }
    float t = clamp(length(outside / max(safeMargin, vec3(1e-20))), 0.0, 1.0);
    float w = 1.0 - t * t * (3.0 - 2.0 * t);
    // Returned through a POSITIVE test so a non-finite worldPos reads as
    // "outside": every comparison above is false for NaN, so it reaches here
    // with w = NaN, and `w > 0.0` is false. The old hard inside-test had that
    // property for free (`all(greaterThanEqual(NaN, ...))` is false); losing
    // it would push NaN irradiance through the lit passes, which their
    // `dot(ddgi, ddgi) <= 0.0` fallback does not catch either.
    return (w > 0.0) ? w : 0.0;
}

// Cascade-0 form, kept for source compatibility.
float ddgiVolumeWeight(vec3 worldPos, vec3 margin)
{
    return ddgiVolumeWeightBounds(worldPos, u_DDGIBoundsMin.xyz, u_DDGIBoundsMax.xyz, margin);
}

// Mirrors DDGI::CascadeInteriorWeight, with the bounce margin folded in.
//
// `marginScale` grows the window by that many probe spacings BEFORE the band
// ramp is applied. That ordering matters: a bounce hit point sits on a surface
// just outside the window, and testing it against the ungrown window would
// reject it before the margin could ever apply — which is exactly the #751
// failure, re-introduced by cascades if the two are evaluated in the wrong
// order.
//
// bandFraction <= 0 falls through to ddgiVolumeWeightBounds, i.e. the hard
// bounds test the authored single-volume path has always used.
float ddgiCascadeWeight(vec3 worldPos, int level, float bandFraction, float marginScale)
{
    vec3 spacing = u_DDGICascadeSpacing[level].xyz;
    vec3 margin = spacing * max(marginScale, 0.0);
    vec3 boundsMin = ddgiCascadeBoundsMin(level);
    vec3 boundsMax = ddgiCascadeBoundsMax(level);
    if (bandFraction <= 0.0)
    {
        return ddgiVolumeWeightBounds(worldPos, boundsMin, boundsMax, margin);
    }
    boundsMin -= margin;
    boundsMax += margin;
    vec3 centre = (boundsMin + boundsMax) * 0.5;
    vec3 halfExtent = max((boundsMax - boundsMin) * 0.5, vec3(1e-6));
    vec3 n = abs(worldPos - centre) / halfExtent;
    float t = max(max(n.x, n.y), n.z);
    float bandStart = clamp(1.0 - bandFraction, 0.0, 0.999);
    if (t <= bandStart)
    {
        return 1.0;
    }
    if (t >= 1.0)
    {
        return 0.0;
    }
    float u = (t - bandStart) / (1.0 - bandStart);
    return 1.0 - (u * u * (3.0 - 2.0 * u));
}

// Mirrors DDGI::BounceMargin's role: the margin the INFINITE-BOUNCE feedback
// path samples with, expressed as a SCALE now that each cascade has its own
// spacing. One probe spacing is exactly the distance over which the gather's
// implicit clamp to the boundary probe layer is justified by the field's own
// sample density.
float ddgiBounceMarginScale()
{
    return u_DDGIBounceMarginScale;
}

// -----------------------------------------------------------------------------
// Sparsity + variable update rate (issue #707). Mirrors
// DDGI::ProbeUpdatesThisFrame / IsProbeLive.
// -----------------------------------------------------------------------------

// Mirrors DDGI::ProbeUpdatesThisFrame. Round-robin, not a hash: exactly one
// frame in `divisor` per probe, and adjacent probes update on adjacent frames
// so a probe's neighbours are never all stale at once.
bool ddgiProbeUpdatesThisFrame(int probeIndex, int frameIndex, int divisor)
{
    int d = max(divisor, 1);
    if (d == 1)
    {
        return true;
    }
    return ddgiWrapIndex(probeIndex + (frameIndex % d), d) == 0;
}

// Mirrors DDGI::IsProbeLive. lastRequestFrame == 0 means "never requested",
// which is why the pass's frame counter starts at 1.
bool ddgiIsProbeLive(uint lastRequestFrame, uint frameIndex, uint lifetime)
{
    if (lastRequestFrame == 0u)
    {
        return false;
    }
    return (frameIndex <= lastRequestFrame) || ((frameIndex - lastRequestFrame) <= lifetime);
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

// Mirrors DDGI::SelfShadowBias, parameterized by the cascade's own minimum
// axial spacing (a coarse cascade needs a proportionally larger bias).
vec3 ddgiSelfShadowBiasFor(vec3 normal, vec3 viewDir, float minAxialSpacing)
{
    return (0.2 * normal + 0.8 * viewDir) * (0.75 * minAxialSpacing) * u_DDGISelfShadowBias;
}

vec3 ddgiSelfShadowBias(vec3 normal, vec3 viewDir)
{
    return ddgiSelfShadowBiasFor(normal, viewDir, u_DDGIProbeSpacing.w);
}

// -----------------------------------------------------------------------------
// The visibility-weighted trilinear sampler, per cascade.
//
// Returns irradiance (linear, full E — the blend pass stores pi-normalized
// ratio estimates, so no 2*pi restore factor exists in this pipeline; see
// DDGI_BlendIrradiance). Callers convert to diffuse exitance with albedo/PI.
//
// `outValid` is 0 when every corner probe was rejected (all uncaptured), so the
// caller can fall through to the next cascade instead of blending in a zero.
// The VOLUME/BAND weight is deliberately NOT applied here — the caller owns it,
// because it is what decides how two cascades share a sample.
// -----------------------------------------------------------------------------
vec3 ddgiGatherCascade(sampler2D irradianceAtlas, sampler2D visibilityAtlas, sampler2D probeData,
                       vec3 worldPos, vec3 normal, vec3 viewDir, int level, out float outValid)
{
    outValid = 0.0;

    ivec3 dims = max(u_DDGIGridDimensions.xyz, ivec3(1));
    vec3 spacing = u_DDGICascadeSpacing[level].xyz;
    vec3 origin = u_DDGICascadeOrigin[level].xyz;
    ivec3 latMin = u_DDGICascadeLattice[level].xyz;

    vec2 irradianceAtlasSize = vec2(textureSize(irradianceAtlas, 0));
    vec2 visibilityAtlasSize = vec2(textureSize(visibilityAtlas, 0));

    // Self-shadow bias: probe lookups happen slightly off the surface, toward
    // the viewer, so a probe just behind the wall a texel sits on does not
    // shadow-test against that same wall (JCGT 2021).
    vec3 biasedPos = worldPos + ddgiSelfShadowBiasFor(normal, viewDir, u_DDGICascadeSpacing[level].w);

    vec3 latF = (biasedPos - origin) / max(spacing, vec3(1e-6));
    // Clamp the base cell so both trilinear corners stay inside the stored
    // window. max(dims - 2, 0) keeps a 1-probe axis valid (base == latMin, and
    // the +1 corner clamps back onto it).
    ivec3 base = clamp(ivec3(floor(latF)), latMin, latMin + max(dims - ivec3(2), ivec3(0)));
    vec3 frac = clamp(latF - vec3(base), vec3(0.0), vec3(1.0));

    vec3 sumIrradiance = vec3(0.0);
    float sumWeight = 0.0;

    for (int i = 0; i < 8; ++i)
    {
        ivec3 cornerOffset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 lattice = min(base + cornerOffset, latMin + dims - ivec3(1));
        ivec3 storage = ddgiStorageCoordForLattice(lattice);
        ivec2 tile = ddgiCascadedProbeTileCoord(level, storage);

        vec4 pdata = texelFetch(probeData, tile, 0);
        // Uncaptured probes have no atlas content yet — skip and renormalize.
        // Inactive (in-wall) probes keep participating: their visibility data
        // says "occluder everywhere", so Chebyshev de-weights them naturally.
        if (pdata.w < 0.5)
        {
            continue;
        }

        vec3 probePos = origin + vec3(lattice) * spacing + pdata.xyz * spacing;

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
                              ddgiTileAtlasUV(tile, dirProbeToPoint, DDGI_VISIBILITY_INTERIOR, visibilityAtlasSize),
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
                                     ddgiTileAtlasUV(tile, normal, DDGI_IRRADIANCE_INTERIOR, irradianceAtlasSize),
                                     0.0).rgb;

        sumIrradiance += irradiance * weight;
        sumWeight += weight;
    }

    if (sumWeight < 1e-4)
    {
        return vec3(0.0);
    }
    outValid = 1.0;
    return sumIrradiance / sumWeight;
}

// -----------------------------------------------------------------------------
// The cascaded entry point.
//
// Selects the FINEST cascade that owns `worldPos` and cross-fades it into the
// next coarser one across the blend band. With one cascade and band 0 this is
// bit-identical to the pre-#707 sampler.
//
// TWO PARAMETERS THE LIT PATH DOES NOT USE (issue #751):
//
//   volumeMarginScale — distance past the window, in probe spacings of the
//     cascade being tested, over which the gather still answers, fading
//     smoothly to zero. Zero for the lit pass (a surface outside the field must
//     fall through to the ambient ladder, not be shaded from a clamped boundary
//     probe). One spacing for the infinite-bounce feedback path, whose sample
//     points are hit points ON SURFACES and therefore sit just OUTSIDE any
//     volume fitted to a room's air. A SCALE rather than a vec3 since #707,
//     because each cascade must grow by ITS OWN spacing.
//
//   intensity — the artist gain. u_DDGIIntensity for the lit pass; 1.0 for the
//     feedback path, because a gain inside a feedback loop multiplies its
//     Lipschitz bound (albedo clamp x intensity), and above ~1.11 that bound
//     reaches 1 and stops proving the loop contracts. It does not prove it
//     diverges — the true gain is generally well under the bound — but a
//     stability guarantee an artist knob can revoke is not worth keeping. Kept
//     out, `intensity` scales the converged field linearly instead.
// -----------------------------------------------------------------------------
vec3 ddgiGatherIrradiance(sampler2D irradianceAtlas, sampler2D visibilityAtlas, sampler2D probeData,
                          vec3 worldPos, vec3 normal, vec3 viewDir,
                          float volumeMarginScale, float intensity)
{
    if (u_DDGIEnabled == 0)
    {
        return vec3(0.0);
    }

    int cascadeCount = clamp(u_DDGICascadeCount, 1, DDGI_MAX_CASCADES);
    float band = u_DDGICascadeBlendBand;

    int level = -1;
    float w0 = 0.0;
    for (int i = 0; i < cascadeCount; ++i)
    {
        float w = ddgiCascadeWeight(worldPos, i, band, volumeMarginScale);
        if (w > 0.0)
        {
            level = i;
            w0 = w;
            break;
        }
    }
    if (level < 0)
    {
        return vec3(0.0);
    }

    float valid0 = 0.0;
    vec3 e0 = ddgiGatherCascade(irradianceAtlas, visibilityAtlas, probeData, worldPos, normal, viewDir, level, valid0);

    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    if (valid0 > 0.0)
    {
        sum += e0 * w0;
        wsum += w0;
    }

    // Blend band: the complement of this cascade's weight goes to the next
    // coarser one. Using the complement rather than the coarser cascade's own
    // interior weight is what makes the two a PARTITION of unity — the coarser
    // cascade's weight is 1 here by construction (it contains this one), so the
    // pair would otherwise sum to more than 1 and over-brighten the band.
    if (w0 < 1.0 && (level + 1) < cascadeCount)
    {
        float valid1 = 0.0;
        vec3 e1 = ddgiGatherCascade(irradianceAtlas, visibilityAtlas, probeData, worldPos, normal, viewDir,
                                    level + 1, valid1);
        if (valid1 > 0.0)
        {
            float w1 = 1.0 - w0;
            sum += e1 * w1;
            wsum += w1;
        }
    }

    if (wsum < 1e-4)
    {
        return vec3(0.0);
    }

    // Outermost fade. When there IS a coarser cascade the band already handed
    // the sample over, so no fade applies. When there is not, w0 < 1 means the
    // sample is in the outer band of the whole field (or in the #751 bounce
    // margin), and multiplying by it is exactly what the pre-#707 sampler did
    // with its volume weight.
    float fade = ((level + 1) < cascadeCount) ? 1.0 : w0;
    return (sum / wsum) * (intensity * fade);
}

// The lit passes' entry point — unchanged behaviour: no margin (hard field
// test, so a surface outside the field still falls through to the ambient
// ladder) and the authored intensity.
vec3 ddgiSampleIrradiance(sampler2D irradianceAtlas, sampler2D visibilityAtlas, sampler2D probeData,
                          vec3 worldPos, vec3 normal, vec3 viewDir)
{
    return ddgiGatherIrradiance(irradianceAtlas, visibilityAtlas, probeData, worldPos, normal, viewDir,
                                0.0, u_DDGIIntensity);
}

// -----------------------------------------------------------------------------
// Relocation + classification (issue #707, upgrade 4 — evaluated ON THE GPU by
// compute/DDGI_Relocate.comp, which is what removes the per-frame
// GPU->CPU->GPU stall the issue is about).
//
// Mirrors DDGI::ProbeHitAggregates / ClassifyProbe / RelocateProbeSpring in
// DDGICommon.h one-for-one. The C++ header stays the pinned home of the
// formula; the L1 tests pin it and the shaderpipe parity test pins this file
// against it.
// -----------------------------------------------------------------------------

// Mirrors DDGI::kBackfaceFraction / kMaxProbeOffsetFraction and the spring
// tuning constants. Kept as literals rather than UBO fields on purpose: they
// are algorithm constants, not authored knobs, and a UBO field is a second
// place for them to drift.
const float DDGI_BACKFACE_FRACTION = 0.25;
const float DDGI_MAX_PROBE_OFFSET_FRACTION = 0.45;
const float DDGI_SPRING_STEP_SCALE = 0.35;
const float DDGI_SPRING_GRID_RESTORE_SCALE = 0.25;
const float DDGI_SPRING_FREE_DIRECTION_SCALE = 0.35;
const float DDGI_SPRING_CROWDING_SCALE = 1.0;

// Mirrors DDGI::ProbeHitAggregates.
struct DDGIProbeHitAggregates
{
    float BackfaceFraction;
    vec3  ClosestBackfaceDir;
    float ClosestBackfaceDist;   // < 0 = none
    vec3  ClosestFrontfaceDir;
    float ClosestFrontfaceDist;  // < 0 = none
    vec3  FarthestFrontfaceDir;
    float FarthestFrontfaceDist;
    vec3  FreeDirectionSum;
    float FreeDirectionWeight;
    vec3  CrowdingSum;
    float CrowdingWeight;
};

// Mirrors DDGI::ClassifyProbe. Returns the integer ProbeState.
uint ddgiClassifyProbe(float backfaceFraction)
{
    return (backfaceFraction > DDGI_BACKFACE_FRACTION) ? 2u /* Inactive */ : 1u /* Active */;
}

// Mirrors DDGI::RelocateProbeSpring. See the C++ header for why the strictly-
// inside-geometry escape is kept from the stock RTXGI rule while the two
// non-degenerate cases become a force balance, and why the ellipsoid clamp
// PROJECTS instead of rejecting.
vec3 ddgiRelocateProbeSpring(vec3 currentOffsetN, DDGIProbeHitAggregates agg, vec3 spacing, float minFrontfaceDistance)
{
    vec3 safeSpacing = max(spacing, vec3(1e-6));
    vec3 offsetWorld = currentOffsetN * safeSpacing;

    if (agg.BackfaceFraction > DDGI_BACKFACE_FRACTION && agg.ClosestBackfaceDist >= 0.0)
    {
        offsetWorld += agg.ClosestBackfaceDir * (agg.ClosestBackfaceDist + 0.5 * minFrontfaceDistance);
    }
    else
    {
        vec3 force = vec3(0.0);
        if (agg.CrowdingWeight > 0.0)
        {
            vec3 meanCrowding = agg.CrowdingSum / agg.CrowdingWeight;
            force -= meanCrowding * (DDGI_SPRING_CROWDING_SCALE * minFrontfaceDistance);
        }
        if (agg.FreeDirectionWeight > 0.0)
        {
            vec3 meanFree = agg.FreeDirectionSum / agg.FreeDirectionWeight;
            if (length(meanFree) > 1e-4)
            {
                force += meanFree * (DDGI_SPRING_FREE_DIRECTION_SCALE * minFrontfaceDistance);
            }
        }
        force -= offsetWorld * DDGI_SPRING_GRID_RESTORE_SCALE;
        offsetWorld += force * DDGI_SPRING_STEP_SCALE;
    }

    vec3 offsetN = offsetWorld / safeSpacing;
    float lenSq = dot(offsetN, offsetN);
    if (lenSq < DDGI_MAX_PROBE_OFFSET_FRACTION * DDGI_MAX_PROBE_OFFSET_FRACTION)
    {
        return offsetN;
    }
    if (lenSq <= 0.0)
    {
        return vec3(0.0);
    }
    return offsetN * (DDGI_MAX_PROBE_OFFSET_FRACTION * 0.999 / sqrt(lenSq));
}

// The attenuation the INFINITE-BOUNCE gather would apply at `relPos` — exactly
// the `fade` factor ddgiGatherIrradiance computes, evaluated without touching
// an atlas. This is the #751 bounce-coverage diagnostic, now cascade-aware:
// with one authored cascade it reduces to the old ddgiVolumeWeight against the
// volume grown by one probe spacing, so the number keeps its old meaning.
float ddgiBounceCoverageWeight(vec3 relPos)
{
    int cascadeCount = clamp(u_DDGICascadeCount, 1, DDGI_MAX_CASCADES);
    for (int i = 0; i < cascadeCount; ++i)
    {
        float w = ddgiCascadeWeight(relPos, i, u_DDGICascadeBlendBand, u_DDGIBounceMarginScale);
        if (w > 0.0)
        {
            return ((i + 1) < cascadeCount) ? 1.0 : w;
        }
    }
    return 0.0;
}

// -----------------------------------------------------------------------------
// Engine-global atlas bindings for lit-pass consumers.
// -----------------------------------------------------------------------------
#ifdef DDGI_GLOBAL_SAMPLERS

// Slots mirror ShaderBindingLayout::TEX_DDGI_* — bound once per frame by the
// DDGI update pass to the CURRENT (just-blended) atlases.
// The visibility atlas moved 57 -> 64 (issue #691, ADR item A2):
// 57 is the engine-wide Vulkan vertex-pull SSBO, and DDGI_Capture.glsl both
// pulls its vertices AND includes this block — a real within-shader collision
// on Vulkan's single-set binding model (GL's disjoint namespaces hid it).
layout(binding = 56) uniform sampler2D u_DDGIIrradianceAtlas;
layout(binding = 64) uniform sampler2D u_DDGIVisibilityAtlas;
layout(binding = 58) uniform sampler2D u_DDGIProbeData;

vec3 sampleDDGIIrradiance(vec3 worldPos, vec3 normal, vec3 viewDir)
{
    return ddgiSampleIrradiance(u_DDGIIrradianceAtlas, u_DDGIVisibilityAtlas, u_DDGIProbeData,
                                worldPos, normal, viewDir);
}

#endif // DDGI_GLOBAL_SAMPLERS

#endif // DDGI_COMMON_GLSL
