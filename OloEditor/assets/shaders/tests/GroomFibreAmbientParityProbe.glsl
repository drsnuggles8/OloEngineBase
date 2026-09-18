// =============================================================================
// GroomFibreAmbientParityProbe.glsl
//
// The sibling of GroomFibreParityProbe.glsl, for the ENVIRONMENT half of the
// fibre material (#1247). Dumps include/GroomFibreCommon.glsl's
// oloGroomFibreAmbientResponse over the same angle grid so the C++ twin can be
// compared against the real compiled shader, texel for texel.
//
// A SEPARATE FILE rather than a fourth channel of the other probe, because that
// probe's four channels now carry its four lobes one each — see its header for
// why a summed channel was not an option.
//
// The ambient response shares the attenuations with the direct evaluation but
// reaches them by its own path and carries its own cos(theta_o) factor, so
// without this it would be the one piece of the material no parity test
// covered.
//
//   sinThetaO = -0.95 + 1.90 * (x / 63)
//   R/G/B/A   = the R, TT, TRT and residual ambient lobes' green channels
//
// sinThetaI and phi do not enter the ambient response at all; the grid still
// varies y so a shader that accidentally depended on it would show up as a
// column-to-column difference the C++ side does not predict.
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

	float sinThetaO = -0.95 + (1.90 * float(px) / 63.0);

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

	OloGroomFibreLobes ambient = oloGroomFibreAmbientResponse(fibre, sinThetaO);
	o_Result = vec4(ambient.R.g, ambient.TT.g, ambient.TRT.g, ambient.Residual.g);
}
