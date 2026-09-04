// =============================================================================
// WaterShoreCommon.glsl — shore wave deformation: shoaling, refraction and
// breaking against a seabed depth field (issue #1033).
//
// GPU twin of OloEngine/src/OloEngine/Renderer/Water/WaterShoreDepth.h. That
// header carries the CONTRACT — where the depth comes from, what the lattice
// is, why frequency and not wavenumber is the conserved quantity, and why
// breaking reads the Jacobian the FFT foam already reads rather than a second
// steepness heuristic. Read it before changing anything here; this file is
// deliberately only the arithmetic, in the same order, with the same constants.
//
// Included by WaterCommon.glsl, so it is in scope for every stage that
// displaces the surface — the colour pass AND the surface-depth capture, which
// replay one shared chain (WaterVertexStage.glsl's header comment). There is no
// way to shoal in one and not the other.
//
// Sampling is behind a hook so this file needs no sampler of its own: each
// stage declares its own u_ShoreDepth and defines waterShoreFetchDepth() after
// it, exactly as the FFT cascades do with oceanCascadeFetchDisplacement().
// =============================================================================

#ifndef WATER_SHORE_COMMON_GLSL
#define WATER_SHORE_COMMON_GLSL

// WaterShoreDepth.h :: kGravity / kDeepSentinelMetres / kMinDepthMetres /
// kBreakerIndex / kMaxSteepness / kDispersionIterations. Mirrored values — a
// divergence here is a CPU/GPU surface split, not a look change.
const float WATER_SHORE_GRAVITY = 9.81;
const float WATER_SHORE_DEEP_SENTINEL = 1000.0;
const float WATER_SHORE_MIN_DEPTH = 0.05;
const float WATER_SHORE_BREAKER_INDEX = 0.39;
const float WATER_SHORE_MAX_STEEPNESS = 0.9;
const float WATER_SHORE_MIN_REFRACTION = 0.35;
const int WATER_SHORE_DISPERSION_ITERATIONS = 4;

// Provided by each stage after its own sampler declaration.
// Returns the raw RGBA texel: r = depth (m), gb = d(depth)/d(worldXZ).
vec4 waterShoreFetchDepth(vec2 uv);

// What the surface knows about the seabed under one point.
struct WaterShoreSample
{
    float Depth;      // metres of water; WATER_SHORE_DEEP_SENTINEL where unknown
    vec2 Gradient;    // d(depth)/d(worldXZ) — points toward DEEPER water
    float Enabled;    // 0 disables every transform below
};

// The disabled state, spelled once. Every relation in this file returns its
// deep-water form at this sample, so a scene with no shore field renders the
// pre-#1033 surface — that is a structural property, not a tolerance.
WaterShoreSample waterShoreDisabled()
{
    WaterShoreSample s;
    s.Depth = WATER_SHORE_DEEP_SENTINEL;
    s.Gradient = vec2(0.0);
    s.Enabled = 0.0;
    return s;
}

// Sample the baked field at an ABSOLUTE world XZ (issue #429 — the water stages
// evaluate wave phase in absolute space, and this field is anchored there too).
//
//   shoreParams.xy = window centre (world XZ)
//   shoreParams.z  = 1 / window extent in metres
//   shoreParams.w  = enable (<= 0 disables; there is no separate flag, so a
//                    frame whose bake did not run cannot show a stale field)
WaterShoreSample waterShoreSample(vec2 worldXZ, vec4 shoreParams)
{
    if (shoreParams.w <= 0.0)
        return waterShoreDisabled();

    vec2 uv = (worldXZ - shoreParams.xy) * shoreParams.z + 0.5;
    // Outside the window is open sea. Reporting the sentinel rather than
    // relying on CLAMP_TO_EDGE keeps the edge behaviour independent of the
    // sampler state, and of whatever the last row of the bake happened to hold.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return waterShoreDisabled();

    vec4 texel = waterShoreFetchDepth(uv);
    WaterShoreSample s;
    s.Depth = clamp(texel.r, WATER_SHORE_MIN_DEPTH, WATER_SHORE_DEEP_SENTINEL);
    s.Gradient = texel.gb;
    s.Enabled = 1.0;
    return s;
}

// WaterShoreDepth.h :: LocalWavenumber. k solving w^2 = g k tanh(k h), from the
// deep-water k0 (w^2 = g k0), by Newton from an Eckart seed on the dimensionless
// form (kh) tanh(kh) = x.
//
// Read the header's comment before touching this. The short version: the
// obvious fixed point k <- k0/tanh(kh) OSCILLATES in shallow water rather than
// converging, and returns a plausible wrong wavelength in exactly the water this
// feature is about. The iteration count is part of the CPU/GPU contract, not a
// quality knob — the two sides must run the same number.
float waterShoreLocalWavenumber(float k0, float depth)
{
    float h = max(depth, WATER_SHORE_MIN_DEPTH);
    float x = max(k0 * h, 1e-8);

    float kh = x / max(sqrt(tanh(x)), 1e-6); // Eckart seed, within ~5%
    for (int i = 0; i < WATER_SHORE_DISPERSION_ITERATIONS; ++i)
    {
        float t = tanh(kh);
        float f = kh * t - x;
        float df = t + kh * (1.0 - t * t);
        kh -= f / max(df, 1e-6);
    }
    return max(kh, 1e-6) / h;
}

