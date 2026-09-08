// =============================================================================
// PathTracerSampler.glsl — the GLSL twin of Renderer/PathTracing/PathSampler.h
// (issue #1055, #979 Phase 2).
//
// WHAT IT IS. The CPU reference path tracer draws every random value from an
// Owen-scrambled Sobol' sequence keyed on (pixel seed, sample index, dimension)
// and nothing else — no shared stream, no state outside the sampler object.
// This file is that sampler OBJECT: a struct holding the key and the next
// dimension, and Get1D / Get2D advancing it, so a GPU path that consumes
// dimensions in the same ORDER as the CPU integrator draws the SAME numbers.
// That is what makes the two tracers comparable term by term rather than only
// in the converged image: with matching seeds the GPU's sample i for pixel
// (x, y) is the CPU's sample i for pixel (x, y).
//
// THE ARITHMETIC IS NOT HERE. StochasticCommon.glsl already carries the
// bit-exact transcription of PathSampler.h's generator (OloSobolOwen1D / 2D
// and the helpers under them, pinned by StochasticSamplerTest) for the
// screen-space passes; this file calls it rather than carrying a second copy
// that a change to the direction numbers or to ToUnitFloat would have to
// reach twice. PathTracerSamplerParityTest pins THIS interface — the
// dimension walk and the pixel seed — against the C++ object on the device.
//
// NAMING. Every symbol carries the oloPt prefix: this include ends up in the
// same translation unit as PBRCommon.glsl and StochasticCommon.glsl.
// =============================================================================

#ifndef OLO_PATH_TRACER_SAMPLER_GLSL
#define OLO_PATH_TRACER_SAMPLER_GLSL

#include "StochasticCommon.glsl"

// Construct one per (pixel, sample index); pull dimensions in a FIXED order
// along the path. Dimension is the only mutable state.
struct OloPathSampler
{
    uint PixelSeed;
    uint SampleIndex;
    uint Dimension;
};

OloPathSampler oloPtMakeSampler(uint pixelSeed, uint sampleIndex)
{
    OloPathSampler s;
    s.PixelSeed = pixelSeed;
    s.SampleIndex = sampleIndex;
    s.Dimension = 0u;
    return s;
}

// PathSampler::Get1D — one dimension consumed.
float oloPtGet1D(inout OloPathSampler s)
{
    const float value = OloSobolOwen1D(s.SampleIndex, s.PixelSeed, s.Dimension);
    s.Dimension += 1u;
    return value;
}

// PathSampler::Get2D — two dimensions consumed; both components walk the
// SAME index-scrambled point of the 2D Sobol' set (StochasticCommon's note).
vec2 oloPtGet2D(inout OloPathSampler s)
{
    const vec2 value = OloSobolOwen2D(s.SampleIndex, s.PixelSeed, s.Dimension);
    s.Dimension += 2u;
    return value;
}

// Stable per-pixel seed — twin of PathSampler.h's MakePixelSeed. Deliberately
// not the raw pixel index: a linear seed correlates the hash's low bits along
// a scanline, which shows up as horizontal structure at low sample counts.
uint oloPtMakePixelSeed(uint x, uint y, uint globalSeed)
{
    return OloHashCombine(OloHashCombine(globalSeed, x), y * 0x2545f491u + 1u);
}

#endif // OLO_PATH_TRACER_SAMPLER_GLSL
