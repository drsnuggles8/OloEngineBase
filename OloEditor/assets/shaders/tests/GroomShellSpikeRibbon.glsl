#type vertex
#version 460 core
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Tangent;
layout(location = 3) in float a_Alpha;
layout(location = 4) in uint a_Id;
layout(location = 0) out vec3 v_Tangent;
layout(location = 1) out float v_Alpha;
layout(location = 2) flat out uint v_Id;
void main()
{
    gl_Position = vec4(a_Position, 1.0);
    v_Tangent = a_Tangent;
    v_Alpha = a_Alpha;
    v_Id = a_Id;
}
#type fragment
#version 460 core
#include "include/GroomStrandCommon.glsl"
layout(std140, binding = 30) uniform SpikeParameters
{
    mat4 u_VP;
    mat4 u_InverseVP;
    vec4 u_ViewportFrame;
    vec4 u_Layers;
    vec4 u_Chart;
};
layout(location = 0) in vec3 v_Tangent;
layout(location = 1) in float v_Alpha;
layout(location = 2) flat in uint v_Id;
layout(location = 0) out vec4 o_Metrics;
void main()
{
    float threshold = oloGroomStochasticHash(uint(gl_FragCoord.x), uint(gl_FragCoord.y), uint(u_ViewportFrame.z), v_Id, 1246u);
    if (v_Alpha <= threshold) discard;
    float cosine = dot(normalize(v_Tangent), normalize(vec3(0.4, 0.8, 0.3)));
    float radiance = 0.2 + 0.8 * sqrt(max(0.0, 1.0 - cosine * cosine));
    o_Metrics = vec4(1.0, radiance, radiance, 1.0);
}
