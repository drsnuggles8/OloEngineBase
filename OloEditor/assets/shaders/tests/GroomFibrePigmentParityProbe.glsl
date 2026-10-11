// =============================================================================
// GroomFibrePigmentParityProbe.glsl
//
// The per-strand pigment's sibling of GroomFibreParityProbe.glsl (#1558).
// Dumps include/GroomFibreCommon.glsl's oloGroomFibrePigmentSigmaA -- the table
// lookup GroomFibrePigmentMode::BaseColorPerStrand reads each strand's
// absorption from -- so GroomFibrePigmentSigmaA, its C++ twin, can be compared
// against the real compiled shader, column for column.
//
//   albedo  = 2^(-12 + 12.5 * (x / 63)): past the table's dark end (2^-10),
//             across every entry, and past its bright end, where both sides
//             clamp
//   colour  = (albedo, 0.6 albedo, 0.25 albedo): each channel looks up its own
//   RGB     = the sigma_a the lookup returns for that colour
//
// The table itself comes from the test, in a uniform buffer on UBO_USER_0 (7),
// the pass-local slot whose occupant fills it: it is the production
// GroomFibrePigmentTable, built from the same authored fibre the C++ side
// evaluates, so the comparison is of the lookup and nothing else.
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

layout(std140, binding = 7) uniform PigmentProbeTable
{
	vec4 u_PigmentTable[8];
};

void main()
{
	uint px = uint(floor(gl_FragCoord.x));
	float albedo = exp2(-12.0 + (12.5 * float(px) / 63.0));
	vec3 colour = vec3(albedo, albedo * 0.6, albedo * 0.25);
	o_Result = vec4(oloGroomFibrePigmentSigmaA(colour, u_PigmentTable), 1.0);
}
