#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
void main() { gl_Position = vec4(a_Position.xy, 0.0, 1.0); }

#type fragment
#version 460 core
// GroomAmbientShaderTest's probe (#1618): include/GroomAmbient.glsl's one-pass
// pair against LightProbeSampling.glsl's sampleLightProbeGrid, both directions,
// over a grid of positions in and around the test's probe volume and of normals
// spelled from the pixel's coordinates.
#include "include/PBRCommon.glsl"
#include "include/LightProbeSampling.glsl"
#define OLO_AMBIENT_LADDER_EXPLICIT_CONTROLS
#include "include/AmbientLadder.glsl"
#include "include/GroomAmbient.glsl"

layout(binding = 10) uniform samplerCube u_ProbeTestIrradiance;

layout(location = 0) out vec4 o_LeanFront;
layout(location = 1) out vec4 o_LeanBack;
layout(location = 2) out vec4 o_RefFront;
layout(location = 3) out vec4 o_RefBack;
layout(location = 4) out vec4 o_PairFront;

void main()
{
	float x = floor(gl_FragCoord.x);
	float y = floor(gl_FragCoord.y);
	// Positions from just outside the volume's low corner to just outside its
	// high one, so the grid crosses cells, edges and the volume's boundary.
	vec3 worldPos = vec3(-0.5 + x * (5.0 / 63.0), -0.25 + y * (2.5 / 63.0), 0.3 + 0.01 * mod(x + y, 13.0));
	float a = x * 0.37 + y * 0.11;
	float b = y * 0.23 - x * 0.05;
	vec3 n = normalize(vec3(cos(a) * cos(b), sin(b), sin(a) * cos(b)));

	vec3 leanFront;
	vec3 leanBack;
	oloGroomProbeGridPair(worldPos, n, leanFront, leanBack);
	o_LeanFront = vec4(leanFront, 1.0);
	o_LeanBack = vec4(leanBack, 1.0);
	o_RefFront = vec4(sampleLightProbeGrid(worldPos, n), 1.0);
	o_RefBack = vec4(sampleLightProbeGrid(worldPos, -n), 1.0);

	vec3 pairFront;
	vec3 pairBehind;
	oloGroomAmbientPair(worldPos, n, n, u_ProbeTestIrradiance, false, true, 1.0, pairFront, pairBehind);
	o_PairFront = vec4(pairFront, 1.0);
}
