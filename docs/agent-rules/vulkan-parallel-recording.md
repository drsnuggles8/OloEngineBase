# Vulkan parallel command recording: the threading model, and how a pass opts in

**Rule:** a pass that wants its independent work items recorded on task workers calls
`RenderCommand::RecordParallel(count, body)` and gives every item its own resource objects.
Everything the Vulkan backend keeps per command buffer is per *recording context*, resolved
through a thread-local; everything the backend keeps per process is either read-only inside a
region or behind a mutex. The contract is ADR 0011 amendment (91); this file is the working
checklist and the reasons.

Issue #806. First conversion: `ShadowRenderPass` (CSM cascades and shadow-atlas entries).

## 1. What is per thread, what is per process

| State | Where it lives now | Who may touch it inside a region |
|---|---|---|
| Command buffer, rendering scope, pending clear, bind caches, recorded pipeline state, viewport, scissor, framebuffer selections (draw buffers **and** the depth-array layer), draw tallies | `VulkanRecordingContext` — one per item, one for the render thread (`VulkanRendererAPI::m_Main`) | the item that owns it |
| Bind-point mirror (`VulkanBindingState`), current program (`VulkanShader` / `VulkanComputeShader`) | the item's context; the process-wide object stays the render thread's | same |
| Image layouts (`VulkanImageLayoutTracker`) | an overlay per item over the render thread's tracker, merged at the join in item order | same; the base is frozen |
| Frame arena cursor | one atomic per slot; an item claims a 64 KiB block once and bumps inside it | any thread |
| Descriptor slot cache, sampler heap, pipeline builder, descriptor-heap null-slot memo, framebuffer depth-layer view cache, texture attachment view | key hashed outside the lock; a shared lock on the hit path, exclusive on a miss | any thread |
| Image info / root object / raw buffer registries, device limits, capabilities | read-only inside a region | any thread, reads only |
| Backbuffer, queries, conditional rendering, readbacks, mid-frame flush, one-shots, resource creation | render thread only | refused on an item: Debug asserts, otherwise warn-once |

`Ctx()` inside `VulkanRendererAPI` is the only place that decides which context a call is on.
It reads `CurrentVulkanWorkerContext()`; a `ScopedVulkanWorkerContext` sets that thread-local
for the duration of one item body. The render thread runs items too (`ParallelFor` gives the
calling thread a worker slot), which is why the test is "is a worker context set", never "am I
the render thread".

## 2. The fork, the join, and why the order is what it is

1. **Fork.** The primary's scope is closed and its pending clear materialised, so a clear the
   pass requested *before* the fork lands before any item's work. The bound target's
   attachments are transitioned to their attachment layouts on the primary. Each item context is
   seeded with the render thread's recorded state and its tracker is made an overlay.
2. **Record.** Every item's secondary command buffer is acquired and begun on the render thread
   first, from that item's own (frame slot, item) pool — a pool that cannot deliver declines the
   region into the one inline path. Then `ParallelFor` over the items: the body runs with the
   worker context set; its scope is closed and its clear materialised the way `EndRecording`
   does for a frame.
3. **Join.** Overlays merge into the render thread's tracker in **item order**; the secondaries
   are executed with one `vkCmdExecuteCommands` in **item order**; tallies are summed; the
   primary's command-buffer caches are reset (state is undefined after execute); the render
   thread adopts the last item's framebuffer selections, which is GL's sticky attachment state
   after a loop.

Item order everywhere is what makes the recorded stream identical to the inline loop, so a
pass can be checked on OpenGL (always inline) against Vulkan (forked) and must match.

## 3. The layout rule, and the pre-transition that makes shared targets legal

An item's barrier names an `oldLayout` read from the pre-fork state. If two items transition
the same subresource, the later one's `oldLayout` is stale. So: **two items may write the same
subresource only when every such write is an identity transition** (the layout it was in ==
the layout written). A non-identity write claims the subresource for the region at record time,
and a second item's claim asserts right there with both item indices (`VulkanLayoutClaimTable`);
the merge counts anything that slipped past, logs it every frame, and reports it in
`ParallelRecordingFrameStats::MergeConflicts` (visible through `olo_perf_pass_timings`). A
conflict is a bug in the pass that forked, never a backend condition to tolerate.

