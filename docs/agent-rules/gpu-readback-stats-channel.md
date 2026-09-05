# GPU readback stats — the channel, and the four ways it lies

Issue #721. How a GPU-driven pass publishes counters to the CPU, why the contract is a set of
names rather than a set of offsets, and the failure modes that produce **a plausible number**
instead of an error.

Read this before adding a counter, before instrumenting a new pass, and before adding anything
that needs a buffer binding.

The channel: [`Renderer/Debug/GPUReadbackStats.h`](../../OloEngine/src/OloEngine/Renderer/Debug/GPUReadbackStats.h)
· registry: [`GPUReadbackStatsRegistry.h`](../../OloEngine/src/OloEngine/Renderer/Debug/GPUReadbackStatsRegistry.h)
· GLSL twin: [`include/GPUReadbackStats.glsl`](../../OloEditor/assets/shaders/include/GPUReadbackStats.glsl)

---

## 0. The governing property: this instrument has no wrong-looking failure

Every other renderer subsystem fails visibly. A broken cull drops geometry, a broken shadow pass
renders black, a broken barrier flickers. A broken *stats channel* returns `1,847`.

That is the whole reason this document exists. Nothing downstream can tell a right number from a
wrong one, so the correctness has to be argued at the point the number is produced — which is what
sections 2–5 are. If you take one thing: **never quote a counter without the frame index it belongs
to**, because a counter that stopped updating and a counter that is genuinely constant are the same
bytes.

## 1. Using it

Any compute or fragment stage:

```glsl
#include "../include/GPUReadbackStats.glsl"

oloStatAdd(OLO_STAT_InstanceCullDrawn, 1u);              // a counter
oloStatFlag(OLO_STATFLAG_VSMPhysicalPool);               // a condition
oloStatOverflow(OLO_STATFLAG_InstanceCullOutput,         // both, for a refused append
                OLO_STAT_InstanceCullDropped, 1u);
```

C++ side:

```cpp
if (const auto& frame = GPUReadbackStats::GetLatest(); frame.Valid)
{
    frame.Get(GPUStatCounter::InstanceCullDrawn);
    frame.Overflowed(GPUStatFlag::InstanceCullOutput);
}
```

Adding a counter is two lines that must agree: one `X(...)` in `OLO_GPU_STAT_COUNTERS` and one
`const uint OLO_STAT_<Name> = <n>u;` at the same index in the `.glsl`.
`GPUReadbackStatsLayoutTest` fails loudly if they don't.

Consumers today: the F3 debug overlay (overflows always visible, counters behind a fold) and the
`olo_gpu_readback_stats` MCP tool.

## 2. The contract is NAMES, not offsets — and that was the design decision

