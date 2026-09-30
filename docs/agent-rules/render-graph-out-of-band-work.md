# Out-of-band work in the render graph: declare it, and let the ledger check it

**Rule: when a pass reads or writes something the graph cannot back as a resource, declare it as
a named out-of-band boundary in `Setup()` and record the access with `RGOutOfBand::Note()` where it
happens.** Examples are a TLAS, a buffer reached by device address, the retained occlusion
pyramid, and CPU state one pass hands another. `DependsOnPass("Name")`, `NeverCull` and
registration order are not declarations: none of them says *what* flows, and the scheduler, the
culler and the parallel recorder cannot check them.

Source: [RenderGraphOutOfBand.h](../../OloEngine/src/OloEngine/Renderer/RenderGraphOutOfBand.h)
(issue #1331).

## How to declare

| The pass… | Setup declares |
|---|---|
| writes GPU state the graph cannot back | `builder.WriteOutOfBand(RGOutOfBandBoundaries::X)` |
| reads this frame's value of it | `builder.ReadOutOfBand(X)` |
| reads the value the previous frame left, before a rebuild in place | `builder.ReadOutOfBand(X, RGOutOfBandEpoch::PreviousFrame)` |
| prepares CPU data for a later pass (a bucket, a texture id, a flag, a parameter block) | `builder.Publish(X)` |
| consumes such data | `builder.ConsumePublication(X)` |

A new boundary is a row in `kProductionBoundaries` (`RenderGraphOutOfBand.cpp`). The row names
its owner, the reason it is not a graph resource, and what the frame prologue and epilogue may do
with it. What the edges do:

- **Order.** Every writer runs before a current-frame reader, and a previous-frame reader runs
  before every writer, whatever the registration order. Writers are chained in registration order,
  retained readers first. A resource edge derived in registration order that contradicts one (two
  passes writing the same export) wins, and `ValidateCompiledResourceHazards` reports
  `OutOfBandOrdering`: fix the registration or the accesses.
- **Reachability.** A current-frame reader keeps its writers alive. A previous-frame read, a
  writer chain and a write after read are *ordering-only*: they never keep the earlier pass
  alive. So a boundary can replace `NeverCull` when its consumers are declared.
- **Physical planners.** None: barriers, transients, the resource registry and the submission
  plan never see a boundary. The owner records its own barriers exactly as before.

Work that runs wholly before or after `RenderGraph::Execute` is named in `kFramePhaseWork`. A
graph resource read after `Execute` is a **frame-epilogue read**:
`graph.DeclareFrameEpilogueRead(name, consumer)`, declared before the compile. It roots the
writers, keeps a pooled backing alive to the end of the frame, and resolves the object before the
pool takes it back (`GetFrameEpilogueFramebuffer` / `GetFrameEpilogueTexture`). Every
side-effecting pass has a reason in `kSideEffectReasons`, and `olo_render_graph_schedule` prints
`UNDOCUMENTED` for one without.

## How it is checked

- **At compile.** `ValidateCompiledResourceHazards` also reports an unregistered or mis-kinded
  boundary and any declared pair on the wrong side of a write (`OutOfBandOrdering`).
- **At run time.** The ledger opens in `BeginScene` and attributes every `Note()` to the frame
  prologue, the executing pass (Prepare, Execute and Publish all run on the caller), or the
  epilogue. `EndScene` calls `ValidateOutOfBandLedger()`. It reports an access nobody declared,
  a phase the boundary does not allow, a current-frame read that ran before another pass's write,
  and a previous-frame read that ran after one.
- **Reorder.** `Levers::RenderGraphReverseTieBreak` (`OLO_RENDERGRAPH_REVERSE_TIE_BREAK=1`) runs
  every unordered pass pair the other way round. The #1349 harness pair `schedule-reversed.gl`
  requires identical texels.
- **Negative controls.** `RGOutOfBand::SetOmittedDeclarationFault("Pass/Boundary")` (or the env
  var `OLO_FAULT_OMIT_OUT_OF_BAND_DECLARATION`) drops a declaration while the access still runs.
  A check that stays green with it is not a check.

## Traps

- **A "ran this frame" flag a consumer gates on must be stamped** with
  `RGOutOfBand::GetFrameSerial()`. A culled producer never runs the `Execute` that would clear
  it. Dropping `NeverCull` from VolumetricFogPass made FogPass composite a volume from frames ago,
  until `RanThisFrame()` compared the serial.
- **Note the access, not the query.** `RayTracingScene::GetTlasDeviceAddress()` is a readiness
  check that the prologue also calls; `GetTlasDeviceAddressForTrace()` is the read and notes it.
- **Reachability walks edges and reads in one fixpoint.** A pass reached through a read used to
  lose its own `DependsOnPass` producers, and a kept readback lost the producer of what it read.
  Side-effecting passes are now roots.
- **A write after a read is an edge** (ordering-only). An in-place overwrite of a resource an
  earlier pass reads used to be ordered by registration alone.

## The audit behind it (issue #1331, `7c16fc71e`)

| Item | Before | Now |
|---|---|---|
| TLAS build → six RT tracers, MCP ray probe | `DependsOnPass("RayTracingScenePass")`, `NeverCull` | `SceneTLAS`; NeverCull kept for the refit chain and the probe |
| Deformed vertices → BLAS build | `DependsOnPass("SkeletalDeformPass")`, `NeverCull` | `DeformedVertices`; SkeletalDeform no longer NeverCull |
| Retained occlusion pyramid: prologue cull, VG phase 1, three in-place rebuilds, epilogue rebuild | registration order (VG before DeferredGPUOcclusion); the epilogue read a released SceneColor transient | `OcclusionHZB` with a previous-frame read; epilogue read of `SceneColor` / `GBufferResolved` |
| Forward+ cluster lists → DeferredLighting, VolumetricFog | `DependsOnPass("ScenePass")` / coincidental | `ForwardPlusLightClusters` |
| Scene opaque bucket → PlanarReflection, Overdraw | `DependsOnPass("ScenePass")` | `SceneOpaqueCommandBucket` |
| Froxel volume → FogPass | `DependsOnPass`, `NeverCull` | `FroxelFogVolume`; stamped flag; no NeverCull |
| Planar reflection → Water | registration order, `NeverCull` | `PlanarReflectionTexture`; no NeverCull |
| DDGI atlases → lit passes | registration order, `NeverCull` | `DDGIProbeVolume`; NeverCull kept (undeclared shader samplers) |
| Fluid intermediates → composite | `DependsOnPass`, `NeverCull` | `FluidIntermediates`; stamped flag; no NeverCull |
| SSAO parameter block → AOApply (prepared) | coincidental via AOBuffer | `SSAOParameters` |
| VSM mark globals → lighting | registration order | audited, not a dependency: the mark republishes the camera position ShadowPass already set, and sampling reads no other field it writes |
| DDGI screen requests read last frame's depth | undeclared, by design | still undeclared (best effort); now provably before this frame's depth writes |
| GPU-scene uploads, simulations, readback closes, timers, fence | outside `Execute` | named in `kFramePhaseWork` |
