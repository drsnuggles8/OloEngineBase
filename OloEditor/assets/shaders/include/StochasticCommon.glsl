// =============================================================================
// StochasticCommon.glsl — the ONE header a stochastic screen-space pass includes
// (issue #706)
//
// Pulls in all three parts of the shared stochastic toolkit:
//
//   1. this file  — the screen-space blue-noise sample sequence
//   2. PBRCommon  — GGX VNDF importance sampling + its G2/G1 weight
//   3. TemporalResolve — reprojection / neighbourhood clip / disocclusion
//
// so a pass that wants "good samples, correctly weighted, temporally resolved"
// writes one #include and gets a consistent set rather than reinventing each
// piece. Before this existed, SSGI hashed interleaved-gradient noise, SSR did
// not sample stochastically at all, and TAA and the cloudscape resolve each
// carried their own hand-rolled history blend.
//
// -----------------------------------------------------------------------------
// WHY BLUE NOISE, AND WHAT "BLUE" ACTUALLY BUYS
// -----------------------------------------------------------------------------
// The point is NOT that the numbers are more random. At a fixed sample count
// the per-pixel error is whatever it is; what a blue-noise sampler changes is
// how that error is DISTRIBUTED ACROSS THE SCREEN. White noise puts error
// energy at every spatial frequency, including the low ones the eye is most
// sensitive to and that no small-radius denoiser or temporal filter can remove.
// A blue-noise-distributed error has its energy pushed into the high
// frequencies, which is precisely where a 3x3 filter, TAA, or the human visual
// system already attenuates. Same rays, same cost, visibly less noise.
//
// -----------------------------------------------------------------------------
// THE CONSTRUCTION, AND HOW IT RELATES TO PathSampler.h
// -----------------------------------------------------------------------------
// The engine already has a sampler: OloEngine/src/OloEngine/Renderer/
// PathTracing/PathSampler.h, the Owen-scrambled Sobol' sequence the offline
// reference path tracer uses. That one is CPU-side and optimises a different
// thing — bit-reproducibility and per-PIXEL convergence for an oracle image.
// This one is GPU-side and optimises SCREEN-SPACE error distribution at one or
// two samples per pixel. They are genuinely different jobs.
//
// They are NOT, however, allowed to disagree about what "stratified" means, so
// this header does not invent a second low-discrepancy sequence: the functions
// below are a bit-exact GLSL mirror of PathSampler.h's integer math (same
// ReverseBits, same Laine-Karras permutation, same nested-uniform scramble,
// same Sobol' direction numbers, same 24-bit ToUnitFloat). Given the same seed
// and sample index the two produce the SAME numbers, and
// StochasticSamplerTest.GlslSobolMirrorsPathSampler pins that.
//
// What this header adds on top is the screen-space half, in two steps:
//
//   * A blue-noise MASK (a 64x64 tile, void-and-cluster, generated on the CPU
//     by Renderer/BlueNoise.h and uploaded at TEX_BLUE_NOISE) supplies a
//     per-pixel Cranley-Patterson ROTATION of the shared Sobol' sequence. Every
//     pixel therefore walks the same stratified point set, offset by an amount
//     that is blue-noise-distributed across the screen — which is what turns
//     per-pixel error into screen-space blue noise (Georgiev & Fajardo 2016;
//     the same property Heitz et al. 2019 reach with optimised ranking /
//     scrambling tiles, see the note at the bottom of this comment).
//
//   * The rotation is advanced per frame along the R2 low-discrepancy sequence
//     (Roberts 2018), so each individual pixel sees a low-discrepancy sequence
//     over TIME — which is what makes a temporal resolve converge instead of
//     merely averaging. Measured over 64 frames the largest gap in one pixel's
//     sequence is 1.25x the ideal 1/N, against 4.44x for white noise
//     (StochasticSampler.FrameAdvanceIsLowDiscrepancyPerPixel).
//
//     This costs some of the spatial property, and it is worth being precise
//     about why: `fract(tile + k)` is an offset in VALUE with wraparound, not a
//     rigid shift in SPACE, so pixels whose tile values straddle the wrap swap
//     their relative order. Measured, the low/high frequency power ratio goes
//     from 0.0002 on the raw tile to 0.057 on an advanced frame — still ~18x
//     less low-frequency energy than white noise's ~1.0, but not free. Pinned
//     by StochasticSampler.ConsecutiveFramesRedrawTheField, which asserts BOTH
//     that the field is redrawn and that it is still blue afterwards.
//
// A NOTE ON HEITZ ET AL. 2019, which issue #706 cites: that sampler reaches the
// same screen-space property by looking up a per-pixel RANK and SCRAMBLE key in
// tiles produced by a simulated-annealing optimiser. The tiles are ~1 MB of
// optimiser output distributed as a binary blob with the paper; there is no way
// to author them here that is not either vendoring that blob or re-running the
// optimiser. The mask-rotation construction above is the standard alternative,
// needs 8 KiB we can generate deterministically and TEST, and delivers the
// property the issue actually asks for. That was a deliberate substitution, not
// an oversight.
//
// -----------------------------------------------------------------------------
// USING IT
// -----------------------------------------------------------------------------
//   #define OLO_BLUE_NOISE_GLOBAL_SAMPLER   // if you want the engine-global tile
//   #include "include/StochasticCommon.glsl"
//
//   ivec2 px = ivec2(gl_FragCoord.xy);
//   vec2 u = OloSampleRandomVector2D(px, u_FrameIndex);            // 1 spp
//   vec2 u = OloSampleStratified2D(px, u_FrameIndex, r, rayCount, 0u);  // ray r of N
//
// The tile binding is opt-in for the same reason DDGICommon.glsl's is: passes
// that already bind their own noise (or run in a context with no global bind)
// still want the sequence math.
// =============================================================================

