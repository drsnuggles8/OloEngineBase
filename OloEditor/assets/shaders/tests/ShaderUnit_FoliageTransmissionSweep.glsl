// =============================================================================
// ShaderUnit_FoliageTransmissionSweep.glsl — issue #1255.
//
// WHAT IT IS FOR, and why ShaderUnit_FoliageTransmission.glsl beside it is not
// enough. That probe checks #1234's NAMED CLAIMS: one column per claim, each a
// yes/no about the lobe's behaviour. #1255 needs something different — a DENSE
// SWEEP of the production lobe over view and light directions, so that a C++
// transcription of it (tests/Rendering/PathTracing/ProductionLeafLobeMirror.h)
// can be pinned against the real compiled function texel for texel.
//
// That pinning is what lets the independent slab reference in
// LeafTransmissionReferenceTest judge the SHADER rather than judging a copy of
// it. Without this file, every statement that test makes about "the production
// leaf lobe" would really be a statement about a transcription that nothing
// defends.
//
// It CALLS include/FoliageSurface.glsl rather than transcribing it, which is
// the same discipline the sibling probe uses and for the same reason.
//
// THE GRID, which the CPU side restates exactly — that restatement IS the
// comparison, so it is written out on both sides rather than factored away:
//
//   muV  = (x + 0.5) / 64          the view cosine with the leaf normal
//   muL  = (y + 0.5) / 64          the BACKLIGHT's cosine magnitude
//   phiL = pi * ((5x + 11y) mod 64) / 63
//
//   N = (0, 0, 1)                  the shading normal, viewer-facing
//   V = (sin, 0, muV)              surface -> eye, azimuth 0
//   L = (sin cos phiL, sin sin phiL, -muL)   surface -> light, BEHIND the leaf
//
// The azimuth is stirred by an integer hash of both coordinates rather than
// held at pi, because a lobe that ignored the azimuth entirely would otherwise
// match at every texel. This is the same reason the groom probes stir theirs.
//
// CHANNELS. Two authored profiles and two controls, so one readback covers the
// parameter range as well as the direction range:
//
//   .r  profile A — distortion 0.35, power 4.0, wrap 0.50. The shipped default.
//   .g  profile B — the SAME leaf with the exponent dropped to 1.5, and only
//       the exponent. One axis at a time is deliberate: the orderings the slab
//       reference predicts are about the lobe's WIDTH, and a profile that moved
//       distortion and wrap as well would confound the width with the constant
//       floor the wrap term adds. (That is not hypothetical — the first version
//       of this probe moved all three, and the flatness comparison on the CPU
//       side read backwards because profile A's higher wrap had lifted its
//       mean.) The rest of the authored range is swept on the CPU side in
//       LeafTransmissionReferenceTest; what THIS file establishes is the
//       identity that lets that sweep speak about the shader.
//   .b  profile A with the light in FRONT of the leaf. The forward-scattering
//       control: a term that was equally bright front and back would be an
//       ambient fill wearing a lobe's name, and #1234's probe checks that at one
//       direction while this checks it at 4096.
//   .a  profile A at thickness 0. Exactly 0 at every texel, which is the "this
//       layer is not a leaf material" path.
//
// tint and radiance are vec3(1) and shadow is 1, so each channel is the scalar
// lobe itself: the three multipliers the shader applies afterwards would only
// carry through unchanged and would hide the value being compared.
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

// Include paths resolve against assets/shaders/, NOT against this file. Only
// the LOBE half of FoliageSurface.glsl compiles here: OLO_FOLIAGE_SURFACE_
// SAMPLING is deliberately not defined, because the sampling half needs the
// foliage UBO and the leaf samplers and this probe is testing the maths.
#include "include/FoliageSurface.glsl"

layout(location = 0) out vec4 o_Result;

const float OLO_PROBE_PI = 3.14159265358979323846;
const float OLO_PROBE_THICKNESS = 0.75;

void main()
{
	int x = int(gl_FragCoord.x);
	int y = int(gl_FragCoord.y);

	float muV = (float(x) + 0.5) / 64.0;
	float muL = (float(y) + 0.5) / 64.0;
	float phiL = OLO_PROBE_PI * float((5 * x + 11 * y) % 64) / 63.0;

	vec3 N = vec3(0.0, 0.0, 1.0);
	vec3 V = vec3(sqrt(max(0.0, 1.0 - muV * muV)), 0.0, muV);

	float sinL = sqrt(max(0.0, 1.0 - muL * muL));
	vec3 Lback = vec3(sinL * cos(phiL), sinL * sin(phiL), -muL);
	vec3 Lfront = vec3(sinL * cos(phiL), sinL * sin(phiL), muL);

	vec3 radiance = vec3(1.0);
	vec3 tint = vec3(1.0);

	// distortion, power, wrap, environment scale. The environment scale does
	// not enter the direct half at all; it is carried so the vec4 is the one
	// the renderer publishes rather than a probe-shaped subset of it.
	vec4 lobeA = vec4(0.35, 4.0, 0.50, 0.35);
	vec4 lobeB = vec4(0.35, 1.5, 0.50, 0.35);

	o_Result = vec4(
		oloFoliageTransmissionDirect(N, V, Lback, radiance, 1.0, OLO_PROBE_THICKNESS, tint, lobeA).x,
		oloFoliageTransmissionDirect(N, V, Lback, radiance, 1.0, OLO_PROBE_THICKNESS, tint, lobeB).x,
		oloFoliageTransmissionDirect(N, V, Lfront, radiance, 1.0, OLO_PROBE_THICKNESS, tint, lobeA).x,
		oloFoliageTransmissionDirect(N, V, Lback, radiance, 1.0, 0.0, tint, lobeA).x);
}
