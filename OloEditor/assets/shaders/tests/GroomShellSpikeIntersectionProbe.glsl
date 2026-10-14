#type vertex
#version 460 core
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
#type fragment
#version 460 core
#include "tests/include/GroomShellSpikeIntersection.glsl"
layout(location = 0) out vec4 o_Position;
void main()
{
    float distance = 1.0 + floor(gl_FragCoord.x) * 8.0;
    vec3 ray = normalize(vec3(-1.0, 0.07, -0.3));
    vec3 perpendicular = normalize(vec3(-ray.y, ray.x, 0.0));
    vec3 origin = -ray * distance + perpendicular * 0.10;
    vec3 position = vec3(0.0);
    bool hit = oloGroomShellSpikeIntersect(origin, ray, 0.11, true, position);
    o_Position = vec4(position, hit ? 1.0 : 0.0);
}