#ifndef OLO_STOCHASTIC_COMMON_GLSL
#define OLO_STOCHASTIC_COMMON_GLSL

#include "MathCommon.glsl"
#include "PBRCommon.glsl"
#include "TemporalResolve.glsl"

// -----------------------------------------------------------------------------
// The blue-noise tile
// -----------------------------------------------------------------------------
// Mirrors OloEngine::BlueNoise::kTileSize — pinned equal by
// StochasticSamplerTest.ShaderTileSizeMatchesTheGenerator, because a shader
// that masks with the wrong period samples a folded tile and quietly loses the
// blue-noise property it is here for.
//
// Power of two so the screen wrap is a mask rather than a modulo. 64 rather
// than 128 on measurement, not taste: generation is O(pixels^2), and 128x128
// cost 466 ms against this tile's 12 ms (Release; ~38x, well past the 4x the
// pixel count alone predicts — the scans stop fitting in cache) for spectrally
// IDENTICAL output: low/high band power 0.0002 either way, neighbour
// correlation -0.272 against -0.284.
//
// And the tile PERIOD is not what went wrong in glsl-shaders.md section 11's
// lattice artifact — the field's neighbour CORRELATION was (+0.83 there,
// -0.28 here). A bigger tile would have bought nothing there either.
#define OLO_BLUE_NOISE_TILE_SIZE 64
#define OLO_BLUE_NOISE_TILE_MASK (OLO_BLUE_NOISE_TILE_SIZE - 1)

// The R2 low-discrepancy sequence's additive constants (Roberts 2018) — the
// generalised golden ratio in two dimensions. Same numbers GTAO.comp uses to
// turn its Hilbert index into a value; see glsl-shaders.md section 11 for what
// happens when an ordering is used as a value instead.
//
// HELD IN 32-BIT FIXED POINT, AND THE ADVANCE IS COMPUTED IN INTEGERS.
// The obvious `fract(tile + float(frameIndex) * alpha)` loses the low bits of
// the product as frameIndex grows, because an f32 large enough to hold
// frameIndex * alpha has no precision left for its fraction. Measured against
// the real constants: at frame 3600 (one minute) the advance still resolves
// 4096 distinct values, but by frame 262144 (~73 min at 60 Hz) only **64**
// remain, and at the 2^20 counter ceiling only **16** — well under the tile's
// own 256 levels, so the sampler quietly stops advancing properly after about
// an hour of uptime.
//
// A uint multiply wraps modulo 2^32, which IS the fractional part, exactly, at
// every frame index. So the advance below is exact forever and the failure mode
// simply does not exist. (It also cannot be caught by a CPU mirror that uses
// `double` — which is exactly how it survived the first round of tests here.)
#define OLO_R2_ALPHA_FX_X 3242174889u // round(0.75487766624669276005 * 2^32)
#define OLO_R2_ALPHA_FX_Y 2447445414u // round(0.5698402909980532659114 * 2^32)

