# A measurement is a value **and** a status. A sentinel zero is not a measurement.

**The rule.** Any number this engine measures rather than computes — a GPU timing, a readback, a
counter sampled from a device — travels with a status saying whether it is a measurement. When it
is not one, the status says which of the distinct failures applied, and the number is absent. Never
`0.0`, never `-1.0`, never "the last good value". Publish `std::optional`, a small
`{value, status}` pair, or JSON `null` plus a reason field.

**Why the zero specifically.** Zero is a *legal* value for almost everything worth measuring. A
pass that cost nothing, a counter that counted nothing and a readback that failed all arrive at the
consumer identically, and the consumer reports the failure as a fact. Every layer downstream then
compounds it: a per-pass zero becomes an "unattributed GPU time" finding, a frame zero becomes a
"CPU-bound" verdict, and a benchmark export carries the wrong number to a reader who has no way to
question it months later.

See [no-silent-fallbacks.md](no-silent-fallbacks.md) for the wider family. This file is the
measurement-shaped member of it.

---

## The failure this was written from (#1337)

`GPUPassTimerPool` timed every render-graph pass with a pair of GPU timestamps and published:

```cpp
struct PassTiming { std::string Name; f64 GpuMs = 0.0; };
```

Four unrelated failures all resolved to `GpuMs = 0.0`:

| what happened | why it produced 0.0 |
|---|---|
| the 4-slot ring wrapped before the GPU finished a frame | the slot was dropped; the published list kept the previous frame's entries |
| Vulkan refused a timestamp recorded from a `RecordParallel` worker (ADR 0011 amendment (92) rule 7) | the query kept its previous value; `end - begin` was 0 or stale |
| the two stamps came back out of order (two queue families share no timebase) | `end > begin ? diff : 0.0` — which also swallowed the legitimate equal-stamp case, see rule 7 |
| the device never had timestamp queries | the pool stayed uninitialized and published its zero-initialised state |

The checked-in study at
[vulkan-parallel-recording-1013.md](../analysis/vulkan-parallel-recording-1013.md) hit this and
said so — *"GPU timestamp samples were sometimes stale or zero; no GPU speedup is claimed"* — which
is the correct response to an instrument you cannot trust, and also the cost: a measured 26%
recording-time improvement shipped with no GPU claim attached, because no GPU number in the run
could be defended.

**None of it was visible.** The editor panel printed `0.00 ms`. The MCP tool returned `gpuMs: 0`.
`result.json` persisted it. Every one of those is a confident answer.

---

## What to do instead

**1. Name the causes, not just "invalid".** "Unavailable" is better than a zero and still not
actionable. `pending` (it will resolve), `dropped` (it never will), `notStamped` (the backend
refused), `outOfOrder` (no duration exists), `notTimed` (nothing measured it), `unavailable` (the
instrument is off) each send the reader somewhere different. See
[GPUTimingStatus.h](../../OloEngine/src/OloEngine/Renderer/Debug/GPUTimingStatus.h).

**2. Put the classification in a pure function.** The decision is where the sentinel lives, and it
is the part no GPU will reproduce on demand — you cannot ask a driver for a backwards timestamp
pair. Lift it out over plain data and it unit-tests exhaustively with no device:
`ResolveGpuTimingPair(GpuTimingPairReadout)`.

**3. Make the backend report refusals.** `WriteTimestamp` returned `void`, so a refused stamp was
unrecoverable afterwards: on GL a query object that was never stamped still answers
`GL_QUERY_RESULT_AVAILABLE` with `TRUE` and `GL_QUERY_RESULT` with `0`. It returns `bool` now. The
same applies to reads — `GetQueryResultU64` folded "could not read" into the value `0`, so
`TryGetQueryResultU64` reports it instead.

**4. A derived number inherits its inputs' validity.** `unattributedGpuMs = frameGpu - sum(passes)`
is only meaningful when the frame AND every pass were measured. With one pass missing, the
subtraction silently *attributes that pass's time to barriers and transient materialization* —
inventing a finding. It publishes `null` now, and a total assembled from a list with holes says so
(`passGpuTotalIsComplete`).

