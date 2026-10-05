// =============================================================================
// GroomDualScatteringParityProbe.glsl
//
// Dumps the two functions dual scattering (#1533) added to the strand shader's
// includes — oloGroomCoatForwardTransmittance (GroomCoatShadowCommon.glsl) and
// oloGroomFibreBackScatterProjected (GroomFibreCommon.glsl) — over a
// deterministic grid, so their C++ twins GroomCoatShadow::
// CoatForwardTransmittance and GroomFibreBackScatterProjected can be compared
// against the REAL COMPILED SHADER, texel for texel.
//
// WHY. Both are formulas written twice by hand, and a formula written twice is
// the one that drifts on one side only. The drift would be silent: a coat
// forwarded with the wrong exponent still looks like a coat, and every CPU
// number in GroomFibreDualScatteringTests keeps agreeing with itself.
//
// A pure function dump, like GroomCoatTransmittanceParityProbe.glsl: it lights
// nothing, samples nothing and reads no uniform. The target is 128 x 64: the
// left half dumps the first function and the right half the second, each over
// its own 64 x 64 grid, so one harness and one draw serve both.
//
// PARAMETERISATION — restated in GroomCoatTransmittanceParityTest.cpp,
// deliberately: the restatement IS the comparison. Every argument is an exact
// binary fraction of the texel's integer coordinates, so the two sides cannot
// disagree about the arguments, only about the function.
//
//   left half (x in 0..63), oloGroomCoatForwardTransmittance:
//     tau   = 0.125 * (x + 1)                0.125 .. 8.0, through a coat's depth
//     kappa = 0.25 * (y + 1)                 0.25 .. 16.0, the authored range
//     a_f   = (x % 16, y % 16, (x + y) % 16) / 16
//     RGB   = the transmittance, A = the same at tau = +infinity (fully lit)
//
//   right half (x - 64 in 0..63), oloGroomFibreBackScatterProjected, A_b = (0.6, 0.3, 0.1),
//   shift = 0.0625, width = 0.375:
//     sinThetaI = (x + 0.5) / 32 - 1
//     sinThetaO = (y + 0.5) / 32 - 1
//     cosPhi    = 1 - ((x + 3 y) % 9) / 4    1 .. -1 in quarters
//     RGB = the lobe, A = cosPhi echoed back
//
// RGBA32F target: a half-float readback would quantise away the drift.
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

// Include paths resolve against assets/shaders/, NOT against this file.
#include "include/GroomCoatShadowCommon.glsl"
#include "include/GroomFibreCommon.glsl"

layout(location = 0) out vec4 o_Result;

void main()
{
	uint px = uint(floor(gl_FragCoord.x));
	uint py = uint(floor(gl_FragCoord.y));

	if (px < 64u)
	{
		float tau = 0.125 * float(px + 1u);
		float kappa = 0.25 * float(py + 1u);
		vec3 forwardScatter = vec3(float(px % 16u), float(py % 16u), float((px + py) % 16u)) / 16.0;
		float positiveInfinity = uintBitsToFloat(0x7F800000u);
		o_Result = vec4(oloGroomCoatForwardTransmittance(tau, kappa, forwardScatter),
		                oloGroomCoatForwardTransmittance(positiveInfinity, kappa, forwardScatter).r);
		return;
	}

	px -= 64u;
	float sinThetaI = ((float(px) + 0.5) / 32.0) - 1.0;
	float sinThetaO = ((float(py) + 0.5) / 32.0) - 1.0;
	float cosPhi = 1.0 - (float((px + (3u * py)) % 9u) / 4.0);
	o_Result = vec4(oloGroomFibreBackScatterProjected(vec3(0.6, 0.3, 0.1), 0.0625, 0.375, sinThetaO, sinThetaI,
	                                                  cosPhi),
	                cosPhi);
}
