// =============================================================================
// GroomStrandHashParityProbe.glsl
//
// Dumps include/GroomStrandCommon.glsl's `oloGroomStochasticHash` and
// `oloGroomWidenedAlpha` — the exact functions GroomStrand.glsl calls — over a
// deterministic grid, so the C++ twins in OloEngine/Groom/GroomCoverage.{h,cpp}
// can be compared against the REAL COMPILED SHADER, texel for texel (#1246).
//
// WHY THIS EXISTS. The criterion-1 comparison is computed on the CPU: the
// coverage error every capture is judged against comes from
// GroomCoverage::StochasticHash and WidenedAlpha, not from the GPU. That is
// only meaningful while the two agree. If the GLSL hash is edited and the C++
// twin is not, every measured stochastic number keeps looking reasonable while
// describing a sample pattern that is not on screen — and nothing else in the
// suite can tell.
//
// This is a pure function dump. It computes no coverage and asserts no
// invariant; the C++ side reproduces the same grid and diffs. Keep it that way:
// anything clever added here has to be mirrored exactly on the C++ side, which
// is the thing this test exists to avoid having to trust.
//
// Parameterisation (integer texel coordinates, because the hash's inputs ARE
// integers — a pixel-centre float grid would be testing the decode, not the
// hash):
//   R = oloGroomStochasticHash(x, y, frame, segmentId, seed) with
//       frame     = x % 7      — small and coprime-ish with the axes, so the
//                                frame lane actually varies across the grid
//       segmentId = y * 13 + x — never constant, never equal to the pixel
//       seed      = 1246       — the production seed
//   G = oloGroomWidenedAlpha(x / 64.0)   — sweeps the half-pixel floor, so the
//                                          clamp knee is inside the grid
//   B = oloGroomRasterHalfWidth(x / 64.0)
//   A = 1
//
// The float target is RGBA32F: the hash lands on a 2^-24 grid and a half-float
// readback would quantise away exactly the low-bit drift this is looking for.
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
#include "include/GroomStrandCommon.glsl"

layout(location = 0) out vec4 o_Result;

void main()
{
	uint px = uint(floor(gl_FragCoord.x));
	uint py = uint(floor(gl_FragCoord.y));

	uint frame = px % 7u;
	uint segmentId = py * 13u + px;

	float hash = oloGroomStochasticHash(px, py, frame, segmentId, 1246u);

	float trueHalfWidth = float(px) / 64.0;
	o_Result = vec4(hash, oloGroomWidenedAlpha(trueHalfWidth), oloGroomRasterHalfWidth(trueHalfWidth), 1.0);
}
