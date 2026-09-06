# Vulkan: a CPU buffer write between two recorded draws is a semantic, not a memcpy

Postmortem of the #691 screenshot-parity failure: two of three sandbox
scenes rendered **skybox-only** under Vulkan — every sphere, cube and floor
plane missing — while the third scene rendered perfectly, the full Vulkan pass
suite stayed green, and the log carried zero errors, zero validation messages
and zero dropped-draw counters.

## The failure

GL executes `glNamedBufferSubData` and the draws around it **in command
order**: upload A, draw 1, upload B, draw 2 means draw 1 samples A and draw 2
samples B, even though they name the same buffer. A naive Vulkan port of that
buffer (one `VkBuffer`, one life-stable device address, mapped write-through
in `SetData`) silently replaces that contract with **last-write-wins**: the
CPU writes land immediately, the recorded draws execute at submit, so *every*
draw in the frame reads upload B.

`CommandDispatch::DrawMeshInstanced` is exactly that shape: it re-uploads the
one shared `ModelInstanceBuffer` (SSBO 15, `InstanceData[]`) before **every**
auto-batched instanced draw. Under Vulkan all batches rendered with the final
batch's instance data — typically the last tiny line-gizmo batch — so their
geometry collapsed off-screen / degenerate.

## Why it was scene-specific (the trap that misdirects the diagnosis)

- **MaterialSpheres / LightingTest**: grids of the *same* sphere/cube mesh —
  CommandBucket auto-batches them into instanced draws → all gone.
- **VehiclesTest**: 18 entities of mostly *unique* meshes — nothing batches,
  every draw is single-instance, and the per-draw **UBO** path
  (`VulkanUniformBuffer`) was already arena-versioned per write → perfect
  frame.

A backend bug that keys on *how the content is submitted* masquerades as a
*content* bug ("something wrong with these scenes"). The suite stayed green
because no tenant interleaved two uploads of one SSBO with draws in a single
recording.

## How it was localized (the method is the lesson)

1. **Pinned-pose screenshot gate** (same scene, same camera, both backends,
   read the PNGs side by side) turned "Vulkan looks right" — true only on the
   one scene being watched — into a table of per-scene verdicts.
2. **`olo_render_capture_target` on `SceneDepth`** separated "draws never
   landed" from "post chain ate them": depth was far-plane everywhere except
   the debug-line meshes → geometry pass, not post.
3. The **survivor pattern** (unique-mesh draws and debug-line meshes work,
   repeated-mesh draws vanish) named the batching path, and reading the
   dispatch showed the shared-buffer re-upload.

## The fix (ADR 0011 amendment (80))

`VulkanStorageBuffer::SetData` inside a recording bracket pushes a snapshot of
the written range into the frame arena; draws embed the snapshot address via
`GetRootDataAddress()` (the storage twin of the UBO's root-data seam), so each
recorded draw keeps the bytes that were current when it was recorded. Three
deliberate scope edges:

- **Compute keeps the persistent address** (`commandOrderedBufferReads=false`
  in `AssembleAndPushRootData`): compute SSBOs are GPU-write participants
  (cull survivors, atomically-bumped indirect seeds) whose writes must land in
  the buffer the indirect-draw resolve reads, and the GPU-cull path already
  pools per-dispatch buffers.
- **Outside a recording bracket no snapshot is taken** — load-time /
  between-frames uploads would burn arena space for an ordering nobody
  observes.
- **`VulkanVertexBuffer` (vertex pull) has the same latent archetype** if a
  pull stream is ever rewritten mid-frame between draws. Renderer2D uploads
  each stream once per frame today; recorded as a seam, not fixed.

Tenant: `VulkanPassSuite.InterleavedInstanceBufferUploadsKeepCommandOrderAcrossDraws`
(upload LEFT, draw, upload RIGHT, draw — both quads must land).

## The mirror-image failure: snapshotting a buffer the GPU produces (#1052 / #1058)

The versioning above is only correct for a buffer whose **producer is the CPU**.
Applied to a GPU-produced one it inverts. `PushSnapshot` now refuses outright for
`StorageBufferUsage::DynamicCopy`; this section is why.

Draws read `VulkanStorageBuffer::GetRootDataAddress()` — the snapshot when one is
live. Compute dispatches read `GetDeviceAddress()`, always the persistent buffer
(`AssembleRootData`'s `commandOrderedBufferReads` flag). So a CPU `SetData` on a
GPU-output buffer leaves the two halves disagreeing: the dispatch writes
persistent, and every later draw keeps reading the CPU's stale snapshot.

`VirtualMeshRegistry::PrepareFrame` zeroes the virtual-geometry draw-args buffer
every frame before the cull dispatches, which is exactly that shape. The
consequences split three ways, and only one of them showed:

| consumer | how it reads the count | result |
|---|---|---|
| hardware MDI | `vkCmdDrawIndexedIndirectCount` on the parameter `VkBuffer` | persistent — **correct** |
| software raster | it is a dispatch | persistent — **correct** |
| mesh-shader task stage | it is a **draw**, so root data | the zero snapshot → `EmitMeshTasksEXT(0)` — **rasterizes nothing** |

Nothing warned. `EmitMeshTasksEXT(0)` is a legal launch, so there was no dropped
draw, no validation error, no unfed binding and no stub hit — 4072 clusters
"drawn" into an empty frame. The class comment asserted the case away
("GPU-written buffers never SetData mid-frame"); zero-init before a dispatch *is*
a mid-frame SetData.

The fix is the `DynamicCopy` guard, and it did make the mesh arm render — and it
also lost the device, 3/3, on the sequence "switch to Deferred, then open the
scene". **That second fault is worth reading carefully, because the obvious
reading of it is wrong.**

A third consumer is missing from the table above: the software-raster
**visibility resolve** is a fullscreen *draw*, and the pass zeroes the SW work
list's 16-byte header mid-frame. So the resolve was reading a **16-byte** snapshot
of that list, finding `Count == 0`, and discarding every pixel — it had **never
executed on Vulkan at all**. Removing the snapshot ran it for the first time, and
what it did then had nothing to do with the mesh arm: `hwRasterMode=forcemdi`
faulted just as reliably, and `swRasterMode=disabled` was stable. See
[discard-is-not-a-bounds-check.md](discard-is-not-a-bounds-check.md) for the
actual defect.

**Three generalisable halves:**

1. A per-draw versioning mechanism must know which side produces the data — ask
   it of every buffer the mechanism covers, not just the one that motivated it.
2. A mechanism that hands shaders a *large mapped* stand-in hides bugs for years,
   so removing it looks like it caused the fault it revealed. A/B one build apart
   before believing either direction.
3. **A buffer whose CPU write is small can be worse than one that is wrong.** The
   16-byte snapshot of the SW list was not a stale value a shader misread; it was
   a whole consumer switched off in a way no counter could see. When auditing this
   seam, compare the snapshot's SIZE against the buffer's, not just its contents —
   which is its own open defect, #1080.

## The rule

When porting any GL-shaped facade to a deferred-execution backend, audit every
`SetData`/`Upload` call site for the pattern **"same buffer written more than
once per frame with draws recorded between the writes"**. Each such site needs
per-write versioning (arena snapshot, ring, or per-draw allocation) — a
persistent buffer with write-through *cannot* express it, and the failure is
silent, scene-shaped, and invisible to any tenant that doesn't interleave.

The GL-parity checklist that found this (screenshot gate → intermediate-target
capture → survivor-pattern reasoning) is reusable for any "backend X renders
scene Y wrong but scene Z right" report.
