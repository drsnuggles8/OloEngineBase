# A sampler array binding needs one heap index per element, never slot adjacency

**Rule.** On the Vulkan backend, a binding declared as an array — `uniform sampler2D
u_Textures[32]` — must be mapped with
`VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT`, and the root struct must
carry **one heap index per element**. Never map it with the scalar
`..._HEAP_WITH_INDIRECT_INDEX_EXT`: that source derives element *i* from
`base + i * heapArrayStride`, which is a silent requirement that the array's N textures occupy
N **consecutive** heap slots. Nothing in this engine allocates them that way.

If you add an array binding, `VulkanShaderBinding::ArrayCount` must be reflected from SPIR-V,
`VulkanRootDataLayout::Build` must reserve `4 * N` bytes for the image indices (and another
`4 * N` for the sampler indices of a combined-image-sampler), and `AssembleRootData` must write
element *i* from texture unit `binding + i`. All four move together; three of the four compile
and render fine on their own.

## The failure it caused (issue #1078)

`Renderer2D_Quad.glsl` is the engine's only sampler-array shader, and it is what draws every 2D
and UI quad. Its 32 textures were bound with `BindTexture(unit, …)`, which stages each unit's
slot independently in `VulkanBindingState`; the slots themselves come from
`VulkanDescriptorSlotCache`, which allocates from a LIFO free list or a bump allocator and
promises nothing about adjacency.

A freshly built batch is a run of cache misses in bind order, so the bump allocator hands out
consecutive slots and `base + i` lands on the right descriptors **by accident**. Everything
renders correctly, and has since the array path shipped.

The accident ends the first time any one of those textures gets a non-adjacent slot. An in-place
texture hot reload is the cheapest way to cause it: the reload frees the old image's slot and the
new image takes whatever the free list offers. Measured on `DriftMenu.olo`:

| | unit 0 (white) | unit 1 (backdrop) | what `u_Textures[1]` read |
|---|---|---|---|
| before reload | slot 5171 | slot **5172** | 5171+1 = 5172 ✓ correct by accident |
| after reload | slot 5171 | slot **5161** | 5171+1 = **5172** ✗ |

Slot 5172, just freed, was re-tenanted every frame by transient images — often `VK_FORMAT_R8_UNORM`
storage targets. Sampling an R8 image as `sampler2D` yields `(r, 0, 0, 1)`, so the menu backdrop
rendered **flat red**; when the tenant happened to be a zeroed target it rendered black. The colour
changed from run to run, which is the signature of reading a recycled allocation.

## Why every instrument said the code was fine

This is the part worth remembering. Each of these was measured, and each was green:

- **The image content was correct** — a `GetData()` readback immediately after the reload returned
  the new pixels byte-exact.
- **The descriptor was correct** — the texture's own slot got exactly one descriptor write, for
  exactly the right image, and was never rewritten or freed.
- **The staged binding was correct** — `BindTexture` staged that slot every frame.
- **No validation error fired**, and none could: the descriptor at slot 5172 is a perfectly valid
  descriptor. It just describes a different image.
- **A headless `Texture2D::Create` → `Reload()` → `GetData()` round-trip passed**, because
  `GetData` copies out of the VkImage and never goes through the heap at all.

The defect lives in the *arithmetic between* the staged slot and the descriptor the shader reads,
which no single component owns and no assertion covered.

## The test that catches it, and the one that does not

A regression test must sample **array element 1 or higher** with that element's texture on a
deliberately non-adjacent slot. Element 0 resolves correctly under both the broken and the fixed
mapping, so a test that samples `u_Textures[0]` — or any test using a scalar `sampler2D`, which
*is* element 0 — passes with the defect fully present. That is the
[substituted seam](substituted-seams-compound.md) here.

`VulkanTextureInPlaceReloadTest.cpp` case
`ASamplerArrayElementReadsItsOwnSlotNotAnAdjacentOne` does it: bind unit 0, let a decoy texture
claim the slot immediately after it, then bind the probe to unit 1 and assert the probe's colour.

## Related

- The reload half of #1078 is `VulkanTexture2D::Reload()`, the Vulkan twin of the OpenGL fix from
  #544; path resolution is shared through `Texture2D::ResolveStoredSourcePath` (#1067).
- [no-silent-fallbacks.md](no-silent-fallbacks.md): the same shape — a path that cannot do its job
  produced a plausible frame instead of saying so.
