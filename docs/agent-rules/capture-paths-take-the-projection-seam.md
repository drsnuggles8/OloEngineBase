# Every matrix a vertex stage feeds to `gl_Position` goes through `RHIProjectionSeam` — including a bake's private one

Applies to: anything that builds its own view-projection and renders off-screen outside the
render graph — impostor atlases, IBL and sky face bakes, DDGI captures, asset previews, thumbnail
rigs, any future "render N views of a mesh into a texture" helper.

## The rule

A projection the engine authors is in GL clip conventions (y up, z in [-1, 1]). Vulkan wants y
down and z in [0, w], and it **clips** outside that range before rasterization. The difference is
baked in at upload time by [`RHIProjectionSeam.h`](../../OloEngine/src/OloEngine/Renderer/RHI/RHIProjectionSeam.h)
and nowhere else, so shaders stay source-identical across backends. A path that computes its own
matrix and uploads it raw has silently opted out.

Pick the variant by how the result is **read**, not by how it is drawn:

| The consumer addresses the result by | Use | Effect on Vulkan |
|---|---|---|
| screen uv (a viewport, a full-screen pass) | `AdjustProjectionForBackend` | y flip + z remap |
| **direction** (cubemap face, octahedral atlas tile) | `AdjustCaptureProjectionForBackend` | z remap only — rows stay GL-identical |
| shader arithmetic against uv/depth | `AdjustProjectionForShaderReconstruction` | y flip only |

Both are identity on OpenGL, so adding the call cannot change a GL golden.

**A capture that skips the y flip must draw with culling disabled**, as every face bake in the
engine does. Without the flip the framebuffer-space facing determinant is the opposite of what
the screen path composes, so a culling bake drops on Vulkan exactly the faces GL keeps. Do not
compensate by hand-flipping winding at the call site; the seam header explains why that is a bug
this engine has already shipped once.

## The story (issue #1264)

`ImpostorBaker` rendered each of the 64 octahedral tiles with
`ubo.ViewProjection = glm::ortho(...) * glm::lookAt(...)`, uploaded raw. Its ortho puts the
mesh at ndc z ≈ −0.11. On Vulkan that is negative clip z: every triangle was clipped, the atlas
stayed at its clear colour, and every impostor card then failed its `coverage < cutoff` alpha
test against an empty atlas. The whole mesh-foliage canopy did not exist on Vulkan — while the
billboard layers beside it rendered normally and the same build on OpenGL drew a dense forest.

It survived because nothing was *wrong* in any way a counter could see: the atlas baked "successfully",
the instances generated, the draws were issued with a valid pipeline and a resolved vertex-pull
stream, and there were no validation errors. A canopy that renders nothing looks like sparse art.

## The diagnostic that found it — reuse this shape

Five plausible causes were each killed by one experiment before the right one surfaced. The
sequence that converged, cheapest first, all done by editing the runtime shader and restarting
the editor (Vulkan graphics pipelines do not hot-reload) with **OpenGL as the control at every
step**:

1. **Force the fragment to a solid colour, no discards.** Fills the screen on both backends →
   geometry and transforms are fine; the difference is in shading.
2. **Display the sampled texture raw across the geometry.** Tree tiles on GL, nothing on Vulkan →
   the *texture* is empty, not the binding (the same binding fed a normal texture in the same pass).
3. **Make the bake's clear colour opaque red.** The atlas sampled back red on Vulkan → framebuffer,
   copy and sampler all work; only the **draws** write nothing.
4. **Instrument the draw guards** (`PrepareDraw` / `BindIndexBufferFor`, once each — they have
   side effects). `prepareDraw=true bindIndex=true` for every tile, no missing-pull-occupant
   warning → the draws are issued, not dropped.

At that point "clear lands, draws issue, zero fragments" has one family of causes left — the
vertices leave the clip volume — and the raw ortho was found by asking what every *working*
capture path (`IBLPrecompute`, DDGI, `AssetPreviewRenderer`) did that this one did not.

The four falsified hypotheses, so nobody re-runs them: a UBO block declared with different sizes
in two shaders on one binding (values read back correct on both backends); depth rejection
(`SetDepthTest(false)` changed nothing — clipping precedes depth); stale scissor state (setting it
per tile changed nothing — clipping precedes scissor too); the outside-recording-bracket stub (it
warns once when hit, and never did).

## Checklist for a new off-screen render helper

- The matrix passed to the vertex stage went through the seam variant matching how the target is
  read (table above).
- Culling is explicitly set, not inherited from whatever the previous scene draw left behind.
- Depth/stencil state is explicitly set for the same reason.
- If the target is direction-addressed, the consumer shader derives its uv geometrically and does
  not assume screen row order.
- Verify on **both** backends by looking at the produced texture, not at whether the bake logged
  success — `ImpostorBakeEvidenceTest` reads the atlas back and asserts coverage; add the same for
  a new bake.
