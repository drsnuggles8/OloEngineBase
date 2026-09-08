// =============================================================================
// DescriptorHeapTextures.glsl — runtime-indexed material textures for a shader
// that has NO OpenGL twin (ADR 0011 amendment (95); the capability issue #805
// asked for, scoped to the ray-query consumers).
//
// A ray-query hit can land on any material, so shading it from textures needs
// the shader to pick a descriptor at runtime. VK_EXT_descriptor_heap exposes
// the bound resource and sampler heaps to GLSL as `descriptor_heap` arrays;
// with `descriptor_stride = 1` their index is a BYTE offset into the heap, so
// the CPU hands the shader plain byte offsets (HeapBinding::
// ResolveShaderHeapTexture / ResolveShaderHeapSampler) and the heap's slot
// region and descriptor size stay the backend's business.
//
// RULES.
//   * Only a shader that already needs a Vulkan-only extension may include
//     this. Every shader with a GL twin keeps classic bindings — that is
//     amendment (50), and this file does not reopen it.
//   * The including shader must put `#extension GL_EXT_descriptor_heap` and
//     `#extension GL_EXT_nonuniform_qualifier` at its top, next to its ray
//     query extension: GLSL requires every #extension to precede all other
//     tokens, which an include mid-file cannot satisfy (BindlessHeap.glsl's
//     note).
//   * Outside OLO_VULKAN the sampler compiles to a constant so the shader
//     still builds through a validator; no backend other than Vulkan runs it.
// =============================================================================

#ifndef OLO_DESCRIPTOR_HEAP_TEXTURES_GLSL
#define OLO_DESCRIPTOR_HEAP_TEXTURES_GLSL

#define OLO_HEAP_OFFSET_INVALID 0xFFFFFFFFu

#ifdef OLO_VULKAN
layout(descriptor_heap, descriptor_stride = 1) uniform texture2D g_OloHeapTexture2D[];
layout(descriptor_heap, descriptor_stride = 1) uniform sampler g_OloHeapSampler[];

// One texel fetch at an explicit LOD. A ray hit has no screen-space
// derivatives, so the LOD is the caller's decision (the reference tracer
// samples level 0, as its CPU twin does). Both indices are per-fragment
// divergent (they come from the hit's material), hence nonuniformEXT.
vec4 oloHeapSampleLod(uint textureByteOffset, uint samplerByteOffset, vec2 uv, float lod)
{
    return textureLod(sampler2D(g_OloHeapTexture2D[nonuniformEXT(textureByteOffset)],
                                g_OloHeapSampler[nonuniformEXT(samplerByteOffset)]),
                      uv, lod);
}
#else
vec4 oloHeapSampleLod(uint textureByteOffset, uint samplerByteOffset, vec2 uv, float lod)
{
    return vec4(1.0);
}
#endif

#endif // OLO_DESCRIPTOR_HEAP_TEXTURES_GLSL