// Golden-ratio conjugate — the 1D case of the same idea. (0x9e3779b9.)
#define OLO_R1_ALPHA_FX 2654435769u // round(0.61803398874989484820 * 2^32)

// The Owen scramble key for the shared point set. Fixed for the lifetime of the
// engine: every pixel and every frame must walk the SAME Sobol' points, or the
// two properties this header exists for (screen-space blue error, temporal
// low-discrepancy) both stop holding. See OloSampleStratified2D.
#define OLO_SOBOL_SCRAMBLE_SEED 0x9e3779b9u

// A 32-bit fixed-point fraction as a float in [0, 1). Same 24-bit truncation as
// OloToUnitFloat below, and for the same reason: the result must stay strictly
// below 1.0.
float OloFixedToUnit(uint fx)
{
    return float(fx >> 8u) * (1.0 / 16777216.0);
}

#ifdef OLO_BLUE_NOISE_GLOBAL_SAMPLER
#ifdef OLO_BINDLESS
#define u_OloBlueNoiseTile OLO_HEAP_TEX_2D(17) // TEX_BLUE_NOISE
#else
// Slot 17 in the SAMPLER namespace. It has been free since issue #435 retired
// the four point-cubemap slots 14-17 (ShaderBindingLayout.h says so in as many
// words), so this costs no renumbering of TEX_SHADER_GRAPH_0 and therefore does
// not move MAX_ENGINE_TEXTURE_SLOTS / OLO_HEAP_IMAGE_BASE — the drift issue #702
// caused and BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout now
// guards against.
//
// 17 IS taken in the other two GL namespaces (UBO_FOG, SSBO_INSTANCE_DRAW_INDIRECT).
// That is legal on GL's disjoint namespaces and legal on Vulkan too, EXCEPT
// inside a single shader — the ADR item A2 collision. So the standing rule for
// this header is: NO shader may include it with the global sampler enabled AND
// declare uniform block 17 or storage block 17. Pinned headlessly by
// StochasticSamplerTest.NoShaderCollidesWithTheBlueNoiseSlot, which greps the
// shader tree rather than trusting this comment.
layout(binding = 17) uniform sampler2D u_OloBlueNoiseTile;
#endif

// The raw tile value at a screen pixel: two channels of INDEPENDENT
// void-and-cluster blue noise, so a 2D sample's components are decorrelated.
// Deriving the second channel from the first is the mistake glsl-shaders.md
// section 11 documents (fract(first * golden) collapses to a short ramp).
vec2 OloBlueNoiseTile2D(ivec2 pixel)
{
    return texelFetch(u_OloBlueNoiseTile, pixel & ivec2(OLO_BLUE_NOISE_TILE_MASK), 0).rg;
}

float OloBlueNoiseTile1D(ivec2 pixel)
{
    return texelFetch(u_OloBlueNoiseTile, pixel & ivec2(OLO_BLUE_NOISE_TILE_MASK), 0).r;
}

// The helper issue #706 names. One blue-noise-distributed 2D value per pixel
// per frame, advanced along R2 so each pixel's own sequence over time is
// low-discrepancy. Use this when the pass takes ONE sample per pixel per frame
// and relies on the temporal resolve to accumulate.
vec2 OloSampleRandomVector2D(ivec2 pixel, uint frameIndex)
{
    vec2 advance = vec2(OloFixedToUnit(frameIndex * OLO_R2_ALPHA_FX_X),
                        OloFixedToUnit(frameIndex * OLO_R2_ALPHA_FX_Y));
    return fract(OloBlueNoiseTile2D(pixel) + advance);
}

float OloSampleRandomValue(ivec2 pixel, uint frameIndex)
{
    return fract(OloBlueNoiseTile1D(pixel) + OloFixedToUnit(frameIndex * OLO_R1_ALPHA_FX));
}
#endif // OLO_BLUE_NOISE_GLOBAL_SAMPLER