Cascades are disjoint by construction (one layer each). The atlas is not: every entry renders
into layer 0 with its own viewport. That is why the fork transitions the bound target's
attachments first: every entry's scope-open then finds the layer already in
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` and records an identity barrier. Materialise the atlas clear
**before** the fork for the same reason; a clear inside an item would fold into that item's
`loadOp` and wipe the other entries' tiles.

## 4. Converting a pass: the checklist

1. **Find every object the loop body writes.** `UniformBuffer::SetData`, `StorageBuffer::SetData`,
   `InstanceBuffer::Upload` version bytes per *object* (amendments (78), (80)); one object
   written from two items interleaves. Give the pass one object per item, created **outside**
   the region. The shadow pass keeps a camera UBO, an animation UBO and an instance buffer per
   cascade / entry.
2. **Find every process-wide thing the body touches.** `grep` the body for `static`, `s_`,
   `GetInstance`, `::Get()`. Then look one level down for lazy per-object builders: the first
   race the device test found was `VulkanShader::GetRootDataLayout`, built on first ask and
   asked by every draw, so four items drawing with one shader raced its `std::unique_ptr`.
   A lazy builder on a shared object needs the double-checked flag + mutex shape that one
   has now. Not parallel-safe at all: `HeapBinding::FlushOffsets` (one offset table, one UBO),
   `RendererProfiler`, a subsystem's own shared UBO (`FoliageRenderer::RenderShadows`), compute
   dispatch with file-static parameter UBOs (`VirtualGeometryShadow`). Move that work to a
   sequential tail after the join, or accumulate per item and publish after.
3. **Size every per-item buffer before the fork.** `InstanceBuffer::Upload` grows its storage
   when a batch outgrows it, and growing creates and reclaims GPU memory: refused on an item
   (`StorageBuffer::Resize` asserts). The shadow pass sizes each item's instance buffer to the
   caster count on the render thread every frame.
4. **Order the clears.** A clear that covers what several items share goes before the fork. A
   clear that covers one item's own subresource goes inside that item. The fork pre-transitions
   the target that is *selected at the fork*; a region whose items select their own targets
   (the cascades) relies on those targets being disjoint instead.
5. **Textures an item samples must already be in their read layout at the fork.** Declare them
   as graph reads (the planner transitions them before the pass) or bind them once before the
   fork; a texture written earlier in the frame and first sampled inside two items is a
   rule-5 conflict, and the record-time claim names both items.
6. **Do not target the backbuffer, query, read back, flush or create resources inside an item.**
   Each is refused; in Debug it asserts. The engine side asserts too: `HeapBinding::FlushOffsets`
   and the profiler's writers check `RenderCommand::IsRecordingParallelItem()`, and a Vulkan
   buffer object written by two items in one region asserts at the second `SetData`.
7. **Keep the item bodies independent of iteration order.** Depth-only rendering is
   order-independent; blended colour is not. State an item sets (viewport, cull mode) is its
   own; state it expects from a previous iteration is a bug, because every item starts from
   the fork's seed.
8. **Verify on both backends.** OpenGL runs the same code inline. A Vulkan frame recorded with
   `OLO_VK_PARALLEL_RECORDING=0` (the lever forces inline) must match one recorded with it on,
   and the validation layer must stay clean.

## 5. What a measurement in a Debug build tells you

Nothing about the win. MSVC's debug iterators (`/MDd`, `_ITERATOR_DEBUG_LEVEL=2`) serialise
every container operation on one global lock, so a forked region's summed worker time comes out
around twelve times the inline time and the wall time does not move. Measure in `Release`; the
numbers in the #806 PR body are the ones to compare against.

## 6. Where the frame clock comes from

The secondary pools reset when `VulkanFrameArena`'s generation advances, because
`VulkanFrameArena::BeginFrame(slot)` is only ever called after the frame fence proved that
slot's submissions retired. That is deliberate: it reuses the one per-slot clock every other
per-frame cache already keys on, so the headless fixtures that already call `BeginFrame(0)` fork
without a second call site. With no frame begun a region records inline.

## 7. What is not built, on purpose

- **`CommandBucket` replay on an item.** The dispatcher's bind cache and the material /
  instance UBOs it uploads are per process; rule 1 applied to `Renderer3D` means per-context
  copies of its resources. Recorded as the next step, not done here.
- **Per-pass worker assignment.** The graph executor stays a sequential walk; passes share
  `Renderer3D` state, and "which passes may run concurrently" is a much larger contract than
  "which cascades of one pass are independent".
- **Async compute** is #808 and orthogonal.

## Appendix: the two findings that shaped this

*The handover's plan (b), "forbid layout transitions inside a parallel pass", was not viable.*
Every draw's lazy scope-open transitions its attachments, every `BindTexture` may
auto-transition, and a deferred clear from one cascade is materialised by the next. A pass body
on Vulkan transitions layouts on every draw; the design had to make that safe, not forbid it.

*The API object was never the hard part.* Its per-recording members moved into a struct in one
scripted pass. The hazards that needed design were the resource objects a pass writes per
iteration and the depth-layer selection that lived on the shared framebuffer object. Rule 6 of
the amendment and the selection's move onto the recording context are the actual fixes.
