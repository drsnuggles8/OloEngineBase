// =============================================================================
// PathTracerSamplerProbe.glsl
//
// Dumps include/PathTracerSampler.glsl — the GLSL twin of PathSampler.h — over
// a deterministic (pixel seed, sample index) grid so the C++ sampler can be
// compared against the REAL COMPILED SHADER value for value (issue #1055).
//
// The GPU path tracer's claim to be comparable with the CPU reference TERM BY
// TERM rests on the two drawing the same random numbers for the same (pixel,
// sample, dimension). The sampler is pure integer arithmetic modulo 2^32 plus
// one exactly-representable float conversion, so the bar here is EXACT
// equality, not a tolerance: PathTracerSamplerParityTest fails on a single
// differing bit.
//
// Like the other probes this computes NO estimator and asserts no invariant.
// It is a pure function dump; the C++ side reproduces the grid and diffs.
//
// Parameterisation (integer pixel coordinates, no interpolation involved):
//   x -> sample index      (0 .. width - 1)
//   y -> pixel seed row    (the seed is oloPtMakePixelSeed(y, 7, GLOBAL_SEED))
// Each texel draws the same fixed dimension sequence the integrator draws:
// Get2D, Get1D, Get2D, Get1D — and stores the four resulting values so a
// dimension counter that advanced wrongly shows up as well as a wrong value.
//
// Output: .r = Get2D().x   (dims 0,1)
//         .g = Get2D().y
//         .b = Get1D()     (dim 2)
//         .a = Get2D().x + Get1D() * 4.0   (dims 3,4 then 5) — packed so all
//              six draws fit one RGBA32F texel; both terms are < 1, so the
//              C++ side unpacks them exactly with floor / fract in double.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/PathTracerSampler.glsl"

// Must match PathTracerSamplerParityTest.cpp.
const uint GLOBAL_SEED = 0x9e3779b9u;
const uint SEED_COLUMN = 7u;

void main()
{
    const uvec2 texel = uvec2(gl_FragCoord.xy);
    const uint sampleIndex = texel.x;
    const uint pixelSeed = oloPtMakePixelSeed(texel.y, SEED_COLUMN, GLOBAL_SEED);

    OloPathSampler pathSampler = oloPtMakeSampler(pixelSeed, sampleIndex);
    const vec2 first2D = oloPtGet2D(pathSampler);
    const float first1D = oloPtGet1D(pathSampler);
    const vec2 second2D = oloPtGet2D(pathSampler);
    const float second1D = oloPtGet1D(pathSampler);

    o_Color = vec4(first2D.x, first2D.y, first1D, second2D.x + second1D * 4.0);
}
