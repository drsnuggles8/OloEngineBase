# Render-graph declaration configuration (#1333)

**Anything a pass's `Setup()` or `PopulateBlackboard` branches on, sizes a declaration from, or
imports must reach the declaration key, and must be read from the same place the key reads it.**
`BuildFrameGraph` and `PopulateBlackboard` skip their work while the key holds, and a `Setup()` is
not re-run on a hit. A missing input renders a stale graph with no error; an execution-only input
in the key rebuilds the graph on every frame it changes.

## Where an input goes

| The input is | Put it | And read it in Setup/populate from |
|---|---|---|
| a pipeline-level choice (path, sizes, a setting populate gates on, an imported identity) | a line in `OLO_FRAME_GRAPH_DECLARATION_FIELDS` (`FrameGraphDeclarationConfig.h`), filled in `RenderPipeline::CaptureDeclarationConfig` | `config.X` in `PopulateBlackboard`, `board.Config.X` in a `Setup()` |
| a pass's enable or readiness | nothing: `IsEnabled()` / `IsReadyForExecution()` are keyed for every pass. A pass that declares unconditionally and gates only `Execute()` on a per-frame enable overrides `IsEnableADeclarationInput()` to false (`PlanarReflectionRenderPass`) | the same virtual |
| anything else a pass's `Setup()` branches on (a bucket, a callback, a pass-local setting, a pass-owned import) | the pass's `AppendDeclarationInputs(RGDeclarationKey&)` override | the same accessor (`HasSubmittedCommands()`, `WillDispatchDenoise()`, ...) |
| read only by `Execute()` or a UBO | nowhere | anywhere |

The config's key, equality and field-by-field diff are generated from the field list, so a field
cannot be read without being keyed. Every pass is walked through `RenderPipeline::ForEachPass`,
which enumerates each pass set's `Members()` table; a `static_assert` in each set's `Reset()` fails
the build when a member is added without being listed. There is no hand-written list left to forget
an entry in.

Key the **resolved** value, not the settings behind it: `ReSTIRDIPass::ReservoirExtractionSource()`
rather than `SpatialReuse` + `SpatialPasses` (1 and 3 passes extract from the same target), a pass's
`IsEnabled()` rather than the raw `SSREnabled`, `HasSubmittedCommands()` rather than the count.

## Order of one frame

`RenderPipeline::CompileFrameGraph`: `PrepareDeclarationInputs` (the FSR1 scene-band resize and the
TAA/cloud history-storage resize, both of which change an input) → `CaptureDeclarationConfig` →
`PopulateBlackboard(config)` → `UploadExecutionState` → `BuildFrameGraph(config.ComputeKey())`. One
key for both layers. Anything that moves an input after the capture is a bug: it used to be two keys
per frame, and TAA ran without its history after every resize.

## Execution-only, by rule

Camera matrices, jitter, frame indices, time, object transforms, UBO values and **history
generations**. `TemporalHistoryRegistry::ComputeValidityKey` keys which histories exist and are
valid, never the generation: `Invalidate` bumps it on already-invalid entries, so a generation in
the key rebuilt the graph on every frame an object moved. The cost of leaving it out: a history sink
latches its token when populate acquires the history, so `CompileFrameGraph` calls
`RenderGraph::RefreshHistorySinkTokens()` right after the capture; without it, a history invalidated
again while already invalid keeps a stale token and never becomes valid again
(`FrameGraphDeclarationCacheEvidenceTest.AHistoryInvalidatedWhileInvalidComesBackOnACachedGraph`;
the verifier cannot see this one, because its forced repopulate re-latches). Ping-pong resources import both halves
under stable names and pick the current one in `Execute`, so the ping index is execution data too.

## Proving it

- `OLO_RG_VERIFY_DECLARATION_CACHE=1` rebuilds every frame the cache would have served and compares
  the compiled plan (`RenderGraph::ComputeCompiledPlanDigest`: declarations, descriptors, imported
  identities, history contracts) with the cached one. A difference logs the differing `pass:` and
  `resource:` entries once and counts a stale-cache detection. It costs a full compile per frame.
- `Renderer3D::GetFrameGraphDeclarationStats()`: compiles, cache hits, redundant compiles (the key
  moved, the plan did not), CPU time per outcome, and `LastCompileCause`, which names the config
  fields and passes that moved the key.
- Tests: `FrameGraphDeclarationConfig.EveryFieldMovesTheKeyAndIsNamedInTheDiff` (generated from the
  field list), `RenderGraphFingerprint.*` (per bug, plus the execution-only list),
  `RenderGraphDeclarationIdentity.*` (identity, reset ownership, the plan digest), and
  `FrameGraphDeclarationCacheEvidenceTest.*` (cached vs forced rebuild on the real pipeline, motion
  without recompiles).

## Identity

