// =============================================================================
// WaterRainCommon.glsl — the GLSL half of the rain-impact ripple contract
// (issue #1034, §7.3).
//
// GLSL twin of OloEngine/src/OloEngine/Renderer/Water/WaterRainRipples.h. That
// header is the source of truth; read its comment block before changing
// anything here. The rules it states and this file must honour:
//
//   * the cell grid is anchored at the WORLD ORIGIN, so the stipple never
//     slides with the camera, the mesh or its tessellation level;
//   * the hash is INTEGER (lowbias32), because the cycle index grows without
//     bound with the clock and a sin-based hash degenerates once its argument
//     gets large — a defect that only shows after minutes of play;
//   * `strength <= 0` returns BEFORE the neighbourhood walk. That early-out is
//     the "no cost when rain is off" acceptance criterion, and strength comes
//     from a uniform, so the branch is uniform.
//
// Nothing here takes a derivative, so every function is safe inside
// non-quad-uniform control flow — unlike the smoothstepAA()/fbmNoise() pair in
// Water.glsl, which is the defect docs/agent-rules/water-shading-nyquist.md §2
// is about.
// =============================================================================

#ifndef WATER_RAIN_COMMON_GLSL
#define WATER_RAIN_COMMON_GLSL

// Mirrors WaterRain::kRingLifetimeSeconds / kRingMaxRadiusMetres /
// kRingWidthMetres / kRingCutoffWidths / kJitterOrigin / kJitterExtent.
// Constants rather than uniforms: they are the CONTRACT, not artist knobs, and
// a knob here would be a second place for the two sides to disagree.
const float WATER_RAIN_RING_LIFETIME = 0.9;
const float WATER_RAIN_RING_MAX_RADIUS = 0.22;
// The LENGTH SCALE. waterRainRingProfileSlope is dimensionless, and the
// slope it feeds is divided by a width in metres — without a height in
// metres multiplied back in, the ring is one metre tall and the surface
// slope peaks near 41. See WaterRain::kRingHeightMetres.
const float WATER_RAIN_RING_HEIGHT = 0.0015;
const float WATER_RAIN_RING_WIDTH = 0.045;
const float WATER_RAIN_RING_CUTOFF = 2.5;
const float WATER_RAIN_JITTER_ORIGIN = 0.15;
const float WATER_RAIN_JITTER_EXTENT = 0.70;

