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
   seam, compare the snapshot's SIZE against the buffer's, not just its contents.

   That comparison is now the mechanism's own invariant rather than a thing to
   remember: **a snapshot covers the whole buffer, or there is no snapshot**
   (#1080). `PushSnapshot` sizes every snapshot to `m_Size` and sources the bytes
   the write does not define from the live snapshot or the mapped persistent
   buffer; when neither can supply them it refuses, warns once and increments
   `VulkanStorageBuffer::GetSnapshotRefusedCount()`, dropping that buffer back to
   last-write-wins rather than handing a draw a short block. This matters because
   `GetRootDataAddress()` carries no length, so a shorter snapshot makes the
   shader's own indexing the only bound — and the index routinely comes from a
   buffer that was fed correctly (see
   [no-silent-fallbacks.md](no-silent-fallbacks.md) on why an indexable stand-in
   ranks worse than an unfed UBO).

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

## The same hole, left open in vertex buffers for two years (#1171)

**The audit above was done for uniform and storage buffers and not for vertex
streams.** `VulkanVertexBuffer::SetData` kept the write-through shape, under a
comment that said so out loud:

> `NOTE: mesh data is upload-once at load time. […] nothing in Waves A/B streams vertex data.`

`ParticleBatchRenderer::Flush` streams vertex data, and PrecipitationSystem's two
engine-init streams (5.76 MB and 3.84 MB) rewrite theirs every frame. So every
draw in the frame read the last batch — the identical last-write-wins failure
this document is about, in the one buffer family the fix skipped.

Two lessons worth more than the fix:

- **A comment asserting "nothing does X" ages into a bug** the moment something
  does, and nothing checks it. The original seam detected a second write
  inside one frame generation and warned; the current seam versions the first
  external write, before any draw can consume it.
- **Fixing a failure class in one buffer type is not fixing the class.** When
  amendment (80) gave UBOs and SSBOs per-write versioning, vertex buffers had
  the same facade, semantics and deferred execution. Ask which *other* types
  share the shape before closing such an issue.

That detector left a once-per-frame in-flight race and lost the first draw of
the frame that first had two writes. The current policy below replaces it.

## Current buffer-family audit (#1351)

The original second-write detector has been replaced: the first `SetData`
after construction makes a vertex-pull stream mutable. Every subsequent pull
draw uses an arena snapshot, including a stream written once per frame. The
constructor's initial upload alone leaves a static mesh on its persistent
address. A first rewrite after a persistent draw waits for earlier submissions;
if that draw was recorded in the current frame, the rewrite is refused rather
than silently changing its bytes. A partial first rewrite of a staged buffer
with constructor data is also refused when its unwritten tail cannot be read.

| Family and actual consumer | Producer and intended lifetime | Mechanism and remaining limit |
|---|---|---|
| UBO root addresses (`VulkanRendererAPI::AssembleAndPushRootData`) | CPU, per draw | `VulkanUniformBuffer` keeps a full CPU shadow and pushes a versioned, whole-buffer arena allocation. A refused snapshot returns zero; root assembly substitutes the null block and counts the unfed binding. |
| Bound SSBO root addresses, including model instances | CPU, per draw | `VulkanStorageBuffer::PushSnapshot` preserves the whole buffer across partial writes; `DynamicDrawExactUpload` uses a draw-bounded prefix. Draw consumption prevents overwriting a published snapshot. A staged partial write with no readable prior bytes refuses the snapshot and still has last-write-wins ordering; this remains an explicit unsupported path. |
| GPU Scene record tables read by compute | CPU records, persistent compute address for one published version | On Vulkan, each dirty `GPUScene::Upload` publishes a new complete SSBO allocation. Compute uses that version's persistent address, and the old allocation enters deferred reclaim after its last owner drops it. GL retains the incremental upload. A device test blocks an old-buffer copy on a timeline semaphore while publishing the next version, then checks the old bytes after its owner was dropped. Upload cost remains unmeasured. |
| Vertex-pull binding 57/63 | CPU, per draw for mutable streams; persistent for constructor-uploaded meshes | `GetPullAddress` gives mutable draws a per-version frame-arena snapshot. Arena refusal returns zero; root assembly counts the unfed binding and drops that draw before it can index the small null block. Vertex data used by BLAS still uses the persistent allocation; a simultaneous BLAS read and rewrite needs separate coverage. |
| Index buffer in `vkCmdBindIndexBuffer3KHR` | CPU at construction, persistent | `VulkanIndexBuffer` has no `SetData` API. Raw index arenas use `UploadBufferSubData`, a separate command-buffer copy path. |
| `InstanceBuffer` at SSBO 15 | CPU per batch | `DynamicDrawExactUpload` snapshots its uploaded prefix; `UploadRange` with a nonzero offset takes the whole-buffer rule. Count-to-byte overflow and out-of-capacity writes are refused, clearing the live count. The interleaved instance-draw device test exercises the actual binding. |
| GPU frustum-cull inputs, indirect seed and rejected counter | CPU per cull dispatch followed by compute-produced survivors and indirect args | `GPUFrustumCuller` now records input and seed copies through `UploadBufferSubData`; the command stream orders them with prior and later dispatches even when a pool slot is reused. The cull-to-indirect device suite covers the consumer, but a deliberately delayed pool-reuse test remains unrun. |
| GPU-produced `DynamicCopy` SSBOs, including particle counters and indirect args | GPU output plus occasional CPU seed, persistent | `DynamicCopy` refuses draw snapshots. `SetData` during a Vulkan recording now records an ordered transfer; `ClearData` records a fill. The two-dispatch seed test checks that two writes to one buffer feed different dispatches. Outside a recording bracket, `SetData` still uses a one-shot copy, so cross-queue previous-frame reuse needs owner-specific lifetime discipline. |
| Terrain GPU picker state and node-list seed | CPU query seed followed by GPU-produced indirect args and node lists | `TerrainGPUPicker` now seeds the persistent GPU-written buffers with `UploadBufferSubData`, which records a transfer and barriers inside the frame command buffer. A later pick's seed cannot overtake an earlier dispatch. A same-recording two-pick device test remains unrun. |
| Terrain virtual-texture bake requests and indirection parameters | CPU per compute dispatch, persistent compute address | `TerrainVirtualTexture` now records the bake request and each indirection-list/parameter update through `UploadBufferSubData`. Draw-only SSBO snapshots never protected these compute reads. A multi-mip Vulkan image-result test remains unrun. |
| VSM invalidation and caster-cull inputs | CPU per shadow update, compute reads | `VirtualShadowMap` now records CPU invalidation and cull-input copies in command order; GPU-written VSM buffers use `DynamicCopy` ordered seeds/fills. The existing Vulkan full-frame VSM test covers the consumer, but a deliberately delayed shadow frame remains unrun. |
| Foliage GPU culler layer input | CPU on registry-generation change, persistent compute read | `FoliageGPUCuller::BuildLayer` now allocates a fresh Vulkan layer buffer even when the new generation has the same byte count; the prior version retires through deferred reclaim. GL keeps same-size reuse. A delayed-frame cull test remains unrun. |
| GPU particle emit staging at SSBO 5 | CPU per compute dispatch | On Vulkan, each `EmitParticles` call creates a staging SSBO sized to that batch. The previous allocation retires through deferred reclaim, so two dispatches and adjacent frames cannot read the final CPU upload from the same mapped range. GL reuses one full-capacity buffer because its uploads order against dispatches. |
| GPU fluid emit staging and body proxies | CPU per solver step, compute reads | Vulkan allocates one emit staging SSBO per pending batch and one body-proxy SSBO per nonempty step; earlier versions retire through deferred reclaim. The GL path retains its existing buffer. `EmitCount` comes from the Fluid UBO, so no CPU write to the GPU-produced counters is needed. Vulkan refuses `SeedParticles` after the first dispatch because a direct reset could race GPU output; recreate the solver. The Vulkan device test covers two emit/step pairs, reset refusal, and shader reload followed by another emit/step. A delayed-frame fluid device test remains unrun. |
| Emissive triangle, material texture and shader-heap tables reached through device addresses | CPU on table change, persistent for an in-flight consumer | Each table allocates a fresh SSBO when its bytes change and publishes the new address. The old buffer enters `VulkanDeferredReclaim` and survives until completed frame generations drain. |
| RT skeletal palette reached by device address | CPU once per populated deformation frame, compute reads | `DeformedSurfaceCache::EnsurePaletteBuffer` publishes a fresh Vulkan `DynamicCopy` allocation even at stable capacity; the old allocation retires through deferred reclaim. GL retains its capacity-reuse upload. A delayed-frame palette-consumer test remains unrun. |
| RT surface vertex addresses (deformed, vegetation, groom) | GPU deformation or CPU groom conversion; persistent BLAS input | GPU deformation writes the persistent output by command, with explicit ordering needed before AS build. On Vulkan, `GroomSurfaceCache` publishes a fresh vertex allocation for each changed proxy while preserving the coat's logical GPU Scene identity. A same-shape, deformed vertex version refits its BLAS; a new index version rebuilds it. The old native buffer retires through deferred reclaim. GL continues to refill stable shapes in place. A delayed-frame BLAS-build test remains unrun. |

The per-dispatch GPU-particle and per-batch GPU-fluid staging allocations fix
their aliases, but still need L6 hot-path timing baselines before they are
treated as performance-safe.

The arena's frame slot is recycled only after its fence completes.
`FrameArenaAdjacentSlotSurvivesDelayedReadAndFenceGatedWrap` holds a slot-0
transfer read behind an unsignaled compute-queue semaphore, publishes into
slot 1, then checks the old bytes and wraps back to slot 0 after its fence.
The production frame loop waits for the slot fence before `BeginFrame`; the
fixture exercises the slots and delayed read directly, not that frame-loop
wait. A delayed GPUScene table read is also covered. Delayed palette, BLAS,
foliage, fluid and shadow consumers remain separate untested paths.

## And again in the GPU particle counters (#1171)

Third instance of the same shape, found two months later. `GPUParticleSystem::
Compact()` reset its counter block every frame with a `SetData` of a zeroed
struct — a CPU write into a buffer the frame's already-recorded dispatches
still read. The emit dispatch therefore saw `deadCount == 0`, took its
"no free slots" undo path, and the emitter produced **nothing**, on Vulkan only,
with no error anywhere. The remedy was already documented on
`StorageBuffer::ClearData`: **a clear stays in the GPU command stream on both
backends**, so it orders against the recorded dispatches the way GL's
`glNamedBufferSubData` does.

The rule that catches this without a debugger: **any CPU write to a buffer a
compute dispatch reads is suspect the moment more than one of them happens per
frame.** Reach for `ClearData` / `ClearSubData` when the write is a reset, and
for per-write versioning when it is real data.

Probing it has its own trap: every buffer in that chain is rewritten each frame
(counters by Compact, free list by CompactScatter), so three probe channels were
silently clobbered before one survived. A probe publishing through a buffer the
pipeline owns reads back the last writer's bytes, not the probe's.
