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
// Get2D, Get1D, Get2D, Get1D, Get2D — and stores every one of the eight
// values UNPACKED across two RGBA32F attachments, so a dimension counter that
// advanced wrongly shows up as well as a wrong value. (An earlier version
// packed two draws into one channel; a 24-bit draw plus another scaled by 4
// needs 26 mantissa bits, so the pack rounded and one dimension went untested.)
//
// Attachment 0: .r = Get2D().x (dim 0)  .g = Get2D().y (dim 1)
//               .b = Get1D()   (dim 2)  .a = Get2D().x (dim 3)
// Attachment 1: .r = Get2D().y (dim 4)  .g = Get1D()   (dim 5)
//               .b = Get2D().x (dim 6)  .a = Get2D().y (dim 7)
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
layout(location = 1) out vec4 o_Color1;

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
    const vec2 third2D = oloPtGet2D(pathSampler);

    o_Color = vec4(first2D.x, first2D.y, first1D, second2D.x);
    o_Color1 = vec4(second2D.y, second1D, third2D.x, third2D.y);
}