Import by `RHI::ResourceHandle`, never by native name: GL reissues a destroyed texture's name, and a
name-keyed import (or key) cannot see the swap.
`RenderGraphDeclarationIdentity.ARecycledNativeNameCannotReviveAHandleImportedByIdentity` shows both
halves. `RenderGraph::m_TopologyGeneration` comes from one process-wide counter, so a new graph can
never report a generation an older one used.

## Appendix: the inventory at `c5cf5c6a9`, what Setup read versus what the key hashed

`BuildFrameGraph(key)` and `PopulateBlackboard` both skip their work when
`ComputeBlackboardFingerprint` matches the previous frame. Any input either of them branches on,
sizes a declaration from, or latches for `Execute()` must therefore be in the key. The table lists
every input that was not, found by reading all 60 `Setup(RGBuilder&, FrameBlackboard&)` bodies
and the whole of `PopulateBlackboard` against the hashed list.

### Under-invalidation: read, not hashed (all fixed by this change)

| # | Input | Read by | What a warm cache does |
|---|---|---|---|
| U1 | Graph physical (display) size | `PopulateBlackboard`: every display-res post output, Bloom mips, water/fluid refraction, UIComposite, OIT, TAA/cloud/outline specs | Only the scene band was hashed. Under FSR1 a display resize that floors to the same band keeps every post target at the old size. |
| U2 | `Precipitation.Enabled && (ScreenStreaks \|\| LensImpacts)` | `PopulateBlackboard` (`PrecipitationColor`), `PrecipitationRenderPass::Setup` | On: the pass stays culled and draws nothing. Off: the cached rename survives while `Execute` returns early, so Fog/ChromAb/ColorGrading read an unwritten transient. |
| U3 | `GTAODenoiseEnabled && GTAODenoisePasses > 0` | `GTAORenderPass::Setup` (declares and latches the denoise pong) | Turning denoise on: `Execute` finds no pong and publishes "no occlusion" every frame. |
| U4 | `EASU` / `DepthVelocityUpscale` readiness | `PopulateBlackboard` (`EASUColor`, `UpscaledDepthVelocity`) | Both passes were missing from the hand-written `HashPassState` list. |
| U5 | TAA history import after a resize | `PopulateBlackboard` | The flag is hashed before `EnsureHistoryStorage` clears it; the next frame reproduces the cached key and TAA runs history-less. SSR had a forced invalidation for this; TAA did not. |
| U6 | Particle render callback present | `ParticleRenderPass::Setup`, OIT prepare/resolve `m_HasContributors` | A frame without a scene callback caches a culled particle node. |
| U7 | The two cache layers' keys | `Renderer3DFrameExecution.cpp` vs `PopulateBlackboard` | The build key was computed before the FSR1 scene-band resize, the populate key after it: two keys for one frame. |
| U8 | ReSTIR DI/GI history planes other than the reservoir sample | ReSTIR Setup latches all five | Only one plane's validity was hashed; correct today only because the planes invalidate together. |
| U9 | DDGI probe-data import descriptor | `DDGIProbeUpdatePass::Setup` | Sized from the probe resolution and cascade count, neither of which was hashed; only the texture was. |
| U10 | ReSTIR PT `StandDown()` inside `Setup` | `ReSTIRPTPass::Setup` | `Active` flips after it was hashed. |

### Over-invalidation: hashed, not a declaration input (all removed)

| # | Hashed input | Why it does not change a declaration |
|---|---|---|
| O1 | Environment-map identity | Populate imports irradiance/prefilter/BRDF only; the environment map is bound in `Execute`. |
| O2 | `RayTracedReflection.TierDebugView` | A UBO flag in `Execute`. |
| O3 | `SkinDiffusion.Quality` | The tap table is rebuilt in `Execute`; `Setup` reads only the enable. |
| O4 | Raw `SSREnabled` / `SSGIEnabled` / `ContactShadowEnabled` / `RayTracedReflection.Enabled` / `GpuPathTracer.Enabled` | Each is folded into a resolved per-pass verdict that was also hashed; the raw bit rebuilds the graph on paths where the verdict cannot change (forward, no ray tracing). |
| O5 | Requested `ActiveAOTechnique` | Populate reads the graph's technique; the requested one only reaches a log line. |
| O6 | `ReSTIRPT.SpatialReuse` | `ReSTIRPTPass::Setup` reads only `Active`. |

### Execution-only (never a declaration input)

Camera matrices, view position, jitter, `StochasticFrameIndex`, cloud frame index, time, object
transforms and every UBO value are pushed by the uncached `ConfigurePassesForFrame`. No `Setup` and
no populate branch reads them.

Ping-pong is not frame-parity dependent anywhere: the DDGI atlases, the froxel-fog volumes and the
ReSTIR PT pools import BOTH halves under stable names every build and choose the current half in
`Execute`; the JFA and GTAO denoise ping/pong alternate inside one `Execute`; the ReSTIR spatial
extraction source depends on `SpatialPasses`, not on the frame.
