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
// indexed [i >> 2][i & 3]. Declaring it `uint g_Offsets[72]` instead would read
// every fourth entry and sample three wrong textures out of four — silently,
// and with entirely plausible-looking output. Must match
// ShaderBindingLayout::HEAP_OFFSET_TABLE_VEC4S (= 18: 64 texture slots + 8
// image slots, / 4).
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

// =============================================================================
// STORAGE IMAGES — the second descriptor kind (ADR 0011 amendment (26)).
//
// THREE THINGS DIFFER FROM THE SAMPLER MACROS ABOVE, and each is forced:
//
// 1. THE INDEX SPACE. GL image units and texture units are separate namespaces
//    that both start at 0, so image unit `u` lives at OLO_HEAP_IMAGE_BASE + u in
//    the single offset table. The base is applied identically here and in
//    RGCommandContext::BindImageOrHeapOffset from
//    ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE, so a converted shader still
//    names the same image unit its bind named.
//
// 2. THE FORMAT IS PART OF THE TYPE, and so are the memory qualifiers.
//    `glGetImageHandleARB` bakes a format into the handle and GLSL requires a
//    matching format layout qualifier; `writeonly` / `readonly` / `coherent`
//    change what the compiler may assume. Both therefore have to travel into the
//    bindless declaration, which is why there is ONE parameterised macro rather
//    than a per-type family — the engine's storage images already span
//    image2D / image3D / image2DArray / uimage2D crossed with four qualifier
//    combinations, and a macro per cell is 16 of them.
//
// 3. IT DECLARES, IT IS NOT AN EXPRESSION. A `#define name <expr>` cannot work
//    here: the format qualifier belongs to a declaration, and an image variable
//    initialised from a buffer read is not a constant expression, so it cannot
//    live at global scope. The macro therefore emits a LOCAL declaration and
//    goes at the top of `main()` (or of the function that uses it):
//
//        #ifndef OLO_BINDLESS
//        layout(r32f, binding = 0) uniform writeonly image2D u_Output;
//        #endif
//
//        void main()
//        {
//        #ifdef OLO_BINDLESS
//            OLO_HEAP_IMAGE(r32f, writeonly, image2D, u_Output, 0);
//        #endif
//            imageStore(u_Output, coord, value);   // <- unchanged
//        }
//
//    The BODY still does not change, which is the property that keeps a
//    conversion reviewable — the declaration simply moves from file scope into
//    function scope, and the macro's arguments are the same tokens the bindful
//    declaration used, in the same order, minus `uniform` and `binding =`.
//
// A poisoned, cleared or failed image binding points at heap slot 1, a resident
// 1x1 R32F zero image. It is a SECOND reserved slot because slot 0 holds a
// SAMPLER handle, and building an image out of one is undefined in exactly the
// way building one out of zero is.
//
// The exact constructor spelling below (format qualifier on the declaration AND
// on the constructor, memory qualifier on the declaration only) is pinned by
// BindlessHeapGpuTest's TheImageConstructorSpellingTheHeaderUsesIsAcceptedByTheDriver,
// which probes the alternatives against a live driver and names the ones that
// work — so if this ever has to change, the test says what to change it to.
// =============================================================================

// Must match ShaderBindingLayout::HEAP_IMAGE_SLOT_BASE.
#define OLO_HEAP_IMAGE_BASE 64u
#define OLO_HEAP_IMAGE_OFFSET(imgUnit) OLO_HEAP_OFFSET(OLO_HEAP_IMAGE_BASE + uint(imgUnit))

// Pass as `mem` when the bindful declaration had no memory qualifier. An
// empty macro ARGUMENT is legal but not uniformly implemented across GLSL
// preprocessors, so this expands to nothing instead of relying on that.
//
// ALSO PASS IT WHERE THE BINDFUL DECLARATION SAID `readonly`. A bindless image
// is a LOCAL WITH AN INITIALISER, and initialising a `readonly` variable is a
// write — the driver rejects it outright:
//
//     error C7504: OpenGL does not allow writing to readonly variable 'i_AOInput'
//
// Dropping the qualifier is safe rather than a compromise: `readonly` is a
// promise the shader makes, not a capability it needs, and every format this
// engine binds as a storage image is in GL's required image-format list, so the
// variable is legally readable without it. The BINDFUL declaration keeps its
// `readonly` — only the bindless local loses it.
//
// `writeonly` and `coherent` are fine on an initialised local; only `readonly`
// is not. All four combinations are pinned by BindlessHeapGpuTest's spelling
// probe, which now enumerates the QUALIFIER SET as well as the qualifier's
// position — the gap that let this reach a live editor run.
#define OLO_HEAP_IMAGE_RW

// OLO_HEAP_IMAGE(fmt, mem, type, name, imgUnit)
//   fmt     format layout qualifier token: r32f, rgba8, r32ui, rgba16f, …
//   mem     memory qualifiers: writeonly | readonly | coherent | OLO_HEAP_IMAGE_RW
//   type    image2D | image3D | image2DArray | uimage2D | …
//   name    the variable name the shader body already uses
//   imgUnit the GL image unit the slot-based bind used — NOT a texture slot
#define OLO_HEAP_IMAGE(fmt, mem, type, name, imgUnit) \
    layout(fmt) mem type name = layout(fmt) type(g_OloResourceHeap[OLO_HEAP_IMAGE_OFFSET(imgUnit)])

#endif // OLO_BINDLESS

#endif // OLO_BINDLESS_HEAP_GLSL
