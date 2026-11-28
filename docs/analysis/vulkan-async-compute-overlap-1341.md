# Vulkan async compute: measured serialization, and why crossing is now opt-in (#1341)

Keep render-graph async batches on the graphics queue unless `OLO_VK_ASYNC_COMPUTE=1`. Measured
on an RTX 4090, crossing to the compute queue never overlapped graphics work and added 2.3 ms of
GPU frame time: the submission makes the graphics queue wait for each whole batch, and the
current schedule leaves no independent graphics work beside any batch anyway. This is a measured
no-go for overlap, not a performance gain from the queue.

## Instrument

GPU pass brackets now record the queue they ran on and their async batch, and resolve begin/end
offsets from the frame-begin timestamp. `olo_perf_pass_timings` publishes them per pass
(`queue`, `asyncBatch`, `beginMs`, `endMs`) and a `queueOverlap` block: union busy time per queue,
the time both queues had a bracket open (`overlapMs`, an upper bound on concurrency) and each
batch's interval and summed pass time. The frame-begin stamp is on the graphics queue, so a
compute-queue offset compares timestamps across queues. Vulkan guarantees one time domain for all
of a device's queues only when it supports `VK_KHR_calibrated_timestamps` or
`VK_EXT_calibrated_timestamps` (`VkTimeDomainKHR`), and orders timestamp writes across
submissions only when one is enabled (`vkCmdWriteTimestamp2`). The device enables whichever it
has, and `queueOverlap.timeDomain` reports `oneDeviceTimeDomain`. Without one it reports
`unverified`, withholds every compute-queue offset, and publishes `overlapMs` and
`asyncComputeBusyMs` as null with `overlapUnavailableReason`, so a missing guarantee never
reads as zero overlap. The RTX 4090 exposes the extension. The no-go does not rest on the
offsets alone either: the frame GPU time rises by the batches' idle time, and the submission
code orders the queues by construction.

## Method

RTX 4090, Vulkan, clang-cl Release editor, 1920 x 1080, Deferred, `Courtyard.olo` at its
benchmark camera with GTAO, volumetric fog and virtual shadow maps on (the three async
candidates: `GTAOPass`, `VolumetricFogPass`, `VirtualShadowMapMarkPass`). The lever is read at
device creation, so each arm is a fresh process; six processes alternate on/off/off/on/on/off,
each warming 240 frames and sampling 120 distinct resolved frames
(`scripts/perf/async-compute-timeline-live.py`). Sibling worktrees were compiling on the same
host throughout; the alternation, not a quiet box, is what makes the arms comparable.

## Result

| process | arm | frame GPU ms (median) | compute-queue busy ms | overlap ms | batches crossed / declined | ownership pairs |
|---:|---|---:|---:|---:|---|---:|
| 1 | on | 23.86 | 1.10 | 0.000 | 3 / 0 | 34 |
| 2 | off | 22.20 | 0 | 0.000 | 0 / 3 | 0 |
| 3 | off | 22.83 | 0 | 0.000 | 0 / 3 | 0 |
| 4 | on | 25.50 | 1.18 | 0.000 | 3 / 0 | 34 |
| 5 | on | 25.13 | 1.16 | 0.000 | 3 / 0 | 34 |
| 6 | off | 22.95 | 0 | 0.000 | 0 / 3 | 0 |

Median on 25.13 ms, off 22.83 ms: **crossing costs 2.3 ms a frame**. `overlapMs` is zero in every
sampled frame of every on-process. The batch work costs the same on either queue (GTAO 0.474 vs
0.470 ms, volumetric fog 0.143 vs 0.135, VSM mark 0.48-0.56 vs 0.51-0.53). The difference is idle
graphics time: per batch the graphics queue waits a median **0.32 ms before** the compute work
starts and **0.63 ms after** it ends (0.012 / 0.010 ms between the same passes with the lever
off), about 2.8 ms over three batches.

## The serialization, and the opportunity

The submission (`VulkanContext::EndAsyncComputeSegment`) is graphics segment -> compute segment
(waits for it) -> graphics continuation (waits for the compute segment at `ALL_COMMANDS`, as its
first submission). The graphics segment is also submitted only at the batch's end, after the
batch is recorded. So no graphics work can run while a batch does, whatever the schedule.

The schedule offers nothing to overlap either. With the lever off, the execution order around
the batches is:

```
DeferredOpaqueDecalPass -> GTAOPass -> VirtualShadowMapMarkPass -> DeferredLightingPass
SSRPass -> BloomPass -> VolumetricFogPass -> FogPass
```

