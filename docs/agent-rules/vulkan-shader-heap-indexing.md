# Vulkan-only shaders may index the descriptor heap themselves

**Rule: a shader that already needs a Vulkan-only extension (`GL_EXT_ray_query`) may sample
material textures by indexing the descriptor heap with `GL_EXT_descriptor_heap`; every shader with an
OpenGL twin keeps classic `layout(binding = N)` declarations.** ADR 0011 amendment (95). This is the
capability issue #805 asked for, scoped to the shaders where it costs the "one SPIR-V serves both
backends" property (amendment (50)) nothing, because those shaders never had a GL twin.

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
device address (`StorageBuffer::kNoBinding`), and does not touch the records.

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
