# Which declarations may index the descriptor heap, and which may not

**Rule: two sets of declarations index the heap with `GL_EXT_descriptor_heap`, and nothing else
does.** A shader with no OpenGL twin (one that already needs `GL_EXT_ray_query`) may reach ANY
texture that way — ADR 0011 amendment (95). A shader that HAS a GL twin may convert only its **five
material-local maps** (albedo, metallic-roughness, normal, AO, emissive), and only on its Vulkan
arm — amendment (96), issue #805. Every other declaration in every shader keeps classic
`layout(binding = N)` on both backends.

The two scopes exist for different reasons. (95) costs the "one SPIR-V serves both backends"
property (amendment (50)) nothing, because those shaders never had a GL twin. (96) spends part of
it deliberately, for the five declarations whose runtime index is the payoff — and it is affordable
because a partial conversion is expressible on Vulkan (§4).

## 1. How a shader reaches a texture

```glsl
#extension GL_EXT_descriptor_heap : require       // at the top, next to GL_EXT_ray_query
#extension GL_EXT_nonuniform_qualifier : require
#include "include/DescriptorHeapTextures.glsl"    // the two heap arrays + oloHeapSampleLod
vec4 c = oloHeapSampleLod(textureByteOffset, samplerByteOffset, uv, 0.0);
```

The arrays are declared with `descriptor_stride = 1`, so the index is a **byte offset** into the
heap. The CPU hands those out: `HeapBinding::ResolveShaderHeapTexture(handle)` and
`HeapBinding::ResolveShaderHeapSampler(desc)` (`HeapBindingSeam.h`), which the Vulkan backend answers
from the same slot cache the draw path binds textures through, so a texture the raster frame samples
and one a ray-query shader samples are one descriptor. OpenGL answers `Invalid`; a consumer that gets
`Invalid` shades untextured and counts it (`GpuPathTracerStats::TexturesAvailable`).

The resolvers fork on the **backend**, not on the engine heap's `OLO_RHI_BINDLESS` lever: that lever
routes the draw path's binding through the engine heap and is off by default, and the GPU Scene
material records' `*HeapOffset` fields stay unresolved under it. `MaterialTextureTable` therefore
resolves the records' texture handles itself, one 16-byte record per material slot, uploaded by
device address (`StorageBuffer::kNoBinding`), and does not touch the records. A table whose bytes
changed is published through a **fresh** `StorageBuffer`, never by `SetData` into the one a
submitted frame may still read through `GetDeviceAddress()`; dropping the old `Ref` hands its
allocation to the backend's deferred reclaim, which keeps it alive for `kFramesInFlight` completed
frames. `EmissiveTriangleTable` follows the same rule ([gpu-path-tracer.md](gpu-path-tracer.md)).

## 2. What must hold

- **The extension directives go at the shader's top.** GLSL requires every `#extension` to precede
  all other tokens; an include mid-file cannot satisfy that (the BindlessHeap.glsl lesson).
- **Reflection ignores the heap arrays.** SPIRV-Cross reports no resource for a `descriptor_heap`
  declaration, so the pipeline builder's binding mapping needs nothing and cannot be told anything.
  The heap is bound per recording already (`VulkanRendererAPI::PrepareDraw`).
- **A masked material is confirmed per candidate.** `RayTracingScene` builds `Masked` instances
  with `FORCE_NO_OPAQUE`; the shader traces with `gl_RayFlagsNoneEXT` and confirms candidates through
  `RayTracingAlphaTest.glsl`'s `oloRayTracingConfirmCandidate`, defining `OLO_RT_SAMPLE_ALPHA` first.
  With textures unreachable it traces opaque and counts `MaskedGeometryTracedAsSolid`.
- **Only textures at rest.** The resolver builds the draw path's whole-image sampled view at
  `SHADER_READ_ONLY_OPTIMAL`, where an uploaded texture rests. A render target is not resolvable
  here: only a recording can move its layout.
- **A sampler state is minted from one conversion.** `VulkanSamplerHeap::CreateInfoFromDesc` serves
  the draw path and the resolver; the integer-format `NEAREST` override stays with the draw path
  because it depends on the image.

## 3. What bit

