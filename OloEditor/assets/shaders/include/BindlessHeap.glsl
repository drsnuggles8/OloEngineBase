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
// and with entirely plausible-looking output.
//
// MUST MATCH ShaderBindingLayout::HEAP_OFFSET_TABLE_VEC4S — which covers the
// texture slots AND the image region above them, not just
// MAX_ENGINE_TEXTURE_SLOTS. This was 16 (texture slots only) while the engine
// wrote 18, so every image unit indexed past the end of the array. It survived
// because `BindlessHeapGpuTest` declares its own inline copy of this block with
// the right size: a test that restates a shared declaration cannot detect that
// the shared one is wrong.
layout(std140, binding = 56) uniform OloHeapOffsetBlock
{
    uvec4 g_OloHeapOffsets[18];
};

#define OLO_HEAP_OFFSET(texSlot) (g_OloHeapOffsets[(texSlot) >> 2][(texSlot) & 3])

// A poisoned or never-written slot holds 0, which is not a valid handle. Reading
// through it yields zero rather than the previous tenant's texels — the property
// RHI::DescriptorHeap's poison mode exists to produce, and the reason a
// use-after-free renders as a deterministic black instead of a plausible
// wrong texture.
#define OLO_HEAP_SAMPLER_2D(offset) sampler2D(g_OloResourceHeap[offset])
// Integer sampler — an entity-ID / index target, not a colour one. Needed
// because a handle carries no type: `sampler2D(h)` and `isampler2D(h)` are
// different reinterpretations of the SAME uvec2, and picking the float one for
// an R32I texture reads garbage rather than failing. JumpFlood_Init is the
// worked example (issue #691 Phase 3, bucket 1).
#define OLO_HEAP_ISAMPLER_2D(offset) isampler2D(g_OloResourceHeap[offset])
// …and the unsigned form, for an R16UI/R32UI lookup table. GTAO's Hilbert curve
// LUT is the worked example.
#define OLO_HEAP_USAMPLER_2D(offset) usampler2D(g_OloResourceHeap[offset])
// -----------------------------------------------------------------------------
// THE NULL OFFSET HAS A TYPE, so a non-2D constructor must not use the 2D one.
//
// Slot 0 is a 1x1 2D texture. `samplerCube(g_OloResourceHeap[0])` is therefore a
// type mismatch, and ARB_bindless_texture makes using a handle whose sampler type
// does not match the texture's target UNDEFINED — not black, undefined. Every
// UNSET input lands on slot 0 (an unset environment probe, an unset IBL map, a
// scene with no shadow cascade), so this is the common case rather than an edge
// one, and it reads as a plausible frame that changes with whatever ran before.
// It cost four wrong diagnoses as an "order-dependent" pop (issue #691 Phase 3).
//
// THE SUBSTITUTION LIVES HERE BECAUSE ONLY THE SHADER KNOWS THE TYPE. A TEX_*
// slot does not imply one — TEX_USER_0..2 are generic slots that different
// shaders declare as cube or 2D — so the C++ cannot pick the right null for the
// shared table, while the declaration always can. The comparison is against a
// value the whole draw shares, so it is uniform control flow.
//
// MIRRORS RHI::kNull*HeapOffset in RHIDescriptorHeap.h; pinned by
// RHIBoundaryRatchet.HeapOffsetIsShaderVisibleAndHandlesAreComparable.
#define OLO_HEAP_NULL_2D 0u
#define OLO_HEAP_NULL_CUBE 2u
#define OLO_HEAP_NULL_ARRAY 3u
#define OLO_HEAP_NULL_ARRAY_SHADOW 4u

#define OLO_HEAP_TYPED_NULL(offset, typedNull) (((offset) == OLO_HEAP_NULL_2D) ? (typedNull) : (offset))

#define OLO_HEAP_SAMPLER_2D_ARRAY(offset) \
    sampler2DArray(g_OloResourceHeap[OLO_HEAP_TYPED_NULL(offset, OLO_HEAP_NULL_ARRAY)])
#define OLO_HEAP_SAMPLER_2D_ARRAY_SHADOW(offset) \
    sampler2DArrayShadow(g_OloResourceHeap[OLO_HEAP_TYPED_NULL(offset, OLO_HEAP_NULL_ARRAY_SHADOW)])
