#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
void main() { gl_Position = vec4(a_Position, 1.0); }

#type fragment
#version 460 core
layout(location = 0) out vec4 o_Result;
mat4 u_Model = mat4(1.0);
mat4 u_PrevModel = mat4(1.0);
vec4 u_WindWeights = vec4(0.0);
vec4 u_WindFlags = vec4(0.0);
vec4 u_WindClock = vec4(2.0, 0.0, 0.0, 1.9);
vec4 u_ImpostorParams1 = vec4(0.0);
vec4 u_WindDirection = vec4(1.0, 0.0, 0.0, 10.0);
vec4 u_WindGust = vec4(1.0, 0.3, 0.0, 0.0);
float u_WindStrength = 2.0;
float u_WindSpeed = 1.0;
float u_Time = 2.0;
float u_PrevTime = 1.9;
float u_WindHistoryValid = 1.0;
#include "include/FoliageInstanceGeometry.glsl"
#include "include/FoliageWind.glsl"

void main()
{
    int column = int(gl_FragCoord.x);
    vec3 pivot = vec3(10.0, 0.0, 20.0);
    vec3 vertex = vec3(0.7, 0.8, 0.2);
    if (column == 0)
    {
        vec3 actual = foliageWindOffset(vertex, pivot, pivot.xz, 1.0, u_Time);
        vec3 legacy = foliageLegacyWindOffset(pivot.xz, u_Time, u_WindSpeed, u_WindStrength, vertex.y);
        o_Result = vec4(actual - legacy, 1.0);
        return;
    }
    if (column == 7)
    {
        u_WindFlags.w = 1.0;
        u_WindDirection.w = 100.0;
        FoliageDeformation actual = foliageDeform(vec3(0.0), vertex, pivot, 1.0);
        float gust = 1.0 + u_WindGust.x * sin(u_WindClock.x * u_WindGust.y * 6.2831853 + pivot.x * 0.05);
        vec3 expected = u_WindDirection.xyz * (100.0 * gust) * u_WindStrength * vertex.y * 0.1;
        o_Result = vec4(actual.Current - expected, 1.0);
        return;
    }
    if (column == 8)
    {
        vec3 eye = vec3(5.0, 3.0, 10.0);
        vec3 previous = vec3(0.0);
        vec3 current = vec3(1.0, 0.0, 0.0);
        vec2 offset = vec2(2.0, 1.0);
        vec3 correct = foliageImpostorPoint(previous, eye, offset);
        vec3 stale = previous + foliageImpostorPoint(current, eye, offset) - current;
        o_Result = vec4(correct - stale, 1.0);
        return;
    }
    u_WindWeights = vec4(0.4, 1.0, 1.0, 0.0);
    u_WindFlags.w = 1.0;
    if (column == 1) vertex.y = 0.0;
    if (column == 3) u_PrevTime = u_Time;
    if (column == 4) u_WindHistoryValid = 0.0;
    if (column == 5)
    {
        vec3 a = foliageWindOffset(vertex, pivot, pivot.xz, 1.0, u_Time);
        vec3 b = foliageWindOffset(vertex, pivot, pivot.xz, 2.0, u_Time);
        o_Result = vec4(a - b, 1.0);
        return;
    }
    if (column == 6)
    {
        u_WindDirection.w = 20000.0;
        u_WindGust.x = 20000.0;
    }
    FoliageDeformation deformed = foliageDeform(vec3(0.0), vertex, pivot, 1.0);
    o_Result = (column == 2 || column == 3 || column == 4)
               ? vec4(deformed.Current - deformed.Previous, 1.0)
               : vec4(deformed.Current, 1.0);
}
