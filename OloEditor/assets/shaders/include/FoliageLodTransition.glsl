// FoliageLodTransition.glsl — GLSL twin of
// OloEngine/src/OloEngine/Terrain/Foliage/FoliageLodTransition.h (issue #1237).
//
// Pure functions over their arguments: no uniform blocks, no varyings, no
// stage assumptions — the same contract FoliageInstanceGeometry.glsl beside it
// keeps, and for the same reason. EVERY consumer calls these: the beauty and
// G-Buffer vertex stage, the impostor vertex stage, the shadow depth stage and
// FoliageInstanceCull.comp. A stage that decided its own thinning would drop a
// plant from the colour pass and keep its shadow, which reads downstream as a
// shadow with nothing casting it.
//
// The C++ header carries the full rationale — why the hash is keyed on
// position, why the spread is centred, why the compensation inverts the
// EFFECTIVE keep fraction rather than the raw one, and what the anti-
// oscillation guarantee is and is not. Keep the two in step; the contract test
// (tests/Rendering/FoliageLodTransitionContractTest.cpp) evaluates the C++
// side and the evidence captures are what catch a drift here.

#ifndef OLO_FOLIAGE_LOD_TRANSITION_GLSL
#define OLO_FOLIAGE_LOD_TRANSITION_GLSL

// Twin of FoliageLod::kMinKeepFraction.
#define OLO_FOLIAGE_MIN_KEEP_FRACTION (1.0 / 256.0)

// Deterministic per-instance draw in [0, 1) from the plant's TERRAIN-LOCAL
// position — the same lane (a_PositionScale.xyz) the cull kernel reads, so a
// compacted row and its source row hash identically.
float foliageLodInstanceHash(vec3 terrainLocalPos)
{
    uint qx = uint(int(floor(terrainLocalPos.x * 1000.0)));
    uint qy = uint(int(floor(terrainLocalPos.y * 1000.0)));
    uint qz = uint(int(floor(terrainLocalPos.z * 1000.0)));

    uint h = qx * 0x8DA6B343u ^ qy * 0xD8163841u ^ qz * 0xCB1AB31Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return float(h >> 8) * (1.0 / 16777216.0);
}

// This instance's own threshold for a transition authored at `nominal`.
// `spread` decorrelates the layer's ring into a band (mean is still
// `nominal`); `hysteresisOffset` is the world-unit shift from
// foliageLodHysteresisOffset, one value for every edge of a band.
float foliageLodTransitionDistance(float nominal, float offset01, float spread, float hysteresisOffset)
{
    return max(nominal + spread * (offset01 - 0.5) + hysteresisOffset, 0.0);
}

// The hysteresis shift in WORLD UNITS: outward while the viewer retreats,
// inward while it approaches. Absolute rather than a per-edge factor, so the
// band slides bodily instead of changing width — see the C++ twin.
float foliageLodHysteresisOffset(float bandStart, bool receding, float hysteresis)
{
    float magnitude = max(bandStart, 0.0) * hysteresis;
    return receding ? magnitude : -magnitude;
}

// Fraction of the layer still drawn at `dist`: 1 up to `start`, smoothly down
// to `minFraction` at `end`, flat beyond.
float foliageDensityKeepFraction(float dist, float start, float end, float minFraction)
{
    float floorFraction = clamp(minFraction, OLO_FOLIAGE_MIN_KEEP_FRACTION, 1.0);
    if (!(end > start))
        return (dist >= start) ? floorFraction : 1.0;
    return mix(1.0, floorFraction, smoothstep(start, end, dist));
}

float foliageDensityFadeWidth(float fadeFraction)
{
    return clamp(fadeFraction, 0.0, 1.0);
}

// 1 while this plant's hash is under the keep threshold, ramping to 0 across
// the fade band ABOVE it, 0 beyond. The ramp sits above rather than below the
// threshold so a keep fraction of 1 leaves the whole layer opaque — see the
// C++ twin for the failure the other way round.
float foliageDensityInstanceAlpha(float hash01, float keepFraction, float fadeFraction)
{
    if (hash01 < keepFraction)
        return 1.0;
    float width = foliageDensityFadeWidth(fadeFraction);
    if (width <= 0.0)
        return 0.0;
    return clamp(1.0 - (hash01 - keepFraction) / width, 0.0, 1.0);
}

// The fraction the layer EFFECTIVELY covers, counting a partially-faded plant
// at its fade value: the integral of the alpha above over a uniform hash.
float foliageEffectiveKeepFraction(float keepFraction, float fadeFraction)
{
    float k = clamp(keepFraction, 0.0, 1.0);
    float width = foliageDensityFadeWidth(fadeFraction);
    if (width <= 0.0)
        return max(k, OLO_FOLIAGE_MIN_KEEP_FRACTION);
    float u = min(1.0 - k, width);
    return max(k + u - (u * u) / (2.0 * width), OLO_FOLIAGE_MIN_KEEP_FRACTION);
}

// The linear growth that keeps a thinned layer's apparent coverage constant:
// coverage is a sum of AREAS, so `effectiveKeep * compensation^2 == 1`.
float foliageCoverageCompensation(float keepFraction, float fadeFraction, float maxScale)
{
    float effective = foliageEffectiveKeepFraction(keepFraction, fadeFraction);
    return clamp(inversesqrt(effective), 1.0, max(maxScale, 1.0));
}

// ── The packed authored parameters ───────────────────────────────────────────
//
// Two UBO lanes rather than eight floats through every call:
//   p0 = (enabled, start, end, minFraction)
//   p1 = (fadeFraction, maxScale, transitionSpread, hysteresis)
// Order and meaning mirror ShaderBindingLayout::FoliageUBO::LodTransition0/1
// exactly. `enabled` is a float because the block is std140 floats; 0 is the
// off switch and the default, and with it the two functions below return the
// identity at every distance.

// p0.x is a BITFIELD carried as a float (a small exact integer), not a bool:
//   1 — coverage-preserving density reduction is authored on
//   2 — the alpha-less passes (G-Buffer, shadow depth) resolve a partial fade
//       stochastically instead of with a hard alpha cut-off
// They are separate switches because they answer separate criteria and a layer
// may want either alone: a layer with no density LOD still benefits from its
// far fade dissolving rather than ending on a line, and a layer that thins may
// be forward-only, where alpha blending already dissolves it.
bool foliageDensityEnabled(vec4 p0)
{
    return (int(p0.x + 0.5) & 1) != 0;
}

bool foliageStochasticCoverage(vec4 p0)
{
    return (int(p0.x + 0.5) & 2) != 0;
}

// Per-instance density alpha at `dist`. 1 when the feature is off.
float foliageDensityAlphaAt(vec4 p0, vec4 p1, float hash01, float dist)
{
    if (!foliageDensityEnabled(p0))
        return 1.0;
    float keep = foliageDensityKeepFraction(dist, p0.y, p0.z, p0.w);
    return foliageDensityInstanceAlpha(hash01, keep, p1.x);
}

// Per-instance coverage compensation at `dist`. 1 when the feature is off.
float foliageDensityScaleAt(vec4 p0, vec4 p1, float dist)
{
    if (!foliageDensityEnabled(p0))
        return 1.0;
    float keep = foliageDensityKeepFraction(dist, p0.y, p0.z, p0.w);
    return foliageCoverageCompensation(keep, p1.x, p1.y);
}

float foliageLodSpread(vec4 p1)
{
    return p1.z;
}

float foliageLodHysteresis(vec4 p1)
{
    return p1.w;
}

#endif // OLO_FOLIAGE_LOD_TRANSITION_GLSL
