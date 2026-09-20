// =============================================================================
// GroomCoatTransmittanceParityProbe.glsl
//
// Dumps include/GroomCoatShadowCommon.glsl's oloGroomCoatTransmittance — the
// exact function GroomStrand.glsl calls — over a deterministic grid of
// (optical depth, kappa) pairs, so the C++ twin
// GroomCoatShadow::CoatTransmittance can be compared against the REAL COMPILED
// SHADER, texel for texel.
//
// WHY THIS EXISTS. That header said, in its own words, that there was no GPU
// parity test for this pair yet. #1360 changed the formula on both sides at
// once — from exp(-kappa * tau) to the Poisson generating form
// exp(-tau * (1 - exp(-kappa))) — and a change made twice by hand is exactly
// the change that lands on one side only. The failure would be silent: a coat
// shaded with the old form still looks like a coat, and every CPU number in the
// suite would keep agreeing with itself.
//
// This covers the TRANSMITTANCE half of the twin. The march
// (oloGroomCoatOpticalDepth / SampleDensityVolume) still has no parity probe,
// because it needs a 3D volume uploaded and read back the same way on both
// sides, which is a larger harness than this slice needs.
//
// A pure function dump. It lights nothing, samples nothing, and must stay that
// way: anything clever here would have to be mirrored on the C++ side, which is
// what this test exists to avoid having to trust.
//
// PARAMETERISATION — an integer texel grid decoded into arguments. Written out
// here and restated in the C++ test, deliberately rather than factored away:
// the restatement IS the comparison.
//
//   tau   = 0.02 * (x + 1)                  0.02 .. 1.28, the range a coat
//                                           march actually produces
//   kappa = 0.25 * (y + 1)                  0.25 .. 16.0, the authored clamp
//                                           range end to end
//
//   R = the transmittance at (tau, kappa)
//   G = tau, echoed back
//   B = kappa, echoed back
//   A = the transmittance at (+infinity, kappa)
//
// THE TWO ECHOES ARE NOT PADDING. They are what separates "the shader computes
// a different function" from "the shader was handed different arguments", which
// are the two ways this comparison can fail and which need different fixes.
//
// ALPHA CARRIES THE NON-FINITE CASE, which no point on a finite grid can reach
// and which is the one input where the two sides disagreeing matters most: a
// corrupt optical depth must read FULLY LIT, because a bright coat is visibly
// "this did not run" and a black one is indistinguishable from a correct
// silhouette. The infinity is built from its BIT PATTERN rather than from a
// division by zero, so no optimiser is entitled to fold it away.
//
// RGBA32F target: a half-float readback would quantise away exactly the drift
// this is looking for, and the low end of the grid is where the two forms
// differ least.
// =============================================================================

#type vertex
#version 450 core

#ifdef OLO_VULKAN
layout(std430, binding = 57) readonly buffer OloVertexPull
{
	float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
#endif

void main()
{
#ifdef OLO_PULLED_VERTEX
	vec3 a_Position = vec3(b_Vertices.v[gl_VertexIndex * 3 + 0],
	                       b_Vertices.v[gl_VertexIndex * 3 + 1],
	                       b_Vertices.v[gl_VertexIndex * 3 + 2]);
#endif
	gl_Position = vec4(a_Position.xy, 0.0, 1.0);
}

#type fragment
#version 450 core

// Include paths resolve against assets/shaders/, NOT against this file — a
// "../include/..." here reads as assets/shaders/../include/ and fails.
#include "include/GroomCoatShadowCommon.glsl"

layout(location = 0) out vec4 o_Result;

void main()
{
	uint px = uint(floor(gl_FragCoord.x));
	uint py = uint(floor(gl_FragCoord.y));

	float tau = 0.02 * float(px + 1u);
	float kappa = 0.25 * float(py + 1u);

	float positiveInfinity = uintBitsToFloat(0x7F800000u);

	o_Result = vec4(oloGroomCoatTransmittance(tau, kappa),
	                tau,
	                kappa,
	                oloGroomCoatTransmittance(positiveInfinity, kappa));
}
