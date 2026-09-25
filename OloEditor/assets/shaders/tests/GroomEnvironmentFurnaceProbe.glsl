// =============================================================================
// GroomEnvironmentFurnaceProbe.glsl — the white furnace for the coat's
// environment term (issue #1450).
//
// The test binds a REAL irradiance cube, baked by IBLPrecompute from a uniform
// sky of radiance L, at TEX_USER_0 (binding 10) — the slot GroomStrand.glsl
// samples. This probe then runs the two production readers of that cube side by
// side:
//
//   columns 0-3  the coat's environment term, exactly as GroomStrand.glsl
//                composes it: oloGroomFibreAmbientResponse times
//                oloGroomFibreEnvironmentRadiance, summed over the lobes, at
//                four view angles and four cube directions
//   column 4     a Lambertian surface on the ambient ladder's IBL rung:
//                calculateLightProbeAmbient fed the same cube's sample
//   column 5     oloGroomFibreEnvironmentRadiance alone, along -Y
//
// Both readers must recover the same L. The coat reading 1/pi of it is #1450.
// Coat self-shadowing is left out on purpose: it multiplies the term by a
// transmittance that is 1 for an unshadowed coat, and GroomCoatTransmittance
// ParityTest owns it.
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

// Include paths resolve against assets/shaders/, NOT against this file. Same
// order as GroomStrand.glsl.
#include "include/GroomFibreCommon.glsl"
#include "include/PBRCommon.glsl"

layout(binding = 10) uniform samplerCube u_IrradianceMap; // TEX_USER_0

layout(location = 0) out vec4 o_Result;

// THE FIXTURE, mirrored in GroomEnvironmentFurnaceTest.cpp.
const vec3 kAlbedo = vec3(0.8, 0.5, 0.2);
const float kRoughness = 0.5;

// The brown fibre GroomFibreAmbientParityProbe.glsl uses, derived the same way
// so the C++ side can build it with MakeGroomFibreParams.
OloGroomFibre oloProbeFibre()
{
	OloGroomFibre fibre;
	fibre.SigmaA = vec3(0.5447, 0.9061, 1.7810);
	fibre.Eta = 1.55;
	fibre.Intensity = 1.0;
	fibre.HSamples = 4;

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
	return fibre;
}

void main()
{
	int column = int(gl_FragCoord.x);
	vec3 result = vec3(0.0);

	if (column < 4)
	{
		// Four cube faces' worth of directions, so a producer that got one
		// face wrong cannot hide behind the other five.
		const vec3 kDirections[4] = vec3[4](vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), vec3(0.0, 1.0, 0.0),
		                                    normalize(vec3(-1.0, 1.0, 1.0)));
		const float kSinThetaO[4] = float[4](0.0, 0.3, 0.6, 0.85);

		OloGroomFibreLobes response = oloGroomFibreAmbientResponse(oloProbeFibre(), kSinThetaO[column]);
		vec3 averageRadiance = oloGroomFibreEnvironmentRadiance(u_IrradianceMap, kDirections[column]);
		result = oloGroomFibreSum(response) * averageRadiance;
	}
	else if (column == 4)
	{
		vec3 N = vec3(0.0, 0.0, 1.0);
		vec3 V = normalize(vec3(0.3, 0.0, 1.0));
		result = calculateLightProbeAmbient(texture(u_IrradianceMap, N).rgb, kAlbedo, 0.0, kRoughness, N, V);
	}
	else if (column == 5)
	{
		result = oloGroomFibreEnvironmentRadiance(u_IrradianceMap, vec3(0.0, -1.0, 0.0));
	}

	o_Result = vec4(result, 1.0);
}