#define OLO_HEAP_SAMPLER_3D(offset) sampler3D(g_OloResourceHeap[offset])
#define OLO_HEAP_SAMPLER_CUBE(offset) \
    samplerCube(g_OloResourceHeap[OLO_HEAP_TYPED_NULL(offset, OLO_HEAP_NULL_CUBE)])

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
#define OLO_HEAP_TEX_2D_INT(texSlot) OLO_HEAP_ISAMPLER_2D(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_2D_UINT(texSlot) OLO_HEAP_USAMPLER_2D(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_2D_ARRAY(texSlot) OLO_HEAP_SAMPLER_2D_ARRAY(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_2D_ARRAY_SHADOW(texSlot) OLO_HEAP_SAMPLER_2D_ARRAY_SHADOW(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_3D(texSlot) OLO_HEAP_SAMPLER_3D(OLO_HEAP_OFFSET(texSlot))
#define OLO_HEAP_TEX_CUBE(texSlot) OLO_HEAP_SAMPLER_CUBE(OLO_HEAP_OFFSET(texSlot))

// -----------------------------------------------------------------------------
// PER-MATERIAL OFFSETS — the second offset SOURCE (#691 Phase 3, amendment (32)).
//
// A material's textures change per draw, so routing them through the shared
// g_OloHeapOffsets table would mean re-uploading that whole table every draw —
// the exact per-draw cost bindless exists to remove. Their offsets instead ride
// in PBRMaterialUBO, which the draw path already uploads per material, so the
// nine `glBindTextureUnit` calls disappear for free.
//
// The declaring shader owns `u_MaterialHeapOffsets[3]` as the LAST member of its
// PBRMaterialUBO block; these macros only name the lanes, so the index layout
// lives in exactly one place on each side (CommandDispatch::WriteMaterialHeapOffsets
// mirrors it).
#define OLO_MATERIAL_ALBEDO_OFFSET u_MaterialHeapOffsets[0].x
#define OLO_MATERIAL_METALLIC_ROUGHNESS_OFFSET u_MaterialHeapOffsets[0].y
#define OLO_MATERIAL_NORMAL_OFFSET u_MaterialHeapOffsets[0].z
#define OLO_MATERIAL_AO_OFFSET u_MaterialHeapOffsets[0].w
#define OLO_MATERIAL_EMISSIVE_OFFSET u_MaterialHeapOffsets[1].x
#define OLO_MATERIAL_ENVIRONMENT_OFFSET u_MaterialHeapOffsets[1].y
#define OLO_MATERIAL_IRRADIANCE_OFFSET u_MaterialHeapOffsets[1].z
#define OLO_MATERIAL_PREFILTER_OFFSET u_MaterialHeapOffsets[1].w
#define OLO_MATERIAL_BRDF_LUT_OFFSET u_MaterialHeapOffsets[2].x
#define OLO_MATERIAL_DIFFUSE_OFFSET u_MaterialHeapOffsets[2].y
#define OLO_MATERIAL_SPECULAR_OFFSET u_MaterialHeapOffsets[2].z

// The sampler constructors for them. Unlike OLO_HEAP_TEX_*, these take an OFFSET
// straight from the material block rather than a TEX_* slot — there is no table
// indirection because there is no shared table involved.
#define OLO_MATERIAL_TEX_2D(offset) OLO_HEAP_SAMPLER_2D(offset)
#define OLO_MATERIAL_TEX_CUBE(offset) OLO_HEAP_SAMPLER_CUBE(offset)

// -----------------------------------------------------------------------------
// STORAGE IMAGES — the second descriptor kind (#691 Phase 3).
//
// GL image units and texture units are separate namespaces that BOTH start at
// zero, so image unit `u` lives at table index OLO_HEAP_IMAGE_BASE + u. Without
// the rebase a compute pass writing image unit 0 would overwrite TEX_DIFFUSE's
// offset and each would silently render the other's resource. The base must
// match ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE.
#define OLO_HEAP_IMAGE_BASE 64u
#define OLO_HEAP_IMAGE_OFFSET(imgUnit) OLO_HEAP_OFFSET(OLO_HEAP_IMAGE_BASE + uint(imgUnit))

// Pass this as `mem` for an image you both read and write, or for one with no
// memory qualifier at all.
//
// DO NOT PASS `readonly`. The macro DECLARES AND INITIALISES a local, and
// initialising a readonly variable is a write — `error C7504`. A read-only image
// has to stay on the slot-based path (a plain `layout(binding = N) readonly
// uniform image2D`) until that is expressible. Four compute shaders silently
// fell back to the slot path before this was understood.
#define OLO_HEAP_IMAGE_RW

// Declare a bindless storage image. `fmt` must match the format the CPU side
// passed to HeapBinding::BindImageOrOffset — a storage image's format is part of
// its binding contract, not something either side may infer.
//
// The local is function-scoped, so it must be declared in EVERY function that
// touches the image, not once at file scope like a uniform.
#define OLO_HEAP_IMAGE(fmt, mem, type, name, imgUnit) \
    layout(fmt) mem type name = layout(fmt) type(g_OloResourceHeap[OLO_HEAP_IMAGE_OFFSET(imgUnit)])

#endif // OLO_BINDLESS

#endif // OLO_BINDLESS_HEAP_GLSL