- **The compiler has to be new enough.** Vulkan SDK 1.4.357.0's glslang compiles
  `GL_EXT_descriptor_heap`; the hosted CI runners were pinned to 1.4.321.0, whose glslang does not, and `ShaderCompilation.AllProductionShaders-
  CompileUnderVulkanTarget` failed with "extension not supported" on every Windows job while the box
  with the newer SDK passed. The pin lives in `.github/actions/setup-vulkan/action.yml`; the
  self-hosted runners carry their own SDK. An old SDK fails loudly there, by design: the engine does
  not skip a shader it cannot compile.
- **The GLSL syntax is `layout(descriptor_heap) uniform texture2D name[]`**, plus
  `descriptor_stride`. Neither `resourceHeapEXT` built-ins nor `layout(resource_heap)` exist;
  glslang reports them as undeclared identifiers, not as an unsupported extension, which reads as a
  typo. The SPIR-V carries `BuiltIn ResourceHeapEXT` and `ArrayStrideIdEXT` and validates for
  Vulkan 1.4; the device needs `descriptorHeap`, `shaderUntypedPointers` and, for the `nonuniformEXT`
  index, `shaderSampledImageArrayNonUniformIndexing`. The last one was missing, and the failure is a
  VUID-08740 at `vkCreateShaderModule`, not a compile error; `VulkanDevice` enables it when the
  device offers it and the resolvers refuse when it does not.
- **Region offsets are not descriptor multiples.** The heap's slot region starts at an aligned
  offset that need not be a multiple of the descriptor size, so an index in descriptors cannot
  address a slot. `descriptor_stride = 1` and byte offsets side-step the whole question.

## 4. The material arm (amendment (96)) — what makes a PARTIAL conversion legal

**A heap array carries no binding decoration, so converting one declaration does not disturb the
others.** Measured on SDK 1.4.357.0: in one fragment shader, `layout(descriptor_heap,
descriptor_stride = 1)` arrays of `texture2D`, `textureCube` and `sampler` compile alongside classic
`layout(binding = 8/12/33)` samplers, `spirv-val` clean for Vulkan 1.4. The heap arrays lower to two
variables decorated `BuiltIn ResourceHeapEXT` and `BuiltIn SamplerHeapEXT`, with **no `Binding` and
no `DescriptorSet` decoration**; the classic samplers keep theirs unchanged, so their
`VkDescriptorSetAndBindingMappingEXT` entries are untouched.

**The GL bindless route has no such property, and that asymmetry is the thing to remember.**
`Shader::IsBoundProgramBindless()` is a property of the whole PROGRAM, so the seam withholds EVERY
bind and one unconverted sampler reads black (§5c). A GL conversion is all-or-nothing; a Vulkan one
is per-declaration.

### How a material shader takes the arm

```glsl
#type fragment
#version 460 core
#ifdef OLO_VULKAN                              // at the TOP — before any other token
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : require
#define OLO_MATERIAL_VULKAN_HEAP_READER 1      // read by PBRCommon.glsl AND by VulkanShader
#endif
...
#ifdef OLO_MATERIAL_VULKAN_HEAP_READER
#include "include/DescriptorHeapTextures.glsl"
#define u_AlbedoMap OLO_HEAP_MATERIAL_TEX_2D(OLO_MATERIAL_ALBEDO_OFFSET, OLO_MATERIAL_SAMPLER_OFFSET)
#elif defined(OLO_BINDLESS)                    // the GL arm, unchanged
...
#else                                          // classic declarations
```

`#ifdef` is not a token, so guarding the `#extension` directives keeps them legal while hiding them
from the GL tier, which compiles the same source at vulkan_1_2 without the macro.

### What bit, or would have

- **A combined sampler cannot cross a function call.** `sampler2D(texture2D, sampler)` must appear
  at its point of use; glslc rejects `sampleAlbedo(u_AlbedoMap, ...)` with `'call argument' :
  sampler constructor must appear at point of use`. `GL_ARB_bindless_texture`'s `sampler2D(uvec2)`
  has no such restriction, so this is the one place the two bindless arms cannot share a spelling.
  `PBRCommon.glsl`'s `OLO_MAT_*` macros exist for it: on GL and the slot path they expand to the
  helper call unchanged, and only the Vulkan arm inlines the `texture()`.
