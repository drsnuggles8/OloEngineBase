// Shader copy permits RGBA8 albedo to RGBA16F scene-color conversion on Vulkan.
#type vertex
#version 450 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): V3 fullscreen-triangle pull (20 B
// {vec3 position, vec2 uv} => 5 floats per vertex) on the engine-wide vertex
// binding 57 — byte-identical to FullscreenBlit.glsl's branch. The uv lane is
// declared but unused here (this shader texelFetches by gl_FragCoord), exactly
// as on the GL side.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;
#define OLO_PULLED_VERTEX 1
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;
#endif

void main()
{
#ifdef OLO_PULLED_VERTEX
    int vertBase = gl_VertexIndex * 5;
    vec3 a_Position = vec3(b_Vertices.v[vertBase + 0], b_Vertices.v[vertBase + 1], b_Vertices.v[vertBase + 2]);
#endif
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 450 core

layout(location = 0) out vec4 o_Color;

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). This is a G-Buffer
// CONSUMER (a debug viewer), not a producer — it declares no o_GBuffer*/gAlbedo
// outputs, so CreateProgramFromRawGLSL's misroute guard does not apply.
#ifdef OLO_BINDLESS
#define u_GAlbedo OLO_HEAP_TEX_2D(43)   // TEX_GBUFFER_ALBEDO
#else
layout(binding = 43) uniform sampler2D u_GAlbedo;    // RT0: albedo.rgb + metallic.a
#endif

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);
    vec4 g0 = texelFetch(u_GAlbedo, coord, 0);
    o_Color = g0;
}
