# Renderer memory: count physical backing once, keep GPU and CPU apart, say "unknown"

**The rule.** Every GPU allocation the renderer makes is booked once, as physical backing,
in `RendererMemoryTracker`. A view or a second handle onto existing storage is an **alias**:
it carries logical bytes and adds nothing to the physical totals. Memory the owner released
but the GPU may still read is **retiring**, not freed. GPU and CPU bytes are separate
totals, and nothing adds them. A figure the backend cannot observe is reported as unknown,
never as 0. The report is `RendererMemoryTracker::BuildReport()` (issue #1342); the editor
window, `olo_memory_report` and the benchmark `result.json` all read that one report.

## How to book an allocation

| You are writing | Book it with |
|---|---|
| an OpenGL resource class | `OLO_TRACK_GPU_ALLOC(this, bytes, …)` with a size from `RendererMemoryFormat` (mip chain, layers, samples, every attachment). A CPU-side estimate uses `OLO_TRACK_CPU_ALLOC`. |
| a GL destructor, resize or reload whose `glDelete*` goes through `FrameResourceManager::SubmitForDeletion` | `const u64 ticket = OLO_TRACK_RETIRE(this);` then `OLO_TRACK_RELEASE_RETIRED(ticket)` **inside** the deletion lambda. `OLO_TRACK_DEALLOC` there would drop two frames of resident bytes. |
| any Vulkan allocation | the `TrackedVma*` wrappers in `VulkanTrackedAllocation.h` (same signatures as `vma*`). They book the size VMA reports, keyed by `VmaAllocation`. `VulkanDeferredReclaim` retires and releases for you. |
| a view of existing storage | `TrackAlias` (by address), or let `RenderCommand::CreateDepthArrayCompareOffViewHandle` book it through `RendererMemory::TrackResourceView`. The backing must bind its handle: `RendererMemory::BindBackingResourceHandle`. |
| an aggregate over objects that book themselves | **nothing**. `EnvironmentMap` booked a "rough estimate" on top of cubemaps that tracked themselves: a double count on every environment. |

Guards: `RendererMemoryTracker.AllocTrackingIsPairedWithDeallocTracking` (every file that
tracks also untracks or retires-and-releases) and
`RendererMemoryTracker.VulkanAllocationsGoThroughTheTrackedWrappers` (no raw `vmaCreate*` or
`vmaDestroy*` outside the wrapper).

## Attribution: owner scopes and capacity rows

- **Owner scope.** `RendererMemoryOwnerScope scope("Owner", MemoryLifetime::…)` attributes
  every allocation made on this thread while it lives; the innermost scope wins. The render
  graph opens one per executing pass, so lazily created pass resources are attributed with no
  code in the pass. The name must outlive the scope (a literal, or a name the owner holds).
- **Capacity row.** An owner that knows what it holds versus what the last frame used
  registers a `RendererMemoryReporterHandle` (a member declared **last**, so it unregisters
  before what it reads is destroyed). Rows attribute bytes that are already in the totals;
  they are never added to them. An empty optional needs an `UnknownReason`.
- Demand must survive the render-graph declaration cache: stamping "acquired this frame" at
  declaration time reads 0 on every cached frame. The temporal-history row uses the sinks of
  the current topology instead.

## Backend limits on residency (what each figure can and cannot say)

| Figure | OpenGL | Vulkan |
|---|---|---|
| physical bytes per allocation | **format estimate**: driver padding, tiling and compression metadata are invisible | **committed**: `VmaAllocationInfo::size`, alignment included |
| reconciliation (tracked vs allocator) | `notObservable`: GL has no allocator to ask | `vmaGetHeapBudgets` statistics; `reconciled`, `untracked` or `overCounted` to the byte |
| residency / budget per heap | `unknown`: no residency query. `NVX_gpu_memory_info` and `ATI_meminfo` measure different things per vendor (the #810 decision), so none is used | `osReported` with `VK_EXT_memory_budget` (enabled whenever the device lists it); otherwise `allocatorHeuristic`: VMA's own blocks against a fixed fraction of the heap |
| in-flight retirement | the engine's 2-frame `SubmitForDeletion` delay; the driver's own deferral after `glDelete*` is invisible | `VulkanDeferredReclaim` entries until their generation completes |
| outside every figure | driver-internal allocations | allocations made without VMA: the swapchain images, and the ImGui Vulkan backend's vertex/index buffers and textures (raw `vkAllocateMemory` in the vendored backend). They appear in the OS heap usage, not in reconciliation |

An `untracked` difference on Vulkan means some VMA allocation bypassed the wrapper; an
`overCounted` one means something was booked twice. `racing` means an allocation was in
flight while the two sides were read; read again.

## Failures that shaped the rules

- **Vulkan read 6 MiB.** No Vulkan code called the tracker, so the integrated benchmark
  (#1338) recorded a CPU shader estimate as the GPU budget. Tracking now happens at the VMA
  seam, where no Vulkan allocation can avoid it.
- **`GpuMemoryTotalBytes` was CPU plus GPU.** The benchmark field and the overlay's "GPU Mem"
  both read the tracker's all-type total. Shader "GPU bytes" were SPIR-V size plus 1 KiB,
  which is not device memory; they are booked as CPU now.
- **Estimates ignored most of the storage.** GL framebuffers counted 4 bytes per attachment
  at one sample; GL textures counted the base level only; the transient pool sized an MRT by
  its first attachment and dropped framebuffers from its total entirely; the planner
  multiplied by the mip *count*. One table, `RendererMemoryFormat`, serves them all.
- **A leaked-looking total was a dropped deallocation.** `TrackDeallocation` used `TryLock`
  and discarded the call whenever another thread held the mutex. It locks now.
- **A saving that never shrank.** `CompactionSavedBytes` was a running sum; a retired or
  rebuilt BLAS kept its saving forever. It is derived from resident entries now.

## Negative control

`OLO_FAULT_COUNT_ALIAS_AS_BACKING=1` (`Levers::FaultCountAliasAsBacking`) books every alias
as backing. `RendererMemoryReport.NegativeControl_TheSnapshotCheckCatchesAnAliasCountedAsBacking`
proves the snapshot check fails under it; on Vulkan the same fault turns the reconciliation to
`overCounted`. If a change makes either stay green under the fault, the check is vacuous.
