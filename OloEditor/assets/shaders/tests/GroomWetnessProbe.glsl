#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
void main() { gl_Position = vec4(a_Position.xy, 0.0, 1.0); }

#type fragment
#version 460 core
// GroomWetnessShaderTest's probe (#1615): include/GroomWetness.glsl's three
// functions over a grid of inputs spelled from the pixel's integer coordinates,
// which GroomWetnessShaderTest spells the same way for the C++ twins.
#include "include/GroomWetness.glsl"
layout(location = 0) out vec4 o_Scalars;
layout(location = 1) out vec4 o_Point;
void main()
{
	float x = floor(gl_FragCoord.x);
	float y = floor(gl_FragCoord.y);
	float height = (x - 32.0) * 0.004;
	float rain = y < 32.0 ? 0.0 : 0.3;
	float wetness = mod(x * 7.0 + y * 3.0, 64.0) / 63.0;
	OloGroomWetScales scales = oloGroomFibreWet(wetness, vec4(2.2, 0.5, 0.4, 0.0));
	o_Scalars = vec4(oloGroomWetness(height, rain, 0.05, 0.04), scales.SigmaA, scales.V0, scales.S);

	float t = y / 63.0;
	vec3 local = vec3((x - 32.0) * 0.0005, (y - 32.0) * 0.0003, 0.005 + 0.0004 * y);
	vec3 gravity = normalize(vec3(mod(x, 7.0) - 3.0 + 0.25, mod(y, 5.0) - 2.0, mod(x + y, 9.0) - 4.0 + 0.5));
	vec3 clump = mod(x, 3.0) < 0.5 ? vec3(0.0) : vec3(0.004, -0.002, 0.03);
	o_Point = vec4(oloGroomWetLocal(local, t, wetness, gravity, clump, vec4(0.6, 0.15, 0.5, 0.0)), 1.0);
}
