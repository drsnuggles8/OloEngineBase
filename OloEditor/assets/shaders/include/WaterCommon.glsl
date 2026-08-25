// =============================================================================
// WaterCommon.glsl - Gerstner wave displacement and normal computation
// Reusable include for water surface vertex animation
// =============================================================================

// Lightweight hash-based 2D noise for domain warping (vertex stage).
// Breaks spatial coherence between wave octaves so they don't share
// a common origin, eliminating the visible grid/diamond pattern.
float waveHash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float waveNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); // smoothstep interpolation
    float a = waveHash(i);
    float b = waveHash(i + vec2(1.0, 0.0));
    float c = waveHash(i + vec2(0.0, 1.0));
    float d = waveHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Domain warp: returns a 2D offset for a given world position and octave seed.
// The offset magnitude scales with wavelength so large waves warp more.
vec2 domainWarp(vec2 pos, float seed, float wavelength)
{
    float scale = 0.04 / max(wavelength, 0.1); // lower freq → less warp per unit
    float warpAmount = wavelength * 0.3; // warp magnitude proportional to wavelength
    float nx = waveNoise(pos * scale + vec2(seed, seed * 1.7));
    float ny = waveNoise(pos * scale + vec2(seed * 2.3, seed * 0.5));
    return (vec2(nx, ny) - 0.5) * warpAmount;
}

// Single Gerstner wave displacement
// @param position World-space vertex position (xz plane)
// @param direction Normalized wave propagation direction (2D)
// @param steepness Wave steepness (Q parameter, 0..1)
// @param wavelength Distance between wave crests
// @param time Current animation time
// @param phase Phase offset in radians (breaks up regularity)
// @return Displacement vector to add to the vertex position
vec3 gerstnerWave(vec3 position, vec2 direction, float steepness, float wavelength, float time, float phase)
{
    float k = 2.0 * 3.14159265 / max(wavelength, 0.001);
    float c = sqrt(9.81 / k); // phase speed from dispersion relation
    float d = dot(direction, position.xz);
    float f = k * (d - c * time) + phase;
    float a = steepness / k; // amplitude derived from steepness

    return vec3(
        direction.x * a * cos(f),
        a * sin(f),
        direction.y * a * cos(f)
    );
}

// Convenience overload: zero phase (for artist-controlled primary waves)
vec3 gerstnerWave(vec3 position, vec2 direction, float steepness, float wavelength, float time)
{
    return gerstnerWave(position, direction, steepness, wavelength, time, 0.0);
}

// Analytical normal for a single Gerstner wave (with phase offset)
void gerstnerWaveNormal(vec3 position, vec2 direction, float steepness, float wavelength, float time,
                        float phase, inout vec3 tangent, inout vec3 binormal)
{
    float k = 2.0 * 3.14159265 / max(wavelength, 0.001);
    float c = sqrt(9.81 / k);
    float d = dot(direction, position.xz);
    float f = k * (d - c * time) + phase;
    float a = steepness / k;

    tangent += vec3(
        -direction.x * direction.x * steepness * sin(f),
        direction.x * steepness * cos(f),
        -direction.x * direction.y * steepness * sin(f)
    );

    binormal += vec3(
        -direction.x * direction.y * steepness * sin(f),
        direction.y * steepness * cos(f),
        -direction.y * direction.y * steepness * sin(f)
    );
}

// Convenience overload: zero phase
void gerstnerWaveNormal(vec3 position, vec2 direction, float steepness, float wavelength, float time,
                        inout vec3 tangent, inout vec3 binormal)
{
    gerstnerWaveNormal(position, direction, steepness, wavelength, time, 0.0, tangent, binormal);
}

// How much of an octave the mesh can actually carry, from its wavelength
// against the surface's vertex spacing (issue #943).
//
// A displaced grid can only represent a wave it samples often enough. At two
// samples per wavelength (Nyquist) the "wave" is a zigzag between adjacent
// vertices — not a smaller wave, but flat facets whose size tracks the grid.
// The ladder below runs down to 0.09 * avgWL, about 2 m at Drift's wavelengths,
// against a 2.5 m grid: those last octaves cost vertex work and contribute
// nothing a displaced grid can actually show.
//
// Smooth GEOMETRY needs rather more than Nyquist, so this fades an octave out
// between ~6 and ~3 vertices per wavelength rather than cutting at 2. Returning
// a weight (not a hard cull) matters: a step would pop an octave in and out as
// the spacing changed, and the fade keeps the sum continuous.
//
// SCOPE, measured rather than assumed: this is NOT what produced the hard-edged
// patches in #943 — those were the detail NORMAL MAP aliasing (see the
// detailBlend fade in Water.glsl). Band-limiting here changed that frame very
// little, because the octaves it removes carry the smallest amplitude weights
// (0.1 - 0.22) while everything that dominates the surface sits at 5-11 samples
// per wavelength and is kept. It is worth having on its own terms; it is not a
// fix for that artifact, and reaching for it as one will disappoint.
//
// A spacing of 0 (nothing supplied) disables the mechanism and returns 1, so a
// caller that has not been taught to pass it behaves exactly as before.
float octaveMeshWeight(float wavelength, float vertexSpacing)
{
    if (vertexSpacing <= 0.0)
        return 1.0;
    float samplesPerWave = wavelength / vertexSpacing;
    return smoothstep(3.0, 6.0, samplesPerWave);
}

