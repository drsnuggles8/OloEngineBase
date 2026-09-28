// =============================================================================
// DeferredLighting_MSAA.glsl — per-sample deferred lighting composition.
//
// Selected by DeferredLightingPass when GBuffer::GetSampleCount() > 1 AND
// DeferredSettings::PerSampleLighting is true. Samples each G-Buffer
// attachment with sampler2DMS / texelFetch per sub-sample, evaluates full
// PBR lighting per sample, and averages the final HDR colour. This avoids
// the shading-rate collapse of a resolve-before-light approach where MSAA
// would only affect geometric edge samples of the depth/normal during
// G-Buffer write but not the shading itself.
//
// Shares the per-pixel shading body with the non-MSAA variant via
// include/DeferredLightingShared.glsl so there is a single source of truth
// for the PBR math.
// =============================================================================

#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// the pipeline has no vertex-input state at all. Binding 57 is the engine-wide
// vertex-pull binding (ShaderBindingLayout::SSBO_VERTEX_PULL); the root struct
// carries this buffer's device address, so the SAME 20-byte
// {vec3 position, vec2 uv} stream the attribute path consumes is read by index
// instead. OLO_VULKAN is defined only on the Vulkan shaderc route; the GL
// branch below is untouched.
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
#include "include/DeferredLightingFragment_MSAA.glsl"
