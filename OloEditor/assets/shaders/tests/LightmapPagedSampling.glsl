#type vertex
#version 460 core

#ifdef OLO_VULKAN
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 5;
    vec3 position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
    v_TexCoord = vec2(b_Vertices.v[base + 3], b_Vertices.v[base + 4]);
    gl_Position = vec4(position, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}
#endif

#type fragment
#version 460 core

layout(location = 0) out vec4 o_Color;
layout(location = 0) in vec2 v_TexCoord;

#include "include/LightmapSampling.glsl"

layout(std140, binding = 2) uniform TestLightmapRegions
{
    vec4 u_FirstRegion;
    vec4 u_SecondRegion;
    vec4 u_ThirdRegion;
};

void main()
{
    vec4 region = v_TexCoord.x < (1.0 / 3.0)
        ? u_FirstRegion
        : (v_TexCoord.x < (2.0 / 3.0) ? u_SecondRegion : u_ThirdRegion);
    o_Color = sampleLightmapIrradiance(vec2(0.5), region);
}
