// =============================================================================
// BindlessHeap.glsl — the shader side of the heap-bindless binding model.
//
// Issue #691 Phase 3, ADR 0011 §1.1. Include this and index the heap instead of
// declaring a `layout(binding = N) uniform sampler2D`:
//
//     #include "include/BindlessHeap.glsl"
//     ...
//     vec4 albedo = texture(OLO_HEAP_SAMPLER_2D(u_AlbedoOffset), uv);
//
// where `u_AlbedoOffset` is a plain `uint` the pass wrote into a UBO with
// `RHI::OffsetOf(view)`. That is the whole model: what dies is the ACT of
// binding, and the TEX_* number survives promoted from a compile-time constant
// to runtime data.
//
// -----------------------------------------------------------------------------
// READ THIS BEFORE ADDING THE INCLUDE TO A PRODUCTION SHADER.
//
// A shader that enables OLO_BINDLESS CANNOT travel the engine's normal compile
// path. Every production shader goes
//
//     GLSL -> shaderc(target = vulkan 1.2) -> SPIR-V
//          -> SPIRV-Cross -> GLSL 450
//          -> shaderc(target = opengl 4.5) -> OpenGL SPIR-V
//          -> glShaderBinary / glSpecializeShader
//
// and `GL_ARB_bindless_texture` is a GLSL-only extension that predates SPIR-V
// with no representation in the Vulkan target environment. The first hop
// rejects it. Pinned by
// OloEngine/tests/Rendering/PropertyTests/BindlessShaderPipelineTest.cpp, which
// fails if that ever changes — in either direction.
//
// So this file is written to be INERT unless `OLO_BINDLESS` is defined. With it
// undefined — which is every production shader today — it declares nothing, adds
// no extension directive, and compiles away to whitespace, so including it costs
// a shader nothing and breaks no existing path. The bindless branch is exercised
// by BindlessHeapGpuTest, which compiles it with `glShaderSource` against a live
// context and renders through it.
//
// -----------------------------------------------------------------------------
// WHY uvec2 AND NOT uint64_t.
//
// A `GLuint64` texture handle IS a `uvec2` in memory, so the CPU-side mirror
// uploads verbatim with no packing step, and `sampler2D(uvec2)` is provided by
// the extension itself. Declaring the block as `uint64_t[]` would additionally
// require `GL_ARB_gpu_shader_int64` in every consuming shader, buying nothing.
//
// The binding point is `ShaderBindingLayout::SSBO_RESOURCE_HEAP` (45). It is
// bound ONCE PER FRAME, never per draw — that is the performance argument for
// bindless, and re-binding it per draw would give it back.
// =============================================================================

#ifndef OLO_BINDLESS_HEAP_GLSL
#define OLO_BINDLESS_HEAP_GLSL

#ifdef OLO_BINDLESS

// NOTE: `#extension GL_ARB_bindless_texture : require` is NOT here, and that is
// deliberate. GLSL requires every `#extension` directive to precede all
// non-preprocessor tokens, so putting it in this file would force every
// including shader to place the `#include` above its first declaration — a
// per-file rule that is invisible until a driver rejects it, across ~35 shaders.
// `OpenGLShader::CreateProgramFromRawGLSL` injects the directive immediately
// after `#version` instead, together with `#define OLO_BINDLESS 1`. Include this
// file wherever it reads best.

// Must match ShaderBindingLayout::SSBO_RESOURCE_HEAP. Unsized on purpose: the
// heap's capacity is a runtime decision (OpenGLDescriptorHeapBackend sizes the
// buffer), and baking a size here would put a second copy of that number in
// every shader that indexes it.
layout(std430, binding = 45) readonly buffer OloResourceHeapBlock
{
    uvec2 g_OloResourceHeap[];
};

// The offset table. Indexed by the SAME `TEX_*` constant a slot-based shader
// would have written in `layout(binding = N)` — that reuse is what makes the two
// variants of one shader structurally unable to disagree about which texture is
// which, and it is ADR 0011 §1.1's "the number survives, promoted from a
// compile-time constant to runtime data" made literal.
//
// std140 pads a `uint` array to a 16-byte stride, so this is uvec4-shaped and
// indexed [i >> 2][i & 3]. Declaring it `uint g_Offsets[64]` instead would read
// every fourth entry and sample three wrong textures out of four — silently,
// and with entirely plausible-looking output. Must match
// ShaderBindingLayout::MAX_ENGINE_TEXTURE_SLOTS rounded up to a multiple of 4.
layout(std140, binding = 56) uniform OloHeapOffsetBlock
{
    uvec4 g_OloHeapOffsets[16];
};

#define OLO_HEAP_OFFSET(texSlot) (g_OloHeapOffsets[(texSlot) >> 2][(texSlot) & 3])

// A poisoned or never-written slot holds 0, which is not a valid handle. Reading
// through it yields zero rather than the previous tenant's texels — the property
// RHI::DescriptorHeap's poison mode exists to produce, and the reason a
// use-after-free renders as a deterministic black instead of a plausible
// wrong texture.
#define OLO_HEAP_SAMPLER_2D(offset) sampler2D(g_OloResourceHeap[offset])
#define OLO_HEAP_SAMPLER_2D_ARRAY(offset) sampler2DArray(g_OloResourceHeap[offset])
#define OLO_HEAP_SAMPLER_2D_ARRAY_SHADOW(offset) sampler2DArrayShadow(g_OloResourceHeap[offset])
#define OLO_HEAP_SAMPLER_3D(offset) sampler3D(g_OloResourceHeap[offset])
#define OLO_HEAP_SAMPLER_CUBE(offset) samplerCube(g_OloResourceHeap[offset])

// The forms a converted shader actually uses. Take the TEX_* slot number, not a
// heap offset — the indirection through the table is the whole mechanism:
//
//     #ifdef OLO_BINDLESS
//     #define u_NoiseTexture OLO_HEAP_TEX_2D(21)
//     #else
//     layout(binding = 21) uniform sampler2D u_NoiseTexture;
//     #endif
//
// …after which the shader BODY is byte-identical between the two variants,
// which is what keeps a conversion reviewable.
#define OLO_HEAP_TEX_2D(texSlot) OLO_HEAP_SAMPLER_2D(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_2D_ARRAY(texSlot) OLO_HEAP_SAMPLER_2D_ARRAY(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_2D_ARRAY_SHADOW(texSlot) OLO_HEAP_SAMPLER_2D_ARRAY_SHADOW(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_3D(texSlot) OLO_HEAP_SAMPLER_3D(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_CUBE(texSlot) OLO_HEAP_SAMPLER_CUBE(OLO_HEAP_OFFSET(texSlot))

#endif // OLO_BINDLESS

#endif // OLO_BINDLESS_HEAP_GLSL