// Sum Gerstner wave octaves for realistic water surface animation.
// Two artist-controlled primary waves plus six procedural detail
// octaves with golden-angle direction distribution and independent
// wavelength ratios.  The broader angular spread and extra octaves
// produce a much more natural, "random" ocean surface.
//
// @param position  Original world-space vertex position
// @param time      Current animation time
// @param waveDir0  xy = direction0, z = steepness0, w = wavelength0
// @param waveDir1  xy = direction1, z = steepness1, w = wavelength1
// @param waveFrequency  Global frequency multiplier
// @param waveAmplitude  Global amplitude multiplier
// @param vertexSpacing  Surface mesh vertex spacing in metres; band-limits the
//                       octave ladder. 0 disables that (every octave at weight 1).
// @param normal    Output: computed surface normal
// @return          Displaced world-space position
vec3 sumGerstnerWaves(vec3 position, float time,
                      vec4 waveDir0, vec4 waveDir1,
                      float waveFrequency, float waveAmplitude,
                      float vertexSpacing,
                      out vec3 normal)
{
    vec3 displaced = position;
    vec3 tangent = vec3(1.0, 0.0, 0.0);
    vec3 binormal = vec3(0.0, 0.0, 1.0);

    // Normalize directions
    vec2 dir0 = normalize(waveDir0.xy + vec2(0.0001));
    vec2 dir1 = normalize(waveDir1.xy + vec2(0.0001));

    float wl0 = max(waveDir0.w, 0.1) / max(waveFrequency, 0.01);
    float wl1 = max(waveDir1.w, 0.1) / max(waveFrequency, 0.01);

    // EVERY octave below scales its displacement AND its normal by the same
    // `waveAmplitude * <weight>` factor, and that pairing is load-bearing
    // (issue #943). gerstnerWave() derives its amplitude as `steepness / k` and
    // gerstnerWaveNormal()'s tangent/binormal terms are each a bare multiple of
    // `steepness` — both are LINEAR in steepness, and the normal's phase does
    // not involve it at all. So an octave displaced by `waveAmplitude * w`
    // describes a wave of steepness `steepness * waveAmplitude * w`, and that
    // is the steepness its normal has to be built from.
    //
    // Passing the raw steepness to the normal (as this did) derives the normal
    // for a full-amplitude wave the surface never had: WaveAmplitude stopped
    // affecting shading normals entirely, and the finest octave contributed 10%
    // of the displacement but 100% of the slope. The resulting normal carried
    // far more high-frequency energy than the surface it described, which is
    // what aliased into hard-edged flats on a near-mirror sea.
    //
    // The factor is therefore written out at BOTH call sites rather than hoisted
    // into a local: the pairing is the invariant, and keeping it visible per
    // line is what WaterGerstnerNormalScaleTest pins. Keep them in step.

    // --- Primary waves (artist-controlled) ---
    // Scaled down to 0.55 each so detail octaves can compete and break the grid
    float meshW_wl0 = octaveMeshWeight(wl0, vertexSpacing);
    displaced += gerstnerWave(position, dir0, waveDir0.z, wl0, time) * waveAmplitude * 0.55 * meshW_wl0;
    gerstnerWaveNormal(position, dir0, waveDir0.z * waveAmplitude * 0.55 * meshW_wl0, wl0, time, tangent, binormal);

    float meshW_wl1 = octaveMeshWeight(wl1, vertexSpacing);
    displaced += gerstnerWave(position, dir1, waveDir1.z, wl1, time) * waveAmplitude * 0.55 * meshW_wl1;
    gerstnerWaveNormal(position, dir1, waveDir1.z * waveAmplitude * 0.55 * meshW_wl1, wl1, time, tangent, binormal);

    // --- Procedural detail octaves ---
    // Uses golden angle (~137.5 degrees) for direction distribution,
    // irrational wavelength ratios, random phase offsets per octave.
    // Detail energy must be comparable to primaries to break the grid.

    // Helper: direction from angle in radians
    #define DIR_FROM_ANGLE(a) normalize(vec2(cos(a), sin(a)))

    // Base params: average of artist waves
    float avgWL = (wl0 + wl1) * 0.5;
    float avgSteepness = (waveDir0.z + waveDir1.z) * 0.5;

    // Golden angle in radians = pi * (3 - sqrt(5)) ≈ 2.39996
    const float GOLDEN_ANGLE = 2.39996;

    // Starting angle offset: derived from primary directions to follow wind
    float baseAngle = atan(dir0.y, dir0.x);

    // Phase offsets — irrational multiples so waves never re-align
    const float PI = 3.14159265;

    // Octave 0: long crosswave — nearly as strong as primary
    {
        float angle = baseAngle + GOLDEN_ANGLE * 1.0;
        vec2  d  = DIR_FROM_ANGLE(angle);
        float wl = avgWL * 0.85;
        float st = avgSteepness * 0.5;
        float ph = PI * 1.7231;
        vec3 warpedPos = position; warpedPos.xz += domainWarp(position.xz, 1.0, wl);
        float meshW = octaveMeshWeight(wl, vertexSpacing);
        displaced += gerstnerWave(warpedPos, d, st, wl, time * 1.03, ph) * waveAmplitude * 0.5 * meshW;
        gerstnerWaveNormal(warpedPos, d, st * waveAmplitude * 0.5 * meshW, wl, time * 1.03, ph, tangent, binormal);
    }

    // Octave 1: medium crosswave
    {
        float angle = baseAngle + GOLDEN_ANGLE * 2.0;
        vec2  d  = DIR_FROM_ANGLE(angle);
        float wl = avgWL * 0.6;
        float st = avgSteepness * 0.45;
        float ph = PI * 3.4519;
        vec3 warpedPos = position; warpedPos.xz += domainWarp(position.xz, 2.7, wl);
        float meshW = octaveMeshWeight(wl, vertexSpacing);
        displaced += gerstnerWave(warpedPos, d, st, wl, time * 0.97, ph) * waveAmplitude * 0.4 * meshW;
        gerstnerWaveNormal(warpedPos, d, st * waveAmplitude * 0.4 * meshW, wl, time * 0.97, ph, tangent, binormal);
    }

    // Octave 2: medium-short chop
    {
        float angle = baseAngle + GOLDEN_ANGLE * 3.0;
        vec2  d  = DIR_FROM_ANGLE(angle);
        float wl = avgWL * 0.4;
        float st = avgSteepness * 0.38;
        float ph = PI * 0.8637;
        vec3 warpedPos = position; warpedPos.xz += domainWarp(position.xz, 4.1, wl);
        float meshW = octaveMeshWeight(wl, vertexSpacing);
        displaced += gerstnerWave(warpedPos, d, st, wl, time * 1.11, ph) * waveAmplitude * 0.3 * meshW;
        gerstnerWaveNormal(warpedPos, d, st * waveAmplitude * 0.3 * meshW, wl, time * 1.11, ph, tangent, binormal);
    }

    // Octave 3: choppy detail
    {
        float angle = baseAngle + GOLDEN_ANGLE * 4.0;
        vec2  d  = DIR_FROM_ANGLE(angle);
        float wl = avgWL * 0.25;
        float st = avgSteepness * 0.3;
        float ph = PI * 5.1043;
        vec3 warpedPos = position; warpedPos.xz += domainWarp(position.xz, 5.9, wl);
        float meshW = octaveMeshWeight(wl, vertexSpacing);
        displaced += gerstnerWave(warpedPos, d, st, wl, time * 0.89, ph) * waveAmplitude * 0.22 * meshW;
        gerstnerWaveNormal(warpedPos, d, st * waveAmplitude * 0.22 * meshW, wl, time * 0.89, ph, tangent, binormal);
    }

    // Octave 4: fine ripple
    {
        float angle = baseAngle + GOLDEN_ANGLE * 5.0;
        vec2  d  = DIR_FROM_ANGLE(angle);
        float wl = avgWL * 0.15;
        float st = avgSteepness * 0.22;
        float ph = PI * 2.6891;
        vec3 warpedPos = position; warpedPos.xz += domainWarp(position.xz, 7.3, wl);
        float meshW = octaveMeshWeight(wl, vertexSpacing);
        displaced += gerstnerWave(warpedPos, d, st, wl, time * 1.23, ph) * waveAmplitude * 0.15 * meshW;
        gerstnerWaveNormal(warpedPos, d, st * waveAmplitude * 0.15 * meshW, wl, time * 1.23, ph, tangent, binormal);
    }

    // Octave 5: finest detail
    {
        float angle = baseAngle + GOLDEN_ANGLE * 6.0;
        vec2  d  = DIR_FROM_ANGLE(angle);
        float wl = avgWL * 0.09;
        float st = avgSteepness * 0.15;
        float ph = PI * 4.3127;
        vec3 warpedPos = position; warpedPos.xz += domainWarp(position.xz, 9.1, wl);
        float meshW = octaveMeshWeight(wl, vertexSpacing);
        displaced += gerstnerWave(warpedPos, d, st, wl, time * 1.07, ph) * waveAmplitude * 0.1 * meshW;
        gerstnerWaveNormal(warpedPos, d, st * waveAmplitude * 0.1 * meshW, wl, time * 1.07, ph, tangent, binormal);
    }

    #undef DIR_FROM_ANGLE

    normal = normalize(cross(binormal, tangent));
    return displaced;
}
