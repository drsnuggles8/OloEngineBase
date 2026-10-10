# Judging a temporal estimator: paired replays, runs as samples, every reset checked on its frame

**The rules, first.** From #1348, which built the protocol and found six defects with it.

1. **Measure from paired replays.** Start every arm with `Renderer3D::ResetFrameSequences(seed)`.
   Arms with the same seed share every stochastic sample, frame by frame, and differ only in what
   the arm changes. Assert that they are **bit-identical until they diverge**. A difference
   before the divergence is state the reset did not restart, and that is a bug, not noise.
2. **The unit of independence is the run, never the frame or the reservoir's M.** Summarise each
   seeded run to one number and take the interval over runs (`Oracle::MeanOverIndependentRuns`,
   which refuses `CorrelatedFrames` and `ReservoirM`). Frames inside a run share a history.
3. **Perturb only what primary validity cannot see:** a secondary light or an occluder outside
   the frustum, with the camera and the receiver fixed. Measure the raw signal and the
   reconstruction separately, and the response against the slowest an unclipped history of that
   feedback could be: `ceil(ln 0.1 / ln feedback)` frames to 10%.
4. **Check every reset policy on the frame it fires.** On a frame with no history, the resolve at
   the shipped feedback and at feedback 0 produce the same image. On the frame before, they must
   differ, or the check sees nothing. Then run the check again under
   `Levers::FaultKeepStaleTemporalHistory`: it must fail.
5. **Report clamps, caps and bias per estimator.** These resolves clip their history, which is a
   biased mode. Report their bias against their own raw signal, and never pool it with an
   unclamped claim.

Executable halves:

- `TemporalLongSequenceEvidenceTest.cpp`: GL, TAA on every path, SSR and SSGI.
- `ReSTIRDIOracleTest.cpp`: the reservoir arithmetic under a changed integrand.
- The metric and the interval: `TemporalSequenceMetrics::MeasurePairedResponse` and
  `Oracle::MeanOverIndependentRuns`.

---

## The sampling context

`ResetFrameSequences(seed)` restarts everything a frame's sampling depends on:

- the stochastic, TAA-jitter, FSR2-phase, cloud and fog indices;
- each pass's own counters, through `RenderGraphNode::ResetFrameSequence`. These are the froxel
  fog's index and history, GTAO's noise index, DDGI's ray rotation and capture schedule, ReSTIR GI's
  previous frame, and ReSTIR PT's lineage;
- **every** registry history, through `TemporalHistoryInvalidationCause::SamplingSequenceReset`.

`Renderer3D::GetFrameSamplingContext().Fingerprint()` hashes all of it, plus the mock clock and
each history's validity and age. Compare fingerprints, not fields, so that a dimension added later
cannot be left out of an old test.

A pass that keeps a frame index of its own overrides `ResetFrameSequence` and
`GetFrameSequenceState`. Do not rewind a counter that is a dedupe stamp rather than a sampling
dimension. GTAO's classifier stamp is one: rewinding it can match a stamp already used and skip a
classification.

`seed` selects an independent run. Seed `k` starts the stochastic index `k * 2^16` frames in and
the TAA jitter at phase `k`. Two arms of a pair share a seed; repeats of a claim use different
seeds.

## The lineage AOV

Every registry history reports `Age` and `LineageCause` (in `TemporalHistorySnapshot` and in
`olo_render_graph_topology_export` as `age` and `lineageCause`):

- `Age` is the number of frames the current history has accumulated.
- `LineageCause` is what started it. Unlike `lastInvalidation`, it survives produced frames.

A reset policy is read off these two fields. A cut that left `Age` climbing kept a stale history.

ReSTIR PT keeps its path records in its own buffers, so it holds an **external** registry lineage
(`AcquireExternal`, advanced each frame by `RenderGraph::AdvanceTemporalLineage`). It has a lineage
and an age but no texture, and it stays out of the declaration key.

## The reset policy

| event | policy | cause |
|---|---|---|
| camera cut / teleport (the editor and MCP seam) | new lineage | `CameraCut` |
| projection change | new lineage | `ProjectionChanged` |
| render-scale change (Forward paths; Deferred ignores dynamic scale, #1537) | new lineage | `DynamicResolutionChanged` |
| FSR1 upscale preset change (every path) | new lineage, then imported again the next frame | `DynamicResolutionChanged` (TAA), `DescriptorChanged` (scene band) |
| `ResetFrameSequences` | new lineage, every history | `SamplingSequenceReset` |
| a tier stops declaring its history (disabled, stood down) | released | `FeatureToggled` |
| pause | kept | none |
| shader-library hot reload, layout unchanged | kept | none |
| history layout edit | new lineage | `DescriptorChanged` (registry tests) |

The engine detects no cut on its own. A runtime teleport must call
`Renderer3D::InvalidateTemporalHistories(CameraCut)`, as the editor's camera setters do.

The live hot reload is `ShaderLibrary::ReloadShaders`. It does not reach the TAA, SSR and SSGI pass
shaders, which are created outside the library, so a live edit of those files is not
hot-reloaded.

## What the protocol found

- **TAA's, SSR's and the cloudscape's colour histories were outside the registry**, behind bools of
  their own. No invalidation reached them, including a camera cut, a projection change, a sequence
  restart and every `Manual(TAA)`. Dropping TAA's surface plane alone even switched its coverage
  term off, so the kept history was trusted *more*. Each test's first frames also blended the
  previous test's image. On the base commit, a 12-frame TAA replay after a different pre-roll
  differed from a clean replay by up to 0.39 in HDR. #1489's order-dependent moving coat did not
  reproduce on HEAD from its direct predecessors: its luma drift was 14,982 alone, 15,081 after a
  TAA-off test and 14,967 after a TAA-on one. Its 30-frame replay decays an inherited history below
  what its 8-bit metrics see, so do not cite this leak as that report's cause.
- **TAA had no "no history" state.** Its pass binds the current frame as the history, and the shader
  reprojected that by this frame's velocity. So on a cut or a projection change, the very frames a
  reset exists for, TAA blended the frame with a displaced copy of itself. The UBO now carries
  `hasHistory` and a frame without history outputs the current frame.
- **`ResetFrameSequences` was not a complete reset.** It raised `JitterReset`, which ReSTIR DI/GI
  and the path tracer do not declare, and it left the FSR2 phase and every pass-local index running.
- **ReSTIR PT ignored every invalidation.** Its receiver test compares a normal and a roughness,
  which a cut to a similar surface passes.
- **SSGI's resolve is 12.8% darker than its own estimator** (SSR's 2.7%), with no drift. Without
  the clip it is 0.4%. The neighbourhood box intersects the 3x3 min/max, so raising gamma never
  turns the clip off. On a sparse estimator, most neighbourhoods miss the rare bright samples and
  clamp the converged history down. Filed as #1586: which rejection replaces the clip is a
  design decision.
- **A resized history was never imported again** until something else moved the declaration key.
  The resize happens inside the populate, after the key was captured with the history valid; the
  frame produces it valid again, so the next frame's key matched. The validity key now carries the
  descriptor. The reset check's resumption half (the frame after a reset must blend again) and
  the state-machine `cached-vs-rebuild` pair both catch it.
- **An FSR1 upscale-preset change did not reset TAA**, while a dynamic render scale did. It now
  raises `DynamicResolutionChanged` like `RenderGraph::SetRenderScale`.
- **A disabled tier kept its history valid.** A culled pass never re-acquires, so re-enabling RT
  shadows or ReSTIR DI/GI resumed reservoirs from whenever they last ran.