- **Every lane must name a REAL descriptor.** An out-of-range heap index is undefined behaviour, not
  a black texel, so "no map" and "could not resolve" both get
  `HeapBinding::ResolveShaderHeapNullTexture()`. The shader's `Use*Map` gate is not sufficient on its
  own: `PBR_GBuffer` takes that flag from the GPU Scene material record, not from the UBO the CPU
  just corrected, so the OFFSET has to be safe by itself.
- **The offset is not memoisable, and the material UBO is cached.** The slot cache keys on `VkImage`,
  and a texture reloaded in place keeps its `RHI::ResourceHandle` while getting a new one — so a
  stored offset names a different descriptor and the frame renders a **plausible wrong texture**.
  `HeapBinding::ShaderHeapGeneration()` moves on exactly the events that can reassign a slot, and
  `CommandDispatch`'s material-UBO cache key carries it alongside the heap epoch.
- **A sampler offset is a SECOND index.** `GL_ARB_bindless_texture` bakes sampler state into its
  handle; `GL_EXT_descriptor_heap` does not. One lane (`HeapOffsets[2].w`) serves all five, because
  every material 2D descriptor is minted with `HeapBinding::MaterialTexture2DSampler()` — it is
  frame-uniform, not per-material. On GL that lane means nothing and stays null.
- **Only textures AT REST convert**, which is why the scope stops at the five: the environment
  cubemap and the IBL trio are baked into render targets, and this resolver cannot move a layout.
- **A shared stage BODY cannot grant the arm: state the opt-in once per ENTRY POINT, and every
  entry point sharing that body must agree.** `include/VirtualGBufferFragment.glsl` holds the five
  declarations for both virtualized-geometry raster paths, so `VirtualMeshGBuffer.glsl` (MDI) and
  `VirtualMeshletGBuffer.glsl` (mesh shader) each `#define` the token themselves — `VulkanShader`
  asks the entry shader's own PRE-INCLUDE text, and the `#extension` directives must precede every
  token. Agreement is the sharp half: each program is self-consistent alone, but
  `VirtualGeometryPass` switches between them PER INSTANCE inside one `RecordParallel` loop, calling
  `UploadMaterialForDirectDraw` after each rebind, and `Shader::ReadsMaterialHeapOffsets()` is a
  process-wide flag written by the last `Bind` on any thread. Agreeing makes that flag safe;
  disagreeing lets a thread read the other route's answer, and the frame is plausible either way.

Pinned by three tests, all in `BindlessShaderPipeline`.
`VulkanMaterialHeapArmLeavesNoMaterialLocalSamplerDeclared`:
one of the five left declared classic on this arm is a sampler nothing binds.
`EntryShadersSharingAMaterialStageBodyAgreeOnTheHeapArm` is the rule above, found from the tree —
a header that DECLARES one of the five forces its includers to agree, and `PBRCommon.glsl`, which
only reads the token, is correctly not one. `TheMaterialHeapArmCoversExactlyTheRecordedFamilies`
records the converted set both ways, because losing a `#define` is SILENT: the shader falls back to
classic bindings, gets its five binds back, and renders correctly while the conversion stops
existing.

### Proving it in a frame: restart the editor, do not hot-reload

**`olo_shader_reload` does not rebuild a Vulkan GRAPHICS pipeline.** It answers `"ok": true,
"status": "ready"` and the frame does not change — measured by forcing the arm's albedo macro to flat
magenta and reloading, which returned a byte-identical capture. An A/B built on it measures nothing.
Restart the editor between arms; the same control after a restart turned the scene magenta, which is
what made the rest of the evidence trustworthy.

**A/B against `git`, not against a hand-disabled arm.** Commenting out
`OLO_MATERIAL_VULKAN_HEAP_READER` looks like it reproduces the pre-conversion shader and does not —
that variant textured every surface wrongly, which reads as "the classic path is broken" and is
really "this variant is neither arm". Check the base commit's shaders into the working tree
(`git checkout <base> -- OloEditor/assets/shaders/`); the C++ is inert without the token, so the
shader-only baseline needs no rebuild.

Measured that way on SDK 1.4.357.0 / RTX 4090, Sponza on both scenes: deferred **RMSE 0.0000**
(byte-identical) and forward **RMSE 0.0012** (max 1 LSB) against the base engine, with the magenta
control at RMSE 52.9 over 99.7% of pixels. OpenGL is unaffected (RMSE ≤ 0.05).
