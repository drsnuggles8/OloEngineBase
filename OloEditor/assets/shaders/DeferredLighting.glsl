// =============================================================================
// DeferredLighting.glsl - Deferred lighting composition pass (full)
// Part of OloEngine Deferred Renderer
//
// Reads the G-Buffer produced by PBR_GBuffer{,_Skinned}.glsl and
// evaluates opaque PBR lighting for every visible fragment. Output is
// linear HDR RGBA16F ready for post-processing.
//
// Expected texture bindings (match ShaderBindingLayout TEX_GBUFFER_*):
//   slot 43 — RT0  RGBA8   albedo.rgb + metallic
//   slot 44 — RT1  RGBA16F octNormal.xy + roughness + AO
//   slot 45 — RT2  RGBA16F emissive.rgb + material flags
//   slot 46 — RT3  RG16F   screen-space velocity (unused in lighting)
//   slot 47 — depth (D32F)
//   slot 69 — RT5  RGBA16F baked lightmap irradiance + coverage (issue #865)
//
// The per-pixel shading body lives in include/DeferredLightingShared.glsl
// so the MSAA variant (DeferredLighting_MSAA.glsl) can re-use the exact same
// math on per-sample inputs without code drift.
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
#include "include/DeferredLightingFragment.glsl"
