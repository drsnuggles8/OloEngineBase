// =============================================================================
// WaterCommon.glsl - Gerstner wave displacement and normal computation
// Reusable include for water surface vertex animation
// =============================================================================

// Single Gerstner wave displacement
// @param position World-space vertex position (xz plane)
// @param direction Normalized wave propagation direction (2D)
// @param steepness Wave steepness (Q parameter, 0..1)
// @param wavelength Distance between wave crests
// @param time Current animation time
// @return Displacement vector to add to the vertex position
vec3 gerstnerWave(vec3 position, vec2 direction, float steepness, float wavelength, float time)
{
    float k = 2.0 * 3.14159265 / max(wavelength, 0.001);
    float c = sqrt(9.81 / k); // phase speed from dispersion relation
    float d = dot(direction, position.xz);
    float f = k * (d - c * time);
    float a = steepness / k; // amplitude derived from steepness

    return vec3(
        direction.x * a * cos(f),
        a * sin(f),
        direction.y * a * cos(f)
    );
}

// Analytical normal for a single Gerstner wave
// Returns the partial derivatives used to construct the displaced normal
void gerstnerWaveNormal(vec3 position, vec2 direction, float steepness, float wavelength, float time,
                        inout vec3 tangent, inout vec3 binormal)
{
    float k = 2.0 * 3.14159265 / max(wavelength, 0.001);
    float c = sqrt(9.81 / k);
    float d = dot(direction, position.xz);
    float f = k * (d - c * time);
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

// Sum Gerstner wave octaves for realistic water surface animation.
// Two artist-controlled primary waves plus four procedural detail
// octaves with irrational wavelength ratios and rotated propagation
// directions.  This breaks up the uniform tiling that plagues
// two-wave-only Gerstner implementations.
//
// @param position  Original world-space vertex position
// @param time      Current animation time
// @param waveDir0  xy = direction0, z = steepness0, w = wavelength0
// @param waveDir1  xy = direction1, z = steepness1, w = wavelength1
// @param waveFrequency  Global frequency multiplier
// @param waveAmplitude  Global amplitude multiplier
// @param normal    Output: computed surface normal
// @return          Displaced world-space position
vec3 sumGerstnerWaves(vec3 position, float time,
                      vec4 waveDir0, vec4 waveDir1,
                      float waveFrequency, float waveAmplitude,
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

    // --- Primary waves (artist-controlled) ---
    displaced += gerstnerWave(position, dir0, waveDir0.z, wl0, time) * waveAmplitude;
    gerstnerWaveNormal(position, dir0, waveDir0.z, wl0, time, tangent, binormal);

    displaced += gerstnerWave(position, dir1, waveDir1.z, wl1, time) * waveAmplitude;
    gerstnerWaveNormal(position, dir1, waveDir1.z, wl1, time, tangent, binormal);

    // --- Procedural detail octaves ---
    // Irrational wavelength divisors and non-aligned rotation angles
    // prevent the pattern from repeating visibly across the surface.

    // Helper: rotate a 2D direction by angle (radians)
    #define ROTATE2(d, a) vec2(d.x * cos(a) - d.y * sin(a), d.x * sin(a) + d.y * cos(a))

    // Detail 0: derived from primary wave 0
    {
        float wl = wl0 / 2.27;
        float st = waveDir0.z * 0.6;   // softer steepness
        vec2  d  = normalize(ROTATE2(dir0, 0.6457));  // ~37 degrees
        displaced += gerstnerWave(position, d, st, wl, time) * waveAmplitude * 0.5;
        gerstnerWaveNormal(position, d, st, wl, time, tangent, binormal);
    }

    // Detail 1: derived from primary wave 1
    {
        float wl = wl1 / 3.13;
        float st = waveDir1.z * 0.45;
        vec2  d  = normalize(ROTATE2(dir1, -0.9250)); // ~-53 degrees
        displaced += gerstnerWave(position, d, st, wl, time) * waveAmplitude * 0.35;
        gerstnerWaveNormal(position, d, st, wl, time, tangent, binormal);
    }

    // Detail 2: high-frequency chop from wave 0
    {
        float wl = wl0 / 5.19;
        float st = waveDir0.z * 0.3;
        vec2  d  = normalize(ROTATE2(dir0, 2.2863));  // ~131 degrees
        displaced += gerstnerWave(position, d, st, wl, time) * waveAmplitude * 0.2;
        gerstnerWaveNormal(position, d, st, wl, time, tangent, binormal);
    }

    // Detail 3: high-frequency chop from wave 1
    {
        float wl = wl1 / 7.43;
        float st = waveDir1.z * 0.2;
        vec2  d  = normalize(ROTATE2(dir1, -1.6930)); // ~-97 degrees
        displaced += gerstnerWave(position, d, st, wl, time) * waveAmplitude * 0.15;
        gerstnerWaveNormal(position, d, st, wl, time, tangent, binormal);
    }

    #undef ROTATE2

    normal = normalize(cross(binormal, tangent));
    return displaced;
}