// WaterShoreDepth.h :: GroupVelocityRatio. c_g / c: 0.5 deep, 1 shallow.
float waterShoreGroupVelocityRatio(float k, float depth)
{
    float kh = k * max(depth, WATER_SHORE_MIN_DEPTH);
    if (kh > 10.0)
        return 0.5; // sinh(2kh) overflows long before the term stops mattering
    float twoKh = 2.0 * kh;
    return 0.5 * (1.0 + twoKh / sinh(twoKh));
}

// WaterShoreDepth.h :: ShoalingCoefficient. Green's law: a * c_g is conserved.
float waterShoreShoalingCoefficient(float k0, float k, float depth)
{
    float cg0 = 0.5 / max(k0, 1e-6);
    float cg = waterShoreGroupVelocityRatio(k, depth) / max(k, 1e-6);
    return sqrt(max(cg0 / max(cg, 1e-9), 0.0));
}

// WaterShoreDepth.h :: Octave — one deep-water octave transformed for its depth.
struct WaterShoreOctave
{
    vec2 Direction;    // heading after refraction
    float Wavelength;  // 2*pi/k here — shorter than the deep-water one
    float Steepness;   // Q = a k after shoaling, refraction and the breaker clamp
    float PhaseSpeed;  // c = w/k. NOT sqrt(g/k) once the depth bites.
    float Breaking;    // 0 offshore; the fraction of amplitude the breaker limit removed
};

// WaterShoreDepth.h :: TransformOctave. Read that comment for WHY each step is
// the step it is; this is its arithmetic, in the same order.
//
// `amplitudeScale` is everything the CALLER multiplies this octave's
// displacement by afterwards (waveAmplitude * octave weight * mesh weight). It
// is passed in because the breaker limit is a statement about METRES of water,
// and testing it against an amplitude 15x larger than the rendered one puts the
// surf zone 15x too far offshore. The returned Steepness is still the PRE-scale
// value, so call sites keep multiplying by the factor they always did.
WaterShoreOctave waterShoreTransformOctave(vec2 deepDir, float deepWavelength, float deepSteepness,
                                           float amplitudeScale, WaterShoreSample shore,
                                           float breakerIndex)
{
    const float kTwoPi = 6.28318530718;
    float wl0 = max(deepWavelength, 0.001);
    float k0 = kTwoPi / wl0;
    float c0 = sqrt(WATER_SHORE_GRAVITY / k0);

    WaterShoreOctave o;
    o.Direction = deepDir;
    o.Wavelength = wl0;
    o.Steepness = deepSteepness;
    o.PhaseSpeed = c0;
    o.Breaking = 0.0;

    if (shore.Enabled <= 0.0)
        return o;

    float h = clamp(shore.Depth, WATER_SHORE_MIN_DEPTH, WATER_SHORE_DEEP_SENTINEL);
    float k = waterShoreLocalWavenumber(k0, h);
    float omega = sqrt(WATER_SHORE_GRAVITY * k0); // conserved along the ray
    float c = omega / max(k, 1e-6);

    o.Wavelength = kTwoPi / max(k, 1e-6);
    o.PhaseSpeed = c;

    // --- Snell's law (WaterShoreDepth.h :: Refract) --------------------------
    float kr = 1.0;
    float nLen2 = dot(shore.Gradient, shore.Gradient);
    if (nLen2 > 1e-8)
    {
        vec2 s = shore.Gradient * inversesqrt(nLen2); // toward deeper water
        vec2 t = vec2(-s.y, s.x);                     // along the depth contour

        float sinDeep = dot(deepDir, t);
        float cosDeep = dot(deepDir, s);

        // c <= c0, so |sinLocal| <= |sinDeep|: the ray turns TOWARD the normal,
        // never away, which is what makes waves arrive parallel to the beach.
        float sinLocal = clamp(sinDeep * min(c / c0, 1.0), -1.0, 1.0);
        float cosMag = sqrt(max(1.0 - sinLocal * sinLocal, 0.0));
        float cosLocal = (cosDeep < 0.0) ? -cosMag : cosMag;

        o.Direction = t * sinLocal + s * cosLocal;
        // Floored at tangency — WaterShoreDepth.h :: Refract explains why the
        // bare ray-tube term deletes the waves on an island's flanks, and why a
        // threshold guard would be worse than the floor.
        kr = max(sqrt(max(abs(cosDeep), 0.0) / max(cosMag, 1e-3)),
                 WATER_SHORE_MIN_REFRACTION);
    }

    // --- Amplitude, then the breaker limit -----------------------------------
    float scale = max(amplitudeScale, 1e-6);
    float a0 = (deepSteepness / k0) * scale;
    float aGrown = a0 * waterShoreShoalingCoefficient(k0, k, h) * kr;
    float aLimit = max(breakerIndex, 1e-4) * h;
    float aFinal = aGrown;
    if (aGrown > aLimit)
    {
        aFinal = aLimit;
        o.Breaking = clamp((aGrown - aLimit) / max(aGrown, 1e-6), 0.0, 1.0);
    }

    // Back out of the caller's scale, then guard the Gerstner fold: a single
    // octave at Q >= 1 self-intersects whatever the depth says.
    float steepness = (aFinal / scale) * k;
    if (steepness * scale > WATER_SHORE_MAX_STEEPNESS)
    {
        steepness = WATER_SHORE_MAX_STEEPNESS / scale;
        o.Breaking = 1.0;
    }
    o.Steepness = steepness;
    return o;
}

#endif // WATER_SHORE_COMMON_GLSL
