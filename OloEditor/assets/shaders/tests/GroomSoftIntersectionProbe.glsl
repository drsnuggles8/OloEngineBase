#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
void main() { gl_Position = vec4(a_Position.xy, 0.0, 1.0); }

#type fragment
#version 460 core
#include "include/GroomSoftIntersection.glsl"
layout(location = 0) out vec4 o_Color;
void main()
{
    float separation = (floor(gl_FragCoord.x) - 8.0) * 0.001;
    float t = floor(gl_FragCoord.y) / 63.0;
    float n = 0.05;
    float f = 100.0;
    mat4 p = mat4(1.0);
    p[2][2] = -(f + n) / (f - n);
    p[2][3] = -1.0;
    p[3][2] = -2.0 * f * n / (f - n);
    p[3][3] = 0.0;
    float deviceDepth = (p[2][2] * -3.0 + p[3][2]) / 3.0 * 0.5 + 0.5;
    float perspective = oloGroomViewDistance(deviceDepth, p);
    p = mat4(1.0);
    p[2][2] = -2.0 / (f - n);
    p[3][2] = -(f + n) / (f - n);
    deviceDepth = (p[2][2] * -3.0 + p[3][2]) * 0.5 + 0.5;
    o_Color = vec4(oloGroomIntersectionCoverage(separation, 0.01, t, 0.25),
                   oloGroomIntersectionCoverage(separation, 0.01, t, 0.0),
                   perspective, oloGroomViewDistance(deviceDepth, p));
}