The reference implementation (Timberdoodle's `ReadbackValues`) is a struct mirrored between C++ and
GLSL. Mirroring a struct makes the contract a set of **byte offsets**, and this repo's history is
full of what that costs: insert one `u32` on one side and every field after it reads its
neighbour's value. No compile error, no crash, no wrong pixel — just each counter reporting the one
next to it.

So the channel is a flat `uint[32]` plus a shared name → index registry. Drift then means *a name
exists on one side and not the other*, which is detectable by reading the two files as text. That is
exactly what `GPUReadbackStatsLayoutTest` does.

**And the test is built to be incapable of passing vacuously**, because that is a real thing that
happened here: #847 had to fix `CrossShaderUBOMemberOffsetsAgree`, which compared `("", 0)` against
`("", 0)` for every member because both parsers silently matched nothing. It was green and
structurally could not fail. Every parse in the layout test therefore asserts non-empty and asserts
against an independently known count *before* comparing anything. **If you change how the GLSL
declares its constants, the test will fail rather than stop checking — keep it that way.**

## 3. What makes it non-stalling is the FENCE, not the buffer

The issue text says "persistent-mapped staging buffers". The engine cannot do that for a readback:
`AllocatePersistentUploadStorage` maps `GL_MAP_WRITE_BIT` only — write-only on both backends now
that Vulkan lowers it too (#1052, a `HOST_VISIBLE|HOST_COHERENT` sequential-write placement), so it
is still the wrong side of the transfer. The ring is `MemoryResidency::DeviceToHost` buffers read
with `ReadBufferSubData`, and
the thing that keeps it off the critical path is:

> **`IsFenceSignaled` is a poll. There is no `ClientWaitFence` anywhere in `GPUReadbackStats`, and
> adding one reintroduces the stall the whole design exists to remove.**

Consequences you must not undo:

- **A full ring skips the capture.** Every slot still executing means the correct response is to
  keep the newest retired frame, not to block on the oldest. `GetSlotsInFlight()` is how a consumer
  learns this happened; the overlay and the MCP tool both surface it as "ring saturated".
- **Retire OLDEST FIRST, not in array order.** `NextSlot` is the slot the next capture will use, so
  it is also the oldest in flight; walking from there wraps in *issue* order. Array order lets a
  wrapped ring publish an older frame after a newer one already retired — counters that go
  backwards in time, which is worse than stale counters because it looks like a real change.
- **Never read the live SSBO directly.** It is GPU-written every frame and must stay in video
  memory. A CPU read off it makes NVIDIA log 131188 and then migrate the buffer VIDEO → HOST
  (131186), permanently slowing every atomic that touches it. Same trap, same fix, as
  `ShaderDebugDraw::StageStatsForReadback` and `VirtualMeshRegistry::ReadFrameCullStats`.
  **`TerrainGPUQuadtree::PollOverflow` was the one that got away** — it read 4 bytes of
  overflow flags straight off the cull-state SSBO, which is *also* the buffer
  `DispatchComputeIndirect` reads twice a frame. Measured on Drift: **708** × 131188 and
  **25** × 131186 in one session, across six buffers (one per island). Throttling the read
  to once every 240 frames, which is what the code did instead, does not help: the
  migration is permanent, so one read poisons every subsequent frame. Staging it fixed it
  (0 × 131188, verified with the copy and the staged read traced to prove the path still
  ran — a silenced warning and a disabled poll look identical, see §0).
- **The poll needs a flush to make progress.** In the engine, SwapBuffers flushes every frame, so
  this is free. A *test* presents nothing, so it must flush itself — see
  `GPUReadbackStatsEvidenceTest::FlushAndWaitForGPU`. Without it the fences never submit and the
  poll spins forever.

## 4. `BeginFrame` and `EndFrame` bracket the graph, and both ends matter

`BeginFrame` runs in `RenderPipeline::UploadExecutionState` (before `RGraph->Execute()`) and
`EndFrame` in `Renderer3D::EndScene` (after `Execute()`, the HZB rebuild and the capture commit).

- Move `BeginFrame` after `Execute()` and it clears the counters the frame just wrote. Reads as
  "the channel always reports zero", which looks exactly like a binding bug.
- Issue the `EndFrame` copy earlier and it silently omits whatever ran after it. The number stays
  plausible, just smaller.
- `BeginFrame` uploads the WHOLE block, not just the header. That upload is what zeroes the
  counters; a per-frame counter that is never cleared accumulates across the session and reads as a
  monotonically rising number nobody notices is wrong.

## 5. Bound-checking an atomic append: reserve with one counter, commit with another

**Never give a reserved slot back.** This is the section to read before you bound-check any
`atomicAdd` append anywhere in this engine.

Instrumenting the instance cull turned two unbounded appends into bound-checked ones. The obvious
version is wrong:

```glsl
// WRONG — do not write this.
uint outIdx = atomicAdd(indirect.instanceCount, 1u);
if (outIdx >= capacity)
{
    atomicAdd(indirect.instanceCount, 0xFFFFFFFFu);   // "roll it back"
    return;
}
outputInstances[outIdx] = ...;
```

It looks right and it is right single-threaded. Under concurrency it corrupts. With the buffer at
`capacity - 1`:

1. writer **A** takes slot `capacity-1` and writes it; the counter is now `capacity`;
2. overflower **C** takes `capacity`, refuses, decrements the counter back to `capacity-1`;
3. writer **D** is handed `capacity-1` **again** and overwrites A's instance.

The final count looks correct, yet one instance is duplicated and another silently lost. You would
have converted a clean truncation into corruption — the exact opposite of the point.

The correct shape splits reservation from commit:

```glsl
uint outIdx = atomicAdd(indirect.reserveCursor, 1u);   // monotonic; never given back
if (outIdx >= u_OutputCapacity)
{
    oloStatOverflow(OLO_STATFLAG_InstanceCullOutput, OLO_STAT_InstanceCullDropped, 1u);
    return;
}
outputInstances[outIdx] = inputInstances[idx];
atomicAdd(indirect.instanceCount, 1u);                 // exact count of what was WRITTEN
```

`reserveCursor` is monotonic so every invocation owns a distinct index; `instanceCount` counts only
landed writes, so the indirect draw never reads a slot the cull did not fill. Both extra words came
out of existing padding (`_indirectPad0`, and a second word on the reject counter) — and **both must
be zeroed every dispatch**: a cursor carried over from last frame starts past the capacity and
truncates the entire batch while the flag faithfully reports a truncation that was really a stale
counter.

**When the count is already clamped downstream, do neither.** `VSM_MarkRequiredPages` overshoots
`b_RequestCount` on purpose: `VSM_AllocatePages` reads it as
`min(b_RequestCount, VSM_MAX_REQUESTS)`, so the overshoot is harmless and a decrement would
reintroduce exactly the slot-reuse bug above. The honest number of refusals is the dropped counter,
not the cursor.

## 6. Forcing an overflow must force a REAL one

Acceptance criterion #2 of #721 is that an *intentionally overflowed* buffer surfaces as a flag. A
"pretend the flag fired" switch satisfies the letter of that and proves nothing — it tests the
plumbing between a bool and the overlay, not the thing that actually goes wrong.

`GPUFrustumCuller::SetDebugOutputCapacity(n)` shrinks the bound the shader checks against, so the
buffer genuinely truncates: the draw really renders fewer instances, the dropped counter really
counts them. Any future forced-overflow knob should work the same way.

## 7. Instrumenting VSM: read what the pass does with the refused item

`VSM_MarkRequiredPages.comp` appended page-allocation requests to a ring and, when full, did
**nothing**:

```glsl
uint slot = atomicAdd(b_RequestCount, 1u);
if (slot < uint(VSM_MAX_REQUESTS))
    b_Requests[slot] = record;      // ...and no else.
```

A page the camera can see goes unbacked for the frame and is sampled through the coarse-level
fallback instead, with no counter, no flag and no log line. It **self-heals** — `VSM_EndFrame`
strips `REQUESTS_ALLOCATION` from every entry each frame, so the page asks again next frame — and
that is exactly what makes it hard to notice: under sustained ring pressure the shadows are
persistently a little too soft and the only evidence is the pixels.

Two things are worth carrying forward from that, and the second one is the general lesson:

- **Check the self-heal before you claim a permanent bug.** The first draft of this document (and of
  the shader comment) asserted the drop was *permanent*, reasoning that
  `VSM_PAGE_REQUESTS_ALLOC_BIT` was already set and the one-request-per-page collapse would return
  early forever. That reasoning was correct and the conclusion was wrong, because a kernel three
  steps away clears the bit. Trace the whole lifecycle of any flag you are about to build an
  argument on.
- **Read what the pass does with the refused item.** The truncation is rarely the whole story: what
  matters is the state the refusing invocation has *already* mutated on the way in.

## 8. The buffer-binding namespace is FULL

`SSBO_GPU_STATS = 64` took the last number. Every value `0..83` (the GL 4.6 minimum guarantee,
exclusive) is now claimed by some namespace; 57 and 63 are reserved engine-wide for the Vulkan
vertex-pull streams; 64 was the only number never used as an SSBO — it is `TEX_DDGI_VISIBILITY` in
the sampler namespace.

Cross-namespace reuse is fine on GL and fine on Vulkan **except within one shader**, where the
single-set model makes it a real collision — the reason `TEX_DDGI_VISIBILITY` had to move off 57 in
#691 (ADR item A2). The constraint has a checkable form, and it is checked:
`GPUReadbackStatsLayoutTest.NoStatsConsumerAlsoSamplesBinding64` asserts no shader includes both
`GPUReadbackStats.glsl` and `DDGICommon.glsl`. **If that test fails, renumber — do not delete it.**

**The next feature that wants a buffer binding has to ride an existing block** (the way #703 and
#707 did) **or renumber a family.** Say so in the issue before you start, not in review.

## 9. Verifying a counter — cross-check or don't claim it

"It reports a number" is not evidence. `GPUReadbackStatsEvidenceTest` is the template; every
assertion in it is checked against something derived independently of the channel:

| Check | Independent of the channel because… |
|---|---|
| `InstanceCullInput` == CPU-submitted count | counted by the **shader**, one atomic per invocation — a CPU-written counter would agree by construction and prove nothing about whether the dispatch ran |
| `Drawn` == `indirect.instanceCount` | different buffer, different atomic, different readback path — the value the real draw consumes |
| `Drawn + Rejected == Input` | a conservation identity: one counter can be wrong and plausible, three that must sum cannot all be wrong the same way |
| `Latency > 0` | proves the number came through the ring rather than a synchronous read |

A counter with no cross-check is a counter nobody should trust, including you.

**And a test that drives the lifecycle by hand proves nothing about the lifecycle.** Every one of
those assertions calls `BeginFrame → cull → EndFrame` itself, so none of them can tell you whether
the *engine* calls them in that order — see §11, which is the bug they all missed.
`GPUReadbackStatsFullFrame.RealFrameFeedsTheInstanceCullCounters` exists for exactly that reason: it
renders a real frame through `Scene::OnUpdateRuntime` with an instance field past the GPU-cull
threshold and asserts the counters are non-zero.

## 10. Measuring "it does not stall" — the naive A/B does not work

The obvious measurement — sample frame time with the channel on, then off, compare the means —
**gave two opposite answers on consecutive attempts**: a "noise floor" of 0.70 ms one run and
0.05 ms the next, against an effect far smaller than either. An idle editor's frame time drifts on
a timescale of seconds (background load, GPU clocks, the compositor), so a difference of two
sequential means measures the drift, not the change.

The design that works is **paired and interleaved**: each block measures ON and OFF back to back,
the statistic is the mean of the per-block deltas, and the standard error of those deltas is the
noise estimate. Drift slower than one block cancels. Use a median within each block, too — one
scheduler hiccup in a 12-sample window moves a mean much further than the thing under test.

Measured that way on this box (idle editor, 5.0 ms frame, `olo_gpu_readback_stats` flipping the
channel, `olo_perf_snapshot` sampling):

| run | blocks | ON | OFF | paired delta (ON − OFF) |
|---|---|---|---|---|
| 1 | 12 | 5.110 ms | 5.147 ms | **+0.010 ± 0.072 ms** |
| 2 | 14 | 5.030 ms | 5.225 ms | **−0.156 ± 0.092 ms** |

Both within 2 SE of zero, with opposite signs — which is what sampling noise around a true effect
of ~0 looks like. The honest statement is *"the per-frame cost is not resolvable above a ±0.1 ms
noise floor"*, not "it is free": this instrument cannot see 10 µs, and claiming a number it cannot
resolve would be the same failure the rest of this document is about.

**The `enabled` argument on `olo_gpu_readback_stats` exists to make this measurable at all.** A
diagnostic that ships on by default and cannot be switched off from outside the process cannot have
its own cost audited.

## 11. The bug this feature's own review caught: where the frame bracket goes

`BeginFrame`/`EndFrame` must bracket **BeginScene..EndScene**, not the render graph.

The sibling `ShaderDebugDraw` resets itself in `RenderPipeline::UploadExecutionState`, so this
channel was written there too. `UploadExecutionState` runs inside `EndScene` — but the **GPU
instance cull dispatches at submission time**, between `BeginScene` and `EndScene`. The per-frame
clear therefore landed *after* the cull had already published, and adopter #1 reported a permanent
zero in the real engine.

Nothing caught it: the GPU evidence tests all drive `BeginFrame → cull → EndFrame` by hand, so they
exercised the order they assumed rather than the order the engine uses. `BeginFrame` now sits in
`PrepareFrame`, next to `GPUFrustumCuller::BeginFrame`, and the full-frame test above is the guard.

**That guard was validated by reintroducing the bug**, which is the only way to know a regression
test tests anything: with `BeginFrame` moved back to `UploadExecutionState`,
`RealFrameFeedsTheInstanceCullCounters` fails with its own diagnostic
(*"the GPU instance cull published nothing during a real frame"*), and passes again when the call is
restored. A new guard you have never seen fail is a guard you are guessing about.

**Generalise it:** before hooking a per-frame reset next to an existing one, check that the *work
you are instrumenting* happens inside the same bracket. "Where the similar feature does it" is a
guess about your feature's timing, not a fact about it.

## 12. Converting a synchronous per-frame republish to an async ring: the reset does not move with the read

Issue #719 (`VirtualMeshRegistry::ProcessResidency`). Not a stats counter, but the same failure
family: a buffer that mixes a **persistent** bit (residency) with **transient** bits (a
GPU-`atomicOr`'d request/touch flag) that the CPU used to republish clean, once per frame,
synchronously.

The direct instinct when moving that read onto a fenced ring (§3) is to move the *whole*
read-process-republish sequence into the async path — capture, poll, and only *then* clear the
transient bits, using whatever the polled snapshot says. That is wrong, and it does not crash or
mis-render: it silently stops the transient bits from ever being cleared for a group the poll
never happens to touch, because the poll only republishes groups whose *persistent* state changed
(the targeted-write optimization in §5 applies equally well here). A "touched" bit that is
`atomicOr`'d in every frame a page is actually visible, and never cleared once the camera looks
away, freezes an LRU timestamp at "just used" forever — no wrong pixel, no error, no crash, just
eviction picking worse and worse victims under budget pressure, days later, on a scene nobody
thought to test with tight streaming pressure.

The fix: keep the transient-bit reset on the OLD synchronous cadence — unconditional, every frame,
right after the async capture's copy is issued (not gated on whether the copy was actually taken;
see the full-ring-skip case in §3) — and let ONLY the read/decide half move onto the fence-poll
ring. The shader re-derives and re-asserts every transient bit it still needs the very next
dispatch, so nothing is lost by resetting before that snapshot is even read back; the reset and the
async read are decoupled on purpose. Caught in this PR's own `/code-review` self-review, not by any
test — the existing multi-frame streaming-convergence evidence test (which predates #719) could not
distinguish "LRU works" from "LRU never has to work because nothing else is competing for the
budget" over its own short run, which is exactly the gap a stale-touched-bit bug hides in.

**Generalise it:** when a buffer holds both persistent and transient state and you're moving its
*read* off the critical path, check whether its *write-back* was doing double duty — cleaning up
transient state as a side effect of publishing persistent state. Splitting the read from the write
without re-deriving that side effect separately drops it.
