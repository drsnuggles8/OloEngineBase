# Vulkan parallel command recording: the threading model, and how a pass opts in

**Rule:** a pass that wants its independent work items recorded on task workers calls
`RenderCommand::RecordParallel(count, body)` and gives every item its own resource objects.
Command-buffer state lives in a *recording context*, selected through a thread-local.
Process-wide backend state is frozen or protected by a mutex. The contract is ADR 0011
amendments (92) and (94).

Issues #806 and #1013. See the [pass audit](vulkan-parallel-pass-audit.md) for
implementation coverage and the evidence still required for each family.

## 1. What is per thread, what is per process

| State | Where it lives now | Who may touch it inside a region |
|---|---|---|
| Command buffer, rendering scope, pending clear, bind caches, recorded pipeline state, viewport, scissor, framebuffer selections (draw buffers **and** the depth-array layer), draw tallies | `VulkanRecordingContext` — one per item, one for the render thread (`VulkanRendererAPI::m_Main`) | the item that owns it |
| Bind-point mirror (`VulkanBindingState`), current program (`VulkanShader` / `VulkanComputeShader`) | `VulkanWorkerRecordingContext` for items; the process-wide accessors are the caller's only copy | same |
| Dispatcher caches, camera override, per-draw material/camera/bone/terrain/decal/foliage/water uploads and model instances | `CommandDispatchRecordingState` within the item's `FrontendRecordingContext` | item; upload objects and capacity prepared before fork |
| Heap offsets, epoch and offset-table UBO | item-owned `HeapBinding::RecordingState` | item |
| Dispatcher statistics, profiler counters and instanced-draw records | item tally, published in item order after join | item until join, then caller |
| Dispatcher frame flags, committed frame data, GPU Scene records and shadow handles | shared and frozen during the region | reads only |
| Image layouts (`VulkanImageLayoutTracker`) | an overlay per item over the render thread's tracker, merged at the join in item order | same; the base is frozen |
| Frame arena cursor | one atomic per slot; an item claims a 64 KiB block once and bumps inside it | any thread |
| Descriptor slot cache, sampler heap, pipeline builder, descriptor-heap null-slot memo, framebuffer depth-layer view cache, texture attachment view | key hashed outside the lock; a shared lock on the hit path, exclusive on a miss | any thread |
| Image info / root object / raw buffer registries, device limits, capabilities | read-only inside a region | any thread, reads only |
| Backbuffer, queries, conditional rendering, readbacks, mid-frame flush, one-shots, resource creation | render thread only | refused on an item: Debug asserts, otherwise warn-once |

`Ctx()` reads the `CurrentVulkanWorkerContext()` installed by `ScopedVulkanWorkerContext`.
The caller also runs items through `ParallelFor`, so test whether a worker context is set,
not whether the current thread is the render thread.

## 2. The fork, the join, and why the order is what it is

1. **Fork.** The primary's scope is closed and its pending clear materialised, so a clear the
   pass requested *before* the fork lands before any item's work. The bound target's
   attachments are transitioned to their attachment layouts on the primary. Each item context is
   seeded with the render thread's recorded state and its tracker is made an overlay.
   Seeded sampled bindings are transitioned except images also selected for attachment or
   storage writes. Shared bound UBO addresses and null sampled slots are prepared on the caller;
   item frontend upload objects are created/reused with empty dispatcher caches.
2. **Record.** Every item's secondary command buffer is acquired and begun on the render thread
   first, from that item's own (frame slot, item) pool — a pool that cannot deliver declines the
   region into the one inline path. Then `ParallelFor` over the items: the body runs with the
   worker context set; its scope is closed and its clear materialised the way `EndRecording`
   does for a frame.
3. **Join.** Overlays merge into the render thread's tracker in **item order**; the secondaries
   are executed with one `vkCmdExecuteCommands` in **item order**; tallies are summed; the
   primary's command-buffer caches are reset (state is undefined after execute); the render
   thread adopts the last item's framebuffer selections, which is GL's sticky attachment state
   after a loop. Frontend statistics publish in item order and caller dispatcher caches are
   invalidated. A nested region executes inline inside its parent item and contributes to that
   parent's timing without touching the shared item pool or frame statistics.

GPU item order matches the inline loop. Verify that OpenGL inline and Vulkan off/on agree.

## 3. The layout rule, and the pre-transition that makes shared targets legal

Barriers read `oldLayout` from the frozen base. **Several items may write one subresource
only when every write preserves its layout.** Otherwise a later barrier names stale state.
`VulkanLayoutClaimTable` rejects incompatible claims with both item indices; ordered overlay
merge also reports conflicts through `ParallelRecordingFrameStats::MergeConflicts` and MCP.
Every conflict is a pass bug.

Cascades own distinct layers. Atlas entries share layer zero, so transition that attachment
and materialise its clear **before** forking. Every item then opens in the existing depth
attachment layout. Clearing inside an atlas item would erase other entries' tiles.

## 4. Converting a pass: the checklist

1. **Find every written object.** UBO/SSBO `SetData` and `InstanceBuffer::Upload` version bytes
   per object (amendments (78), (80)). Give each item private upload objects created before
   the region; shadow views own camera, animation and instance buffers.