`DeferredLightingPass` consumes GTAO's output and `FogPass` consumes the volumetric fog volume,
each immediately after its producer; GTAO and the VSM mark are two batches back to back.
Overlap would need both (a) the continuation's wait moved to each batch's first consumer, with
the acquire halves recorded there and the graphics passes in between proven free of the batch's
resources and their physical aliases, and (b) a schedule that places independent graphics work
(for example shadow rendering) between a batch and its consumer. Neither exists; (b) is a
render-graph scheduling change, not a queue change.

## What changed

- Crossing is opt-in: with `OLO_VK_ASYNC_COMPUTE` unset, a device with a compute-only family
  still creates the queue, and every batch declines with the reason "not opted in: the graphics
  queue would wait for the whole batch, so crossing adds idle time without overlap", counted in
  `asyncCompute.batchesDeclined` and logged once. `1` crosses as before; `0` does not create the
  queue (the degrade path on hardware that has one).
- Ownership transfers now cover every image a batch touches, including images bound through a
  descriptor that the planner issued no barrier for (`fix(vulkan): move image ownership when an
  async-compute batch binds it`), and slots bound before the batch that a dispatch inside it
  still reads. Before it, the on-arm frames above reported 0 ownership pairs.

Verified with the change, six more fresh processes in the order default/on/off/default/off/on:

| process | arm | frame GPU ms | batches crossed / declined | decline reason |
|---:|---|---:|---|---|
| 1 | default (unset) | 21.34 | 0 / 3 | not opted in: ... |
| 2 | on (`=1`) | 23.44 | 3 / 0 | — |
| 3 | off (`=0`) | 22.70 | 0 / 3 | disabled by OLO_VK_ASYNC_COMPUTE=0 |
| 4 | default (unset) | 21.91 | 0 / 3 | not opted in: ... |
| 5 | off (`=0`) | 33.16 | 0 / 3 | contended: a sibling's GPU work ran beside it, not timing evidence |
| 6 | on (`=1`) | 47.57 | 3 / 0 | contended, as above |

Same work, same pixels: on the deterministic dense scene (`ParallelRecording.olo`, GTAO, fog and
VSM on) one process each with `=1` and `=0` captured scene colour and depth on Deferred, Forward
and Forward+. All six pairs are byte-identical at 1920 x 1080 and non-empty. Its frame GPU time:
6.86 ms crossing, 2.56 ms on the graphics queue, overlap 0.

## Schedule, aliasing and early submission

Aliasing is planned on the linear execution order, so crossing is safe only if the batch really
executes in that order's slot. The timeline shows it does: in 1,080 compute-queue intervals no
graphics bracket intersects a compute one, so every batch runs strictly between the graphics
work before it and after it, exactly where the linear plan put it. A schedule that overlaps would
have to extend the batch's resource lifetimes (and their physical aliases) to its first consumer
before recording; nothing overlaps yet, so nothing needs to.

Release halves cannot be stranded by an early submission: while a batch is open
`SubmitRenderGraphFenceSegment` refuses (it would end the compute buffer, submit it to the
graphics queue and leave the parked buffer holding the releases unsubmitted), and the batch end
records the mirrored releases before ending and submitting both buffers in order. The device test
`AFenceSegmentSubmitInsideAnOpenBatchIsRefused` pins the refusal and the clean close.

## Fallbacks with reasons

Each keeps the batch on the graphics queue and names why: no compute-only family, lever `0`
(both via `VulkanQueueSelection::Describe`), lever unset (above), outside the frame callback,
no compute command buffer, a parallel-recording region or an open occlusion query that prevents
suspending the graphics recording, and a nested batch. A family without timestamp bits is never
selected. The device tests `AnAsyncBatchCrossesToTheComputeQueueAndBack`,
`ABindInsideTheBatchTransfersAnImageThePlannerDidNotBarrier`,
`AnUnsetLeverKeepsTheBatchOnTheGraphicsQueueAndSaysWhy` and
`AFenceSegmentSubmitInsideAnOpenBatchIsRefused` cover the crossing, the transfer pairs, the
default decline and the strand guard on real hardware; lavapipe has no compute-only family and runs the
degrade path.

## Reproduce

Launch the Release editor with `-Rhi vulkan -AllowWrites`, once with `OLO_VK_ASYNC_COMPUTE=1` and
once with `=0`, alternating, and run
`python scripts/perf/async-compute-timeline-live.py --port <p> --arm <on|off> --output <json>`
against each.