// -----------------------------------------------------------------------------
// Owen-scrambled Sobol' — a bit-exact mirror of PathSampler.h
//
// Every function below has a named C++ counterpart in
// OloEngine/src/OloEngine/Renderer/PathTracing/PathSampler.h. If you change a
// formula here, change the mirror too — StochasticSamplerTest pins them equal,
// which is the whole reason the two samplers can coexist.
// -----------------------------------------------------------------------------

// SamplerDetail::ReverseBits
uint OloReverseBits(uint x)
{
    x = (x << 16u) | (x >> 16u);
    x = ((x & 0x55555555u) << 1u) | ((x & 0xAAAAAAAAu) >> 1u);
    x = ((x & 0x33333333u) << 2u) | ((x & 0xCCCCCCCCu) >> 2u);
    x = ((x & 0x0F0F0F0Fu) << 4u) | ((x & 0xF0F0F0F0u) >> 4u);
    x = ((x & 0x00FF00FFu) << 8u) | ((x & 0xFF00FF00u) >> 8u);
    return x;
}

// SamplerDetail::LaineKarrasPermutation (Burley 2020, listing 4)
uint OloLaineKarrasPermutation(uint x, uint seed)
{
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

// SamplerDetail::NestedUniformScramble
uint OloNestedUniformScramble(uint x, uint seed)
{
    x = OloReverseBits(x);
    x = OloLaineKarrasPermutation(x, seed);
    return OloReverseBits(x);
}

// SamplerDetail::Sobol0 — van der Corput in base 2.
uint OloSobol0(uint index)
{
    return OloReverseBits(index);
}

// SamplerDetail::Sobol1 — the classic second direction-number set. The C++ side
// loops while index != 0; the explicit 32-iteration bound here is the same loop
// (a uint has 32 bits) written so the shader compiler can see the trip count.
uint OloSobol1(uint index)
{
    uint v = 0u;
    uint direction = 0x80000000u;
    for (uint i = 0u; i < 32u; ++i)
    {
        if (index == 0u)
            break;
        if ((index & 1u) != 0u)
            v ^= direction;
        index >>= 1u;
        direction ^= direction >> 1u;
    }
    return v;
}

// SamplerDetail::HashCombine
uint OloHashCombine(uint seed, uint value)
{
    uint x = seed ^ (value * 0x9e3779b9u);
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

// SamplerDetail::ToUnitFloat — 24 bits of the code word, strictly below 1.0.
// The truncation is load-bearing: a returned 1.0 makes sqrt(1 - xi) exactly 0
// and sends a cosine-sampled direction into the surface plane.
float OloToUnitFloat(uint x)
{
    return float(x >> 8u) * (1.0 / 16777216.0);
}

// PathSampler::Get2D for a given (seed, dimension). Both components must walk
// the SAME index-scrambled point of the 2D Sobol' set or the pair stops being
// stratified — that invariant is why the index is derived from scrambleX alone.
vec2 OloSobolOwen2D(uint sampleIndex, uint seed, uint dimension)
{
    uint scrambleX = OloHashCombine(seed, dimension);
    uint scrambleY = OloHashCombine(seed, dimension + 1u);
    uint index = OloNestedUniformScramble(sampleIndex, scrambleX ^ 0x51633e2du);
    return vec2(OloToUnitFloat(OloNestedUniformScramble(OloSobol0(index), scrambleX)),
                OloToUnitFloat(OloNestedUniformScramble(OloSobol1(index), scrambleY)));
}

// PathSampler::Get1D for a given (seed, dimension).
float OloSobolOwen1D(uint sampleIndex, uint seed, uint dimension)
{
    uint scramble = OloHashCombine(seed, dimension);
    uint index = OloNestedUniformScramble(sampleIndex, scramble ^ 0x51633e2du);
    return OloToUnitFloat(OloNestedUniformScramble(OloSobol0(index), scramble));
}

#ifdef OLO_BLUE_NOISE_GLOBAL_SAMPLER
// -----------------------------------------------------------------------------
// The combined sampler: stratified within a pixel, blue in screen space.
//
// EVERY PIXEL WALKS THE SAME SOBOL' POINT SET — the seed deliberately does NOT
// depend on `pixel`. That is what makes the per-pixel error a rigid function of
// the rotation, and therefore blue across the screen exactly as the mask is. A
// per-pixel seed would give each pixel an independent (still low-discrepancy)
// sequence, whose errors are mutually white — which is the ordinary sampler
// this header exists to replace.
//
// `dimension` decorrelates independent 2D quantities drawn in the same pass
// (a hemisphere direction and a light-selection pair, say). Pass 0, 2, 4, …
// exactly as PathSampler's dimension counter does.
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// DIMENSION 0 IS STRATIFIED ACROSS THE SAMPLE INDEX; DIMENSION 1 IS ROTATED.
// The asymmetry is the whole point, and getting it wrong is measurable.
//
// The first version of this function rotated BOTH dimensions by the blue-noise
// tile -- the textbook Cranley-Patterson construction. That is correct on the
// torus, and it is wrong here, because the caller feeds dimension 0 to a
// hemisphere RADIUS (sqrt(u1) in OloCosineHemisphere) and a radius does not
// wrap: pushing u1 from 0.95 to 0.05 turns a grazing ray into a near-normal
// one. The rotation therefore destroys exactly the stratification that
// u1 = (sampleIndex + 0.5) / sampleCount gives for free.
//
// Measured, modelling SSGI's own integral (a cosine-weighted hemisphere gather
// at 8 rays/pixel), per-pixel error RMS against a converged reference:
//
//     sampler                       smooth integrand   hard-edged (occluder)
//     interleaved-gradient (old)          0.0218               0.0755
//     rotate both dimensions              0.0349               0.0856   <- worse
//     stratify dim 0, jitter within       0.0124               0.0574   <- this
//
// Rotating both was WORSE than the noise it replaced, on both integrands, and
// the rendered frame agreed. What works is ordinary stratified sampling with a
// blue-noise jitter INSIDE each stratum: the stratification survives intact,
// and the jitter is what decorrelates neighbouring pixels (the old sampler used
// the SAME u1 at every pixel, so its dimension-0 error was a screen-wide
// constant -- a bias, not noise).
//
// Dimension 1 is an azimuth, which genuinely is toroidal, so it takes the
// ordinary rotation.
//
// sampleCount == 1 degenerates to u1 = rot.x, i.e. the plain blue-noise value,
// which is the right answer when there is nothing to stratify across.
// -----------------------------------------------------------------------------
vec2 OloSampleStratified2D(ivec2 pixel, uint frameIndex, uint sampleIndex, uint sampleCount, uint dimension)
{
    // THE SCRAMBLE SEED IS A CONSTANT. It must not depend on frameIndex, and
    // that is the opposite of the intuition that got written here first.
    //
    // Re-hashing the seed per frame does "redraw the sequence" — but it redraws
    // it to an UNRELATED point set, so a given pixel's value over time becomes
    // i.i.d. white noise and the temporal resolve has nothing low-discrepancy to
    // accumulate. Measured over 64 frames: largest per-pixel gap 4.3x the ideal
    // 1/N with a per-frame seed, 1.25x with the constant seed below.
    //
    // The temporal advance is the R2 ROTATION, which is where it belongs.
    vec2 rot = OloSampleRandomVector2D(pixel, frameIndex);
    float n = max(float(sampleCount), 1.0);
    return vec2((float(sampleIndex) + rot.x) / n,
                fract(OloSobolOwen1D(sampleIndex, OLO_SOBOL_SCRAMBLE_SEED, dimension) + rot.y));
}
#endif // OLO_BLUE_NOISE_GLOBAL_SAMPLER

// -----------------------------------------------------------------------------
// Cosine-weighted hemisphere direction from a 2D sample (Malley's method).
// Kept here rather than in each pass because the pairing with the sampler is
// the part that is easy to get subtly wrong: u.x drives the radius, so it must
// be the component the caller stratified.
// -----------------------------------------------------------------------------
vec3 OloCosineHemisphere(vec2 u, vec3 n)
{
    float radius = sqrt(u.x);
    float phi = TWO_PI * u.y;
    vec3 local = vec3(radius * cos(phi), radius * sin(phi), sqrt(max(0.0, 1.0 - u.x)));

    vec3 t;
    vec3 b;
    OrthonormalBasis(n, t, b);
    return normalize(t * local.x + b * local.y + n * local.z);
}

#endif // OLO_STOCHASTIC_COMMON_GLSL