2. **Audit transitive shared state.** Search for `static`, `s_`, `GetInstance` and `::Get()`.
   Include lazy per-object builders: `VulkanShader::GetRootDataLayout` needed guarded first
   construction because every item can ask for it. Dispatcher replay, heap-offset flush,
   profiler counters and instanced-draw records
   use the scoped frontend context. This does not make arbitrary profiler methods, subsystem
   setters, lazy builders or upload objects safe. Shadow foliage receives time as a frozen
   argument and uses the item's foliage UBO; virtual-geometry shadows receive prepared
   per-view command/argument/visible buffers and parameter UBOs.
3. **Anything an item writes into its private upload object dies at the join.** The item's
   copy is seeded from the shared object at the fork; nothing copies it back. The inline path
   leaves the shared object holding the LAST iteration's bytes, the forked path leaves it
   holding the pre-fork bytes, so a pass that reads a UBO's *contents* after a region instead
   of re-uploading them reads different data on OpenGL and on Vulkan. Re-upload after the
   region, or keep the write out of the item. This is why `Publish()` clears the dispatcher's
   bind caches: it forces the next draw to rebind and re-upload rather than trust a cache that
   describes an item's buffer.
4. **Size buffers before the fork.** `InstanceBuffer::Upload` can grow storage, but resource
   creation and reclamation are refused on items. Prepare enough capacity for the largest
   batch that item can upload.
5. **Order the clears.** A clear that covers what several items share goes before the fork. A
   clear that covers one item's own subresource goes inside that item. The fork pre-transitions
   the target that is *selected at the fork*; a region whose items select their own targets
   (the cascades) relies on those targets being disjoint instead.
6. **Textures an item samples must already be in their read layout at the fork.** Declare them
   as graph reads (the planner transitions them before the pass) or bind them once before the
   fork. The fork pretransitions all seeded sampled-image slots except images also bound for
   writes; it cannot discover textures that exist only in packets until replay. A texture
   written earlier in the frame and first sampled inside two items is a rule-5 conflict, and
   the record-time claim names both items. Join between a shared write and a later sampling
   region; a stale sampler must never move the current output into a read layout.
7. **Do not target the backbuffer, query, read back, flush or create resources inside an item.**
   Each is refused; in Debug it asserts. Frame-scoped dispatcher setters also reject item
   calls, and a Vulkan buffer object written by two items in one region asserts at the
   second `SetData`. GPU timestamp scopes belong outside worker bodies.
8. **Keep the item bodies independent of iteration order.** Depth-only rendering is
   order-independent. Blended draws may use contiguous ranges of the already sorted packet
   stream because secondary execution preserves that stream's GPU order. CPU state an item
   expects from a previous iteration is still a bug: every item starts from the fork's seed.
   Explicitly establish item-local shader/mode caches at the start of every body, including
   the inline path where backend state carries over from the preceding item.
9. **Verify on both backends.** OpenGL runs the same code inline. A Vulkan frame recorded with
   `OLO_VK_PARALLEL_RECORDING=0` (the lever forces inline) must match one recorded with it on,
   and the validation layer must stay clean.

## 5. What a measurement in a Debug build tells you

Debug validates correctness, not speedup. MSVC debug iterators serialise container operations
on a global lock. Measure in Release; the #806 PR records the earlier baseline.
The [#1013 measurements and evidence](../analysis/vulkan-parallel-recording-1013.md)
record the dense-scene benefit, small-scene overhead, control drift and fork costs.

## 6. Where the frame clock comes from

Secondary pools reset with `VulkanFrameArena`'s generation. `BeginFrame(slot)` occurs only
after that slot's fence retires its submissions. Headless fixtures use the same clock;
without a begun frame, regions execute inline.

## 7. Bucket replay and region diagnostics

`CommandBucket::ExecuteParallel` and `RecordPackets` partition immutable, sorted packets into
contiguous ranges, at least 32 packets per item and at most `MAX_RENDER_WORKERS` items.
The dispatcher classifier rejects query-bearing meshes, observed decals and unsupported
packet kinds; any rejected packet keeps the whole span inline. Instance capacity is computed
before the fork. Each item owns replay statistics and a scoped view override. The caller
merges statistics after the join. `ExecuteWithGPUTiming` uses ordinary replay inside an item
because GPU query pools remain caller-owned.

`olo_perf_pass_timings` and the renderer profiler expose named regions, the inline/parallel
decision, each item's recording duration, summed worker time, total region wall time and
join wait. The region name comes from the enclosing backend debug group. Join wait measures
the interval from the caller finishing its last item until `ParallelFor` returns, including
remaining scheduler bookkeeping; it is not a GPU wait. Inline regions have item and wall
timings too. Compare Release runs interleaved off/on with an untouched control pass, and
report the control's drift alongside the candidate's change.

Set `OLO_VK_RECORDING_COSTS=1` before launching a diagnostic process to add
`selectionSeedMs`, `attachmentPrepareMs`, `sampledImagePrepareMs` and
`pipelineLookupMs` to each region. These time the framebuffer-selection copies,
bound attachment preparation, seeded sampled-image preparation and graphics PSO
lookup respectively. Pipeline lookup is summed across items, so it is worker CPU
time rather than elapsed region time. The probe adds clock reads per draw; leave
it unset for the final off/on frame-time comparison. Normal runs report zero for
these optional fields.

Whole-pass regions additionally report `itemPassNames`, aligned with `itemRecordMs`; the
region label is `RenderGraph`. Each GPU pass bracket encloses the corresponding secondary's
execution on the primary, so GPU durations retain the original pass boundaries.

## 8. Whole-pass recording

See [whole-pass parallel recording](vulkan-parallel-graph-recording.md) for the
prepared-pass contract, topological grouping, resource lifetimes, fence boundaries
and ordered publication.
