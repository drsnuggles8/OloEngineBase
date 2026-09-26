# The renderer state-machine harness: where a frame ends up must not depend on how it got there

**Rule.** A renderer change that adds a cache, a lever, a pooled resource or an execution mode gets
checked by the state-machine harness (issue #1349), not only by an off/on test. Add its operation
or its pair to [the manifest](../../OloEngine/tests/Rendering/StateMachine/RendererStateMachineManifest.h)
and let generated sequences find the order that breaks it. A pair you add needs a negative control
the harness demonstrably catches; one that stays green with its fault on is not a check.

Most renderer defects here lived in a *sequence* of valid operations, not in any one of them:
Deferred re-entered after Forward (#530), upscale off at an unchanged display size (#563), a bucket
gaining its first draw under a cached graph (#1315), a blackboard re-populated under a cached graph
(#1397). Each had an off/on test that passed, because the off/on test started from a clean state.

## What it does

`RendererStateMachineFixture` drives the real renderer (`RendererAttachedTest`, GL 4.6) through a
`Trace`: a seed, an initial configuration and a list of operations. After every operation, every
pair of executions that must agree renders the same state both ways:

| pair | lever | what disagreeing means |
|---|---|---|
| cached vs forced rebuild | `VerifyDeclarationCache` for one frame | a declaration input missing from the key |
| alias vs no alias | `DisableTransientAliasing` + pool clear | two live transients shared a backing |
| batch vs no batch | geometry bucket `EnableBatching` | batching changed a draw |
| parallel vs serial submission | `SerialMeshSubmission` | `DrawMeshParallel` and `DrawMesh` disagree |
| warm vs cold binding caches | `InvalidateBindingCaches` before the frame and after every pass | a pass relied on a binding nobody reset |
| sequence vs fresh | canonical reset + direct configure | the frame depends on its history |
| before vs after a reload | shader / scene reload | a reload is not an identity |

Every pair also proves its lever acted (the pool grew, nothing batched, no worker-submitted mesh)
or records the comparison as *vacuous* rather than passing it.

## The operations are total

Every operation is legal in every state: MSAA on the forward path is set and ignored, removing a
mesh that is not there does nothing. So every subsequence of a failing trace is a valid trace, and
`MinimizeTrace` (ddmin) can drop any operation. A failure is reported with its full trace, the
minimised one, and the command that replays it:

```
OloEngine-Tests --gtest_filter=RendererStateMachineEvidence.ReplayTraceFromCommandLine \
    --olo-state-machine-replay=<file.trace> --olo-keep-temp
```

Long unattended runs: `--olo-state-machine-seeds=1,2,3,... --olo-state-machine-length=40`. The
generator is SplitMix64 with integer-only bounded draws, so a seed names the same trace on every
compiler; a CPU test pins seed 1349 against an independent Python port.

## Exact versus distribution

A target is held **bit-exact** only if its own control pair (the same state on two consecutive
frames) is bit-exact. A target whose control moves has something frame-indexed in it; that one
target is compared at **distribution** level (channel means and a log-luminance histogram within
twice the control's own difference, 16×16 tile means within three times it or 1% of the brightest
tile mean) and the run reports it as a fallback. A pair never passes because its control was noisy. TAA beauty is the deliberate
distribution case (`TemporalBeautyMatchesInDistribution`).

**Guards run before any statistic, and a missing measurement is never a tolerance** (#1492). A
target fails with a `[rejected]` reason if it is captured on one side only, if format, extent or
texel count differ, if any value is NaN or ±Inf (including the same NaN in both captures, which a
`memcmp` calls equal), or if its control could not calibrate it (captured in one control frame
only, or non-finite there). The first version turned a missing control into a mean floor of 1e30
and a histogram floor of 4, which nothing can exceed, and read NaN as black.

**Means and histograms are blind to where texels are.** A half-image swap and a moved patch have the
same mean and histogram as the original. The tile term catches them; it averages per-texel noise
away, so an independently seeded noise pair still passes. Its floor is relative to the brightest
tile, so a dim buffer is not judged on a scale of 1 and one specular texel does not loosen it.
Calibrated on the corpus: no clean comparison used more than a quarter of the tile allowance, and
the stale-key fault moved a tile by 24 times it. `RendererStateMachineComparison*` pins every case
on the CPU.

## Read intermediates at a pinned point, never after the frame

A transient's backing may legally be handed to another resource after its last reader runs. Read
it after the frame and alias-on versus alias-off compares two *alias layouts*, not two images. The
harness reads each target in a post-pass hook after its last reader (from
`RenderGraph::GetResourceLifetimes()`), which on GL changes nothing about execution.

**The control must span the comparison window.** Two consecutive frames of a temporally resolved
target that is still converging (SSR after an upscale change reset its history) can be
bit-identical, while a frame six frames later is not. The first version compared two adjacent
frames, and a lever-free round trip "failed" alongside the pair. The tell is exactly that: the
round trip changes no lever, so if it fails, the frame was drifting. The control now spans ten
frames.

**Nothing downstream of a noisy target is exact.** An 8-bit composite whose float input wobbled by
a tenth of a step rounded the same way on both control frames and the other way on a third: five
texels, one step, and only after two unrelated tests had run first (they changed how noisy the
input was). The input was correctly compared at distribution level; the composite was held exact
on a control that was stable by coincidence. Every target read later in the frame than the first
noisy one is now compared at distribution level too. Before blaming the engine for a one-step
difference in a quantised target, look at what fed it. This spread was re-examined in #1492 and
kept: with the tile term, a downstream target still fails a tile mean that moves by more than 1%
of its brightest tile, so the spread only admits sub-tile drift, and following real resource lineage would need the graph's read
sets inside a pure comparison.

## Negative controls

Three fault levers re-create known defect classes (`DebugLevers.inl`, *Fault injection*):

| fault | caught by |
|---|---|
| `FaultStaleDeclarationKey`: pass inputs left out of the key | cached-vs-rebuild (stale detection) and sequence-vs-fresh |
| `FaultSkipDispatchBindingReset`: binding caches survive the frame | warm-vs-cold binding caches |
| `FaultShortenTransientLifetimes`: lifetimes end a pass early | alias vs no alias; on the CPU, an independent lifetime model |

**A skipped reset breaks every frame alike, so sequence-vs-fresh cannot see it.** That is why the
cold-binding pair exists: it compares against an execution where the reset genuinely happened.
Pick the detector by asking what execution would be correct under the fault, not by what pair is
already there.

**Run each detector alone.** Detectors interfere: cached-vs-rebuild's verify frame recompiles the
graph at every checkpoint, which cures a stale graph before sequence-vs-fresh looks at it. With
every pair on, the stale-key fault was "caught" by six pairs and missed by the one the manifest
named. Each control now runs its trace once per named detector with only that pair enabled, and
the trace must end somewhere the stale state is wrong: a trace that returns to where the graph was
cached is correct by accident.

## What the first run found

Three engine defects, each behind a green suite, each fixed in its own commit:

- **A parallel-submission merge replaced the geometry bucket.** Every draw submitted before a
  32+-mesh Model (or 32+ animated meshes) vanished. Only the scene premises caught it: 92 packets
  submitted, 40 replayed, a lone model on an empty backdrop. **Assert what a lever needs before
  comparing through it**, or a pair compares two empty frames and passes.
- **Double buffering off deleted a frame fence twice**, destroying another owner's reissued
  fence: `GL_INVALID_VALUE` on every later frame.
- **An auto-batched Model took another entity's material on Deferred**: a reused instance scratch
  kept the previous batch's GPU-scene references. Minimised to one operation, `path deferred`.

## What it does not cover, said out loud

- **Vulkan.** `RendererAttachedTest` holds a GL context only (testing-architecture.md §9–10). The
  Vulkan rows are live-only; parallel graph recording is Vulkan-only and its pass-level half is
  `VulkanParallelRecordingDevice.*`.
- **Two real views.** Isolated-vs-shared view is a Prerequisite row owned by #1352.
- **A new process.** The canonical reset is not a process restart. Singletons the renderer never
  resets (the stochastic frame index, compiled programs, GPU-scene slot allocators) carry over. A
  minimisation that reports "did not fail again from the canonical reset" means exactly this:
  replay it in a fresh process.

## Accounting

`[ STATE MACHINE ]` prints after gtest's summary: every manifest row as executed (with comparison,
fallback and vacuous counts), SKIPPED (with the reason), not exercised, prerequisite or live-only.
On a hosted runner the device rows read SKIPPED, which is the truth, not a pass.
