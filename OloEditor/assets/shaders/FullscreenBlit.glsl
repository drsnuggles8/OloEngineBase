#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// binding 57 is the engine-wide vertex-pull binding; the root struct carries
// this buffer's device address, so the SAME 20-byte {vec3 position, vec2 uv}
// stream the attribute path consumes is read by index instead. OLO_VULKAN is
// defined only on the Vulkan shaderc route; the GL branch below is untouched.
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

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). The BODY is
// byte-identical between the two variants — only the declaration moves, and it
// names the same binding number the pass binds with.
#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#else
layout(binding = 0) uniform sampler2D u_Texture;
#endif

// DRS bounds: xy = (renderWidth / physicalWidth, renderHeight / physicalHeight).
// Clamp screen-space UVs to [0, bounds] so the upscale blit never samples
// uninitialised texels beyond the DRS-rendered region.
// Both components are 1.0 when DRS is inactive (scale == 1.0).
layout(std140, binding = 33) uniform DRSParams
{
    vec2 u_RenderScaleBounds;
    vec2 _drsPad;
};

void main()
{
    vec2 uv = min(v_TexCoord, u_RenderScaleBounds);
    o_Color = texture(u_Texture, uv);
}
