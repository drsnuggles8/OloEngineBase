#type vertex
#version 460 core
#ifdef OLO_VULKAN
layout(std430, binding = 57) readonly buffer OloVertexPull { float v[]; } b_Vertices;
#else
layout(location = 0) in vec3 a_Position;
#endif
void main()
{
#ifdef OLO_VULKAN
    int base = gl_VertexIndex * 5;
    vec3 a_Position = vec3(b_Vertices.v[base], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
#endif
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core
layout(binding = 47) uniform sampler2DMS u_GBufferDepthMS;
layout(binding = 12) uniform isampler2DMS u_GBufferEntityMS;
layout(location = 0) out int o_EntityID;
void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    gl_FragDepth = texelFetch(u_GBufferDepthMS, pixel, 0).r;
    o_EntityID = texelFetch(u_GBufferEntityMS, pixel, 0).r;
}
