// =============================================================================
// NoiseCommon.glsl — Tiling 3D noise primitives for volumetric clouds
//
// Provides:
//   - Inverted Worley (cellular) noise with wrapped cell hashing
//   - Classic Perlin gradient noise with wrapped lattice hashing
//   - Perlin-Worley combination (Schneider / Nubis, Hillaire)
//   - 3-octave Worley FBM (Nubis weights)
//
// TILING CONTRACT: every function samples p in [0, 1)^3 and tiles with
// period 1 in each axis, provided the frequency / cell-count argument is a
// positive integer (each octave doubles it, so powers of two stay integral).
// This lets a repeat-wrapped 3D texture baked from these functions be
// sampled at any world scale without seams.
//
// All names are prefixed or noise-specific so this header can be included
// alongside FogCommon.glsl (which defines its own hash3D / valueNoise3D).
// =============================================================================

#ifndef NOISE_COMMON_GLSL
#define NOISE_COMMON_GLSL

// ---------------------------------------------------------------------------
// Deterministic fract-style hashes (same family as FogCommon.glsl's hash3D,
// renamed to avoid redefinition when both headers are included).
// ---------------------------------------------------------------------------
float noiseHash13(vec3 p)
{
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

vec3 noiseHash33(vec3 p)
{
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yxz + 19.19);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// ---------------------------------------------------------------------------
// Linear range remap: v from [l0, h0] onto [l1, h1] (not clamped).
// ---------------------------------------------------------------------------
float remap(float v, float l0, float h0, float l1, float h1)
{
    return l1 + ((v - l0) / (h0 - l0)) * (h1 - l1);
}

// ---------------------------------------------------------------------------
// Inverted Worley noise — 1 at feature points, falling to 0 at ~1 cell
// distance. Cell hashing is wrapped with mod(cell, cellCount) so the result
// tiles at period 1 when sampling p in [0, 1)^3 (GLSL mod is floored, so
// negative neighbour cells wrap correctly too).
// ---------------------------------------------------------------------------
float worley3D(vec3 p, float cellCount)
{
    vec3 ps = p * cellCount;
    vec3 cell = floor(ps);
    vec3 f = ps - cell;

    float minDistSq = 1.0e10;
    for (int z = -1; z <= 1; ++z)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                vec3 offset = vec3(float(x), float(y), float(z));
                // Wrap the neighbour cell so feature points repeat with
                // period cellCount — this is what makes the noise tile.
                vec3 wrappedCell = mod(cell + offset, cellCount);
                vec3 featurePoint = noiseHash33(wrappedCell) + offset;
                vec3 d = featurePoint - f;
                minDistSq = min(minDistSq, dot(d, d));
            }
        }
    }

    return 1.0 - clamp(sqrt(minDistSq), 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Classic Perlin gradient noise with wrapped lattice hashing (tiles at
// period 1 for integer rep). Gradients are Ken Perlin's 12 edge directions
// selected from the wrapped-lattice hash; quintic fade. Output remapped to
// approximately [0, 1] (clamped).
// ---------------------------------------------------------------------------
float perlinGradDot(vec3 cell, vec3 d, float rep)
{
    // Wrap the lattice point so the gradient field repeats with period rep,
    // then pick one of the 12 improved-noise edge gradients from the hash.
    int h = int(noiseHash13(mod(cell, rep)) * 16.0) & 15;
    float u = h < 8 ? d.x : d.y;
    float v = h < 4 ? d.y : ((h == 12 || h == 14) ? d.x : d.z);
    return (((h & 1) == 0) ? u : -u) + (((h & 2) == 0) ? v : -v);
}

float perlin3D(vec3 p, float rep)
{
    vec3 ps = p * rep;
    vec3 pi = floor(ps);
    vec3 pf = ps - pi;

    // Quintic fade (improved Perlin)
    vec3 w = pf * pf * pf * (pf * (pf * 6.0 - 15.0) + 10.0);

    float n000 = perlinGradDot(pi + vec3(0.0, 0.0, 0.0), pf - vec3(0.0, 0.0, 0.0), rep);
    float n100 = perlinGradDot(pi + vec3(1.0, 0.0, 0.0), pf - vec3(1.0, 0.0, 0.0), rep);
    float n010 = perlinGradDot(pi + vec3(0.0, 1.0, 0.0), pf - vec3(0.0, 1.0, 0.0), rep);
    float n110 = perlinGradDot(pi + vec3(1.0, 1.0, 0.0), pf - vec3(1.0, 1.0, 0.0), rep);
    float n001 = perlinGradDot(pi + vec3(0.0, 0.0, 1.0), pf - vec3(0.0, 0.0, 1.0), rep);
    float n101 = perlinGradDot(pi + vec3(1.0, 0.0, 1.0), pf - vec3(1.0, 0.0, 1.0), rep);
    float n011 = perlinGradDot(pi + vec3(0.0, 1.0, 1.0), pf - vec3(0.0, 1.0, 1.0), rep);
    float n111 = perlinGradDot(pi + vec3(1.0, 1.0, 1.0), pf - vec3(1.0, 1.0, 1.0), rep);

    float nx00 = mix(n000, n100, w.x);
    float nx10 = mix(n010, n110, w.x);
    float nx01 = mix(n001, n101, w.x);
    float nx11 = mix(n011, n111, w.x);
    float nxy0 = mix(nx00, nx10, w.y);
    float nxy1 = mix(nx01, nx11, w.y);
    float n = mix(nxy0, nxy1, w.z);

    // Edge gradients keep |n| within ~1 — remap to [0, 1]
    return clamp(n * 0.5 + 0.5, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// 3-octave Worley FBM — Nubis weights 0.625 / 0.25 / 0.125, frequency
// doubling per octave (integer freq keeps every octave tiling).
// ---------------------------------------------------------------------------
float worleyFbm3(vec3 p, float freq)
{
    return worley3D(p, freq) * 0.625 +
           worley3D(p, freq * 2.0) * 0.25 +
           worley3D(p, freq * 4.0) * 0.125;
}

// ---------------------------------------------------------------------------
// Perlin-Worley base shape (Schneider / Nubis, Hillaire): the Worley FBM
// carves the low-density regions out of the Perlin billows by remapping the
// Perlin value from [worley - 1, 1] onto [0, 1].
// ---------------------------------------------------------------------------
float perlinWorley(vec3 p, float freq)
{
    float pn = perlin3D(p, freq);
    float wn = worleyFbm3(p, freq);
    return clamp(remap(pn, wn - 1.0, 1.0, 0.0, 1.0), 0.0, 1.0);
}

#endif // NOISE_COMMON_GLSL