// lowbias32 (Chris Wellons). Mirrors WaterRain::HashU32 — u32 wrapping
// arithmetic, so this is bit-identical to the C++ side.
uint waterRainHashU32(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

// Mirrors WaterRain::HashCell.
uint waterRainHashCell(ivec2 cell, int cycle, uint stream)
{
    uint h = waterRainHashU32(uint(cell.x) * 0x9e3779b9u);
    h = waterRainHashU32(h ^ (uint(cell.y) * 0x85ebca6bu));
    h = waterRainHashU32(h ^ (uint(cycle) * 0xc2b2ae35u));
    return waterRainHashU32(h ^ (stream * 0x27d4eb2fu));
}

// Mirrors WaterRain::UnitFromHash. 24 bits — exactly an f32 mantissa, so the
// conversion is lossless on both sides.
float waterRainUnit(uint h)
{
    return float(h >> 8u) * (1.0 / 16777216.0);
}

// Mirrors WaterRain::RingRadius / RingWidth / RingAmplitude.
float waterRainRingRadius(float age)
{
    return WATER_RAIN_RING_MAX_RADIUS * age;
}
float waterRainRingWidth(float age)
{
    return WATER_RAIN_RING_WIDTH * (1.0 + 2.0 * age);
}
float waterRainRingAmplitude(float age)
{
    return (1.0 - age) * min(age * 6.0, 1.0);
}

// d/dx of h(x) = sin(pi x) * exp(-x^2). Mirrors WaterRain::RingProfileSlope.
float waterRainRingProfileSlope(float x)
{
    if (abs(x) >= WATER_RAIN_RING_CUTOFF)
        return 0.0;
    float gauss = exp(-x * x);
    return (3.14159265 * cos(3.14159265 * x) - 2.0 * x * sin(3.14159265 * x)) * gauss;
}

// Surface slope (dh/dWorldX, dh/dWorldZ) from rain ripples at ABSOLUTE world
// XZ. Mirrors WaterRain::RippleSlope, expression for expression.
//
// `absoluteWorldXZ` must be absolute, i.e. `v_WorldPos.xz + u_RenderOrigin.xz`
// under camera-relative rendering (issue #429) — the same requirement, and the
// same failure mode, as waterDisturbanceFieldUV: passing the camera-relative
// position makes the whole stipple crawl once the render origin rebases, which
// survives any short test.
//
// `params` = (strength, density, cellSizeMetres, unused). strength <= 0 is the
// disabled state; there is no separate enable flag, so a frame in which rain
// stopped cannot leave a stale stipple showing.
vec2 waterRainRippleSlope(vec2 absoluteWorldXZ, float timeSeconds, vec4 params)
{
    if (params.x <= 0.0)
        return vec2(0.0);

    float cellSize = max(params.z, 1e-3);
    vec2 gridPos = absoluteWorldXZ / cellSize;
    ivec2 baseCell = ivec2(floor(gridPos));
    float density = clamp(params.y, 0.0, 1.0);
    float invLifetime = 1.0 / WATER_RAIN_RING_LIFETIME;

    vec2 slope = vec2(0.0);
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            ivec2 cell = baseCell + ivec2(dx, dz);

            // Per-cell phase offset, so neighbouring cells do not all fire on
            // the same beat.
            float phase = waterRainUnit(waterRainHashCell(cell, 0, 0u));
            float cycleT = timeSeconds * invLifetime + phase;
            float cycleFloor = floor(cycleT);
            int cycle = int(cycleFloor);
            float age = cycleT - cycleFloor;

            // Existence is re-rolled every cycle, so light rain is sparse in
            // TIME as well as in space.
            if (waterRainUnit(waterRainHashCell(cell, cycle, 3u)) >= density)
                continue;

            vec2 jitter = vec2(waterRainUnit(waterRainHashCell(cell, cycle, 1u)),
                               waterRainUnit(waterRainHashCell(cell, cycle, 2u)));
            vec2 centre = (vec2(cell) + WATER_RAIN_JITTER_ORIGIN +
                           WATER_RAIN_JITTER_EXTENT * jitter) * cellSize;

            vec2 delta = absoluteWorldXZ - centre;
            float dist = sqrt(dot(delta, delta));
            if (dist < 1e-6)
                continue; // at the exact centre the radial direction is undefined

            float width = waterRainRingWidth(age);
            float x = (dist - waterRainRingRadius(age)) / width;
            float profile = waterRainRingProfileSlope(x);
            if (profile == 0.0)
                continue;

            // metres x envelope x gain, over a width in metres — a slope.
            float amplitude = WATER_RAIN_RING_HEIGHT * waterRainRingAmplitude(age) * params.x;
            slope += (amplitude * profile / width) * (delta / dist);
        }
    }
    return slope;
}

// Add a surface slope to an existing normal.
//
// SLOPES add, normals do not — the same fact OceanFFTField::SampleCascades
// records when it sums Slope rather than Normal across cascades. For a normal
// n with n.y > 0 the surface slope is (-n.x/n.y, -n.z/n.y); adding `slope` to
// that and rebuilding gives (n.x - slope.x*n.y, n.y, n.z - slope.y*n.y), with
// the division cancelled out so an almost-horizontal normal cannot blow up.
vec3 waterRainApplySlope(vec3 n, vec2 slope)
{
    vec3 perturbed = vec3(n.x - slope.x * n.y, n.y, n.z - slope.y * n.y);
    float len = length(perturbed);
    return (len > 1e-6) ? (perturbed / len) : n;
}

#endif // WATER_RAIN_COMMON_GLSL
