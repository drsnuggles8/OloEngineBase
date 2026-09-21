# A diagnostic view is extracted after the LAST writer, from its own graph node

**Rule.** When a pass produces a resource that later passes also write, any debug extraction of
that resource belongs in a **separate render-graph node registered after the last writer** — never
in a tail of the first writer's `Execute`. Give the extraction a declared `Read` on the resource so
the ordering is a graph edge, and record which version it read so a future reordering is visible
without anyone having to look at the picture.

## The failure this prevents

The deferred G-Buffer has four writers:

| order | node | what it writes |
|---|---|---|
| 1 | `ScenePass` | the opaque MRT geometry |
| 2 | `VirtualGeometryPass` | cluster raster into the borrowed G-Buffer |
| 3 | `DeferredGPUOcclusionPass` | the two-phase occlusion cull's disoccluded statics |
| 4 | `DeferredOpaqueDecalPass` | projected opaque decals |

Only the first is `ScenePass`. The other three are graph nodes the scheduler runs **after**
`ScenePass::Execute` has returned, so a blit at the tail of that `Execute` — which is where the
G-Buffer debug channels lived until issue #1329 — captured attachments that three later passes then
changed. The "Albedo" view showed the surface with no decals on it; the "Normal" view showed it
before the virtual-geometry clusters landed. Nothing crashed, nothing logged, and the image looked
entirely plausible: a decal that is genuinely missing from the G-Buffer and a decal that was written
after the picture was taken are the same picture.

That is worse than having no debug view, because the view is consulted precisely when something
else is already suspected, and it quietly exonerates the stage that is wrong.

The fix is `GBufferDebugPass` (`OloEngine/src/OloEngine/Renderer/Passes/GBufferDebugPass.cpp`),
registered in `RenderPipelineBuilderScene.cpp` immediately before `DeferredLightingPass` — the last
point at which the G-Buffer is still the version lighting will consume.

## Three things that come with it

**1. Register unconditionally; gate in `Execute`.** The node's `Setup` declarations must not depend
on the selected debug channel. Graph topology is fingerprinted and cached, so a declaration behind a
runtime toggle is culled for the whole session the first time the toggle is off — see
[technique-selection-seams.md](technique-selection-seams.md) and #1315. `GBufferDebugPass::Setup`
declares its reads and its `SceneColor` write whenever a G-Buffer exists; `Execute` returns early on
channel 0.

**2. The resolve belongs to the last WRITER, not to the reader.** In MSAA per-sample mode the colour
attachments stay multisample for `DeferredLighting_MSAA`, so the single-sample copy a debug blit
reads only exists because something called `GBuffer::Resolve`. `ScenePass` calls it before the
decals, so an extraction that relies on that resolve reads a pre-decal copy even when the extraction
itself runs late. The obvious fix — resolve in the extracting pass — is wrong twice over: a
diagnostic that writes the resource it reports on is a reason the frame differs, and the L5 hazard
validator rejects it outright (five WAR hazards against `VirtualShadowMapMarkPass`, which reads the
same handles earlier in the frame). It belongs in `DeferredOpaqueDecalPass`, the last G-Buffer
writer, which already declares those writes — and unconditionally in that mode, because which
readers are enabled this frame is not something the writer should have to know.

**3. Record the version, not a "this is final" flag.** `GBuffer::MarkWritten` bumps a content
version and names the writer; `DebugViewProvenanceRegistry` stores the frame, the pass and the
version each extraction read. Two numbers that must agree beat one boolean that says they do: when
someone later reorders the deferred chain, the numbers stop agreeing and
`GBufferDebugFinalWriterEvidenceTest` fails, rather than the boolean staying true because it is set
by the pass that was moved.

## Averaging is correct for radiometry and wrong for everything else

The same resolve carries three kinds of channel and they do not share a rule:

- **Continuous attributes** (albedo, normals, roughness, emissive RGB) — average them; that is what
  MSAA is.
