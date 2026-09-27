// =============================================================================
// DeferredIndirectSpecular.glsl - the reflection tiers' input (issue #1325)
// Part of OloEngine Deferred Renderer
//
// The fragment stage of DeferredLighting.glsl, compiled with its other outputs.
// Writes, into the IndirectSpecular framebuffer:
//   location 0 -- rgb = the indirect specular term the lighting composed into
//                 SceneColor (linear HDR radiance, occluded and tinted as it was)
//   location 1 -- rgb = that term per unit of incident radiance (unitless)
// RayTracedReflection.glsl and PostProcess_SSR.glsl replace the first with the
// second times their own radiance, and leave every other term of the colour
// alone (ADR 0020 section 1, include/ReflectionTierComposite.glsl).
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
#define OLO_DEFERRED_INDIRECT_SPECULAR_OUTPUT 1
#include "include/DeferredLightingFragment.glsl"
