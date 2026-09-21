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

**2. Resolve for yourself, after the late writers.** In MSAA per-sample mode the colour attachments
stay multisample for `DeferredLighting_MSAA`, so the single-sample copy a debug blit reads only
exists because something called `GBuffer::Resolve`. `ScenePass` calls it before the decals; an
extraction that relies on that resolve is reading a pre-decal copy even when the extraction itself
runs late. The extracting pass resolves in its own `Execute`.

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

## Where to look

- `OloEngine/src/OloEngine/Renderer/Passes/GBufferDebugPass.{h,cpp}` — the extraction node.
- `OloEngine/src/OloEngine/Renderer/Debug/DebugViewProvenance.{h,cpp}` — the capture record.
- `OloEngine/src/OloEngine/Renderer/GBuffer.h` — `MarkWritten` / `GetWriteVersion`.
- `OloEngine/tests/Rendering/PropertyTests/GBufferDebugFinalWriterEvidenceTest.cpp` — the decal,
  the MSAA arms, the provenance and the subset rule.