- **Packed material flags** (G-Buffer RT2's alpha) — a bitfield. An average of two valid codes
  decodes to a material nobody authored. `GBuffer::ResolveFlagsLane` puts one real sample's value
  back after the blit (issue #996).
- **Integer picking IDs** (RT4, `RED_INTEGER`) — the GL spec resolves an integer format by selecting
  a single sample rather than averaging, so the hardware is on the right side here. That is a
  guarantee worth *asserting* rather than assuming: `GBufferResolveLaneScene` renders the scene with
  and without MSAA and requires the set of IDs present after the resolve to be a **subset** of the
  set present without it. Any new value was invented by an average, and the test names it.

The subset rule is the reusable part. It needs no knowledge of the encoding, no threshold to tune,
and it fails on exactly the defect it is about.

## ...and it has to reach the screen on every backend

Ordering it correctly is half the job. The other half is that a diagnostic which renders nothing
looks exactly like a renderer that drew nothing, and on Vulkan the G-Buffer debug channels rendered
nothing for a reason that had no symptom: **`glBlitFramebuffer` converts between formats and
`vkCmdCopyImage` does not.**

G-Buffer RT0 is `RGBA8` and RT3 is `RG16F`; the scene colour target is `RGBA16F`. On GL the blit
converts silently. The Vulkan lowering refused the mismatch — correctly, because a copy would
reinterpret bits rather than convert values — and took the `UnimplementedStub` arm: a warn-once, a
no-op, and a viewport left at its `0.1` clear, which tone-maps to a flat 85/85/85 frame.

`vkCmdBlitImage` is the converting primitive, and `VulkanRendererAPI::BlitFramebuffer` now takes
that arm when the formats differ. Three things it needs that the copy arm does not:

- **Colour only, single-sampled.** A converting depth/stencil blit is not a thing, and a converting
  resolve is two operations.
- **`VK_FORMAT_FEATURE_BLIT_SRC_BIT` / `BLIT_DST_BIT` are optional.** Ask
  `vkGetPhysicalDeviceFormatProperties` rather than assume; refusing loudly beats a validation error
  on a device that does not offer them.
- **The barrier stage is `BLIT`, not `COPY`.** This is the one that bites. The transitions the copy
  arm stages are correct for `vkCmdCopyImage`, and sync validation reports the mismatch as a
  `READ_AFTER_WRITE` on the source and a `WRITE_AFTER_WRITE` on the destination — never as anything
  about stages. The regression test
  (`VulkanPassSuite.BlitFramebufferConvertsAnRgba8ColourIntoAnRgba16FTarget`) found it because the
  fixture fails a test on a non-zero validation count, not because the pixels were wrong; the pixels
  were already right.

**The general rule: a format-converting blit is a GL-ism.** Before relying on one, check whether the
source and destination formats actually match — and if a Vulkan path renders nothing while GL is
fine, grep the log for `[RHI/Vulkan]` and a `no-op` before looking anywhere else. The stub warns
once per entry point and then only counts, so a frame captured later in the session shows the
symptom with the explanation already scrolled away.

## Where to look

- `OloEngine/src/OloEngine/Renderer/Passes/GBufferDebugPass.{h,cpp}` — the extraction node.
- `OloEngine/src/OloEngine/Renderer/Debug/DebugViewProvenance.{h,cpp}` — the capture record.
- `OloEngine/src/OloEngine/Renderer/GBuffer.h` — `MarkWritten` / `GetWriteVersion`.
- `OloEngine/tests/Rendering/PropertyTests/GBufferDebugFinalWriterEvidenceTest.cpp` — the decal,
  the MSAA arms, the provenance and the subset rule.
- `OloEngine/src/Platform/Vulkan/VulkanRendererAPI.cpp` — the converting-blit arm of
  `BlitFramebuffer`, and `VulkanPassSuiteTest.cpp`'s value-level regression for it.