**5. Sums and elapsed times are different kinds and must be labelled.** The study measured
25.3–27.8 ms of summed worker CPU inside a 2.4 ms wall. Adding a sum to an elapsed figure, or
reading one as the other, is off by the worker count. `recordingBreakdown` in
`olo_perf_pass_timings` marks each of the seven frame measurements ELAPSED or SUM.

**6. Nesting is stated by the producer, never inferred by the consumer.** A sub-pass interval sits
inside its parent's, so summing both double-counts. That used to be re-derived from a `/` in the
pass name at three separate call sites, which also meant a pass whose own name contained a slash
was misfiled as somebody's sub-pass and dropped from the total. It is a flag on the record now.

**7. Do not overshoot: a real zero is still a measurement.** The whole change is about zeros
being lies, which makes it very easy to classify every zero as one. An interval whose two stamps
land on the same GPU tick has a stamped, readable, correctly ordered pair behind it — the work was
simply below the clock's resolution, or the pass issued no GPU commands at all. That is a
measurement of zero, and an earlier revision of this fix called it `outOfOrder`.

A live run is what caught it: on an ordinary scene, **2 of 17 passes per frame** landed there,
because `SkeletalDeformPass` and `FluidIntermediatesPass` do nothing when the scene has no skeletal
meshes and no fluid. Shipping that would have (a) sent every reader hunting a cross-queue bug that
did not exist, and (b) marked the frame's pass total incomplete on essentially every frame, which
makes `passGpuTotalIsComplete` and `unattributedGpuMs` useless. The test that only ran on synthetic
data was happy with it. **The distinction is not "is the value zero" but "is there a measurement
behind the value".**

**8. Stale is not unmeasured.** A reading from four frames ago is a real measurement of an old
frame; throwing it away to report "no data" loses the best evidence available. Keep the value, keep
its `Valid` status, and report the age separately.

---

## Auditing for the same defect elsewhere

The general shape is **a field with a consumer and no producer**. It reads its default forever
while a panel, a tool or an export presents it as measured. Sweep for it with:

```bash
python - <<'PY'   # see the #1337 PR for the full script
# For every field of a *Stats / *Counters / *Telemetry struct, look for any
# assignment, increment, compound-assign or designated initialiser anywhere in
# the tree. Report the ones with none.
PY
```

Watch for false positives: `fetch_add` on an atomic, writes through a reference
(`f64& best = cond ? a : b;`), constructor member-init lists, and GPU-side mirror structs a shader
writes. The 2026-09 sweep found 23 candidates and 6 real ones.

The six, and what happened to each:

| field | verdict |
|---|---|
| `RayTracing::FrameCounters::BlasBuildGpuNs` / `TlasBuildGpuNs` | removed; consumers repointed at the `AccelerationStructureBuild` sub-pass, which does time it |
| `RayTracing::FrameCounters::MaskedCandidatesAccepted` / `Rejected` | removed; never read either |
| `ReflectionTierStats::MaskedGeometryReflectsAsSolid` | removed. A first revision wrote `true`; review found the statement had stopped being true (the tier requires the material heap and the shader alpha-tests), so writing it would have shipped a false warning. "Never written" is not evidence the claim is still correct |
| `RendererMemoryTracker::PoolStats` (struct + map) | removed; the panel computed its whole table locally |

**When a counter has no producer, prefer deleting it over writing one.** A field that is always
zero answers the question *wrongly*; an absent field sends the reader to the channel that can
answer it. `DeformedSurfaceCache.h` had already reached this conclusion in a comment and declined
to add a GPU-time field of its own — and then the neighbouring header's two dead fields sat there
for another year because nobody was looking for them. A test that renders real frames and requires
each exported counter to have moved is what keeps the list honest;
`GpuTimingPoolEvidenceTest.EveryExportedRendererCounterHasALiveProducer` is that test.
