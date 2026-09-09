# The Vulkan async compute queue: ownership transfers, and why the degrade path is the tested one

**Rule:** a resource that crosses between the graphics queue and the async compute queue needs a
**matched pair** of queue-family ownership barriers — a release recorded into the source family's
command buffer and an acquire into the destination's, submitted in that order with a semaphore
between them, both halves naming the same two families, the same layouts and the same subresource
range. A semaphore alone orders the two queues and leaves the resource's contents **undefined**.

Issue #808, ADR 0010's queue-topology row and ADR 0011 §6.

## 1. What the backend does now

`VulkanDevice` selects a second, optional queue: a family that can dispatch compute but cannot
draw. The pick is a pure function — `VulkanQueueSelection::SelectAsyncComputeFamily` — so every
answer it can give, including every reason it says no, is pinned in plain CI by
`VulkanAsyncComputeTest` against fabricated family tables.

The render graph already knew which passes were candidates: `SetAsyncComputeCandidate(true)` on a
`Compute`-work-type node, grouped into `AsyncComputeBatch`es and bracketed in the submission plan
with `BatchBegin` / `BatchEnd`. #808 only made the backend act on that bracket.

Inside the bracket:

1. **The graphics command buffer is parked, not submitted.** It stays open so the release half of
   every ownership transfer can be appended to it. A batch that submitted the graphics segment
   first would have nowhere to record the releases.
2. Recording switches to a command buffer allocated from the **compute family's** pool.
3. Every `IssueBarrierBatch` inside the bracket becomes an ownership **pair** the first time it
   touches a resource: release into the parked graphics buffer, acquire into the compute buffer.
   Later barriers for the same resource are ordinary same-queue barriers.
4. At `BatchEnd` the transfers are **mirrored**: everything the batch barriered is released back to
   the graphics family on the compute buffer, and acquired on the freshly opened graphics one.

That mirror is what makes the whole thing tractable: **ownership is always with the graphics family
outside a batch**, so no ownership state has to survive a batch, a frame, or a swapchain
recreation. There is no persistent owner map to get wrong.

Submission order at `BatchEnd` is: graphics segment (signals timeline value *N*) → compute segment
(waits *N*, signals *N+1*) → the rest of the frame's graphics work (waits *N+1*).

## 2. The three traps, in the order they bite

### 2a. A stage mask is only legal on a queue that supports the stage

`VUID-vkCmdPipelineBarrier2-srcStageMask-03849`: a barrier recorded into a command buffer from a
compute-only family may not name `COLOR_ATTACHMENT_OUTPUT`, `FRAGMENT_SHADER`,
`EARLY`/`LATE_FRAGMENT_TESTS`, `ALL_GRAPHICS` or any vertex-pipeline stage. The engine's barrier
lowering emits those routinely, because the *producer* of a compute pass's input is usually a raster
pass.

Two things handle it, and both are needed:

- **The transfer pair makes the common case legal by construction.** The release carries the
  source scope and *no* destination scope; the acquire carries the destination scope and *no*
  source scope. So the graphics stages only ever appear on the half recorded into the graphics
  buffer.
- **`VulkanRecordingContext::RecordBarrier` clamps everything else.** It is the one place every
  barrier reaches a command buffer, and when the context is recording onto a compute-only queue it
  replaces any scope naming an unsupported stage with `ALL_COMMANDS` + `MEMORY_READ|MEMORY_WRITE`.
  Over-synchronising a handful of barriers at a queue boundary costs nothing; a validation error
  mid-frame costs the session.

### 2b. `timestampValidBits` is per family

The engine stamps every render-graph pass through `GPUPassTimerPool`, and
`vkCmdWriteTimestamp2` on a family reporting zero valid bits is invalid usage
(`VUID-vkCmdWriteTimestamp2-timestampValidBits-03863`). The graphics family reporting 64 says
nothing about the compute family. The selection therefore **refuses** an async family that cannot
carry timestamps, with its own reason string, rather than growing a second untested "no timers
here" recording path.

This is the same shape as the timestamp-period finding in #801: a per-queue property read once from
the wrong queue.

### 2c. NVIDIA forgives a missing ownership transfer

The failure mode of a missing (or mismatched) transfer pair is *undefined resource contents on
hardware that really splits the families*. On the development box — an RTX 4090 — everything looks
correct. There is no pixel to inspect and no error to read. That is why the pair's shape is pinned
by construction in `VulkanAsyncComputeTest` rather than by looking at a frame:

- both halves name the same `(srcQueueFamilyIndex, dstQueueFamilyIndex)`;
- the release has `dstStageMask`/`dstAccessMask` of `NONE`;
- the acquire has `srcStageMask`/`srcAccessMask` of `NONE`;
- `oldLayout`, `newLayout`, the image and the subresource range are identical on both.

## 3. The degrade path is the tested path

Not every device exposes a compute-only family, and CI hardware does not. When the family is
absent — or `OLO_VK_ASYNC_COMPUTE=0`, or the batch cannot be split for a local reason (an open
occlusion query, a parallel-recording region in flight) — the batch runs inline on the graphics
queue exactly as it did before, and the decline is **counted and named**:
`RendererAPI::AsyncComputeFrameStats::BatchesDeclined` plus a warn-once carrying the reason
(`VulkanQueueSelection::Describe`). It is never a silent no-op
([no-silent-fallbacks.md](no-silent-fallbacks.md)).

Read the counters through `olo_perf_pass_timings`; a session that expects overlap and sees
`BatchesOnComputeQueue == 0` has its answer in `DeclineReason` without a debugger.

## 4. What this does not do

- **It does not reorder passes.** The compute hoist in `RenderGraph::HoistComputePasses` decides
  what may run early and it respects every dependency edge, including the resource-derived ones.
  Async compute overlaps what the schedule already allows to overlap; it does not create the
  opportunity.
- **It does not cover a resource the graph does not declare.** The transfer set is exactly the set
  of resources a barrier inside the batch touches, which is the set the pass declared through
  `RGBuilder`. A compute pass that reads a resource it never declared had no barrier before this
  change either — but the consequence is worse now, so an async-compute candidate's declarations are
  worth auditing before it is marked one.
- **It does not run on OpenGL.** GL 4.6 has one command stream; `BeginAsyncBatch` there stays a
  debug-group label.

## 5. Evidence

See the PR for #808. The layered checks are:

| Layer | What it proves | Runs in CI |
|---|---|---|
| `VulkanAsyncComputeTest` (selection + pair shape) | the policy, and that both halves of a transfer agree | yes |
| `VulkanAsyncComputeTest` (device-gated round trip) | a real graphics → compute → graphics hand-off through `VulkanContext`, validation-clean | no (needs a GPU) |
| Live editor on `--rhi=vulkan` with validation layers | no queue-family or sync errors across a real frame graph | no |
