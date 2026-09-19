// =============================================================================
// GroomFibreParityProbe.glsl
//
// Dumps include/GroomFibreCommon.glsl's far-field evaluation — the exact
// function GroomStrand.glsl calls — over a deterministic grid of angles, so the
// C++ twin in OloEngine/Groom/GroomFibreScattering.{h,cpp} can be compared
// against the REAL COMPILED SHADER, texel for texel (#1247).
//
// WHY THIS EXISTS. The lobe comparison in
// docs/analysis/groom-fibre-scattering-1247.md is computed on the CPU: the
// quadrature rule was CHOSEN there, the energy check runs there, and the
// dark-versus-pale numbers come from there. That is only meaningful while the
// two sides agree. Edit the GLSL and not the C++ and every measured number
// keeps looking reasonable while describing a material that is not on screen —
// and no other test in the suite can tell, because a plausible hair shader and
// a correct one produce the same kind of picture.
//
// A pure function dump. It lights nothing, and it must stay that way: anything
// clever here would have to be mirrored on the C++ side, which is exactly what
// this test exists to avoid having to trust.
//
// PARAMETERISATION — an integer texel grid decoded into angles. Written out
// here and restated in the C++ test, deliberately rather than factored away:
// the restatement IS the comparison.
//
//   sinThetaO = -0.95 + 1.90 * (x / 63)     the view's longitudinal angle
//   sinThetaI = -0.95 + 1.90 * (y / 63)     the light's
//   phi       = pi * ((x * 7 + y * 13) % 64) / 63
//                                           an azimuth that is never constant
//                                           along either axis, so a shader that
//                                           dropped phi entirely would still
//                                           have to reproduce the pattern
//
//   R = the R lobe's green channel
//   G = the TT lobe's green channel
//   B = the TRT lobe's green channel
//   A = the residual lobe's green channel
//
// ONE CHANNEL PER LOBE, NEVER A SUM, and that is load-bearing twice over. The
// three pigment channels differ only through sigma_a, which the C++ side
// computes and uploads, so splitting by LOBE catches strictly more than
// splitting by colour would — a lobe that goes missing is invisible in a sum
// that another lobe dominates.
//
// It is also the form that works. An earlier version packed TRT + residual into
// one channel, and that channel read back as exactly zero here while a constant
// in the same channel read back fine and each lobe alone read its correct
// value. The cause was not established; the split removes the construct and
// improves the coverage, so it was not chased further. If a future probe needs
// a summed channel, measure it before trusting it.
//
// The ambient response is the sibling probe, GroomFibreAmbientParityProbe.glsl.
// It needs its own file rather than alpha here precisely because alpha is now
// carrying the fourth lobe.
//
// The material below is FIXED and spelled out, so the probe needs no uniforms
// and the C++ side builds the identical GroomFibreParams from the same
// numbers. RGBA32F target: these are small floats and a half-float readback
// would quantise away the drift this is looking for.
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
#include "include/GroomFibreCommon.glsl"

layout(location = 0) out vec4 o_Result;

// The probe's fixed material. These are DERIVED GroomFibreParams values, the
// same ones the CPU uploads, so the C++ side of the test builds them with
// MakeGroomFibreParams from the authored numbers named in its own comment
// rather than repeating these literals.
OloGroomFibre oloProbeFibre()
{
	OloGroomFibre fibre;
	// Eumelanin 1.3, pheomelanin 0.0 — the brown fibre the analysis uses.
	fibre.SigmaA = vec3(0.5447, 0.9061, 1.7810);
	fibre.Eta = 1.55;
	fibre.V0 = 0.0;   // filled below
	fibre.S = 0.0;    // filled below
	fibre.Intensity = 1.0;
	fibre.Sin2kAlpha = vec3(0.0);
	fibre.Cos2kAlpha = vec3(1.0);
	fibre.HSamples = 4;
	return fibre;
}

void main()
{
	uint px = uint(floor(gl_FragCoord.x));
	uint py = uint(floor(gl_FragCoord.y));

	float sinThetaO = -0.95 + (1.90 * float(px) / 63.0);
	float sinThetaI = -0.95 + (1.90 * float(py) / 63.0);
	float phi = OLO_GROOM_FIBRE_PI * float((px * 7u + py * 13u) % 64u) / 63.0;

	// Derived on the fly from the same fits the C++ MakeGroomFibreParams uses,
	// for beta_M = beta_N = 0.3 and a 2 degree tilt. Written here rather than
	// passed in because the probe takes no uniforms — and because a derivation
	// the shader performs is one more thing the parity test covers.
	OloGroomFibre fibre = oloProbeFibre();
	float bm = 0.3;
	float bn = 0.3;
	float v0 = 0.726 * bm + (0.812 * bm * bm) + (3.7 * pow(bm, 20.0));
	fibre.V0 = v0 * v0;
	fibre.S = 0.626657069 * ((0.265 * bn) + (1.194 * bn * bn) + (5.372 * pow(bn, 22.0)));

	float alpha = radians(2.0);
	float s0 = sin(alpha);
	float c0 = oloGroomFibreSafeSqrt(1.0 - (s0 * s0));
	float s1 = 2.0 * c0 * s0;
	float c1 = (c0 * c0) - (s0 * s0);
	float s2 = 2.0 * c1 * s1;
	float c2 = (c1 * c1) - (s1 * s1);
	fibre.Sin2kAlpha = vec3(s0, s1, s2);
	fibre.Cos2kAlpha = vec3(c0, c1, c2);

	OloGroomFibreLobes lobes = oloGroomFibreEvaluate(fibre, sinThetaO, sinThetaI, phi);

	o_Result = vec4(lobes.R.g, lobes.TT.g, lobes.TRT.g, lobes.Residual.g);
}
