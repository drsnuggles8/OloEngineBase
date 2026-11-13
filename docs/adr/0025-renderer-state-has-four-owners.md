# Renderer state has four owners: device, scene, view and recording

Issue #1330 (review item ARCH-01, programme #1359). `Renderer3D` and `CommandDispatch` kept most of
their state in two process-wide structs whose fields had four different lifetimes. Each lifetime was
kept correct by a hand-written list of fields to reset, save or republish, and each list had drifted.
This ADR names the four owners, says what each owns and who may reset it, and fixes the contract the
two-view work (#1352) builds on.

The public `Renderer3D` and `CommandDispatch` facades stay. Nothing here adds a second resource stack
or exposes a backend handle.

## 1. What was wrong at `bba596110`

Read from the source, then pinned by the tests named in §6.

| Finding | Where | Effect |
|---|---|---|
| The main camera has five copies kept in step by hand | `Renderer3DData`, the dispatcher's primary data, `ParallelSceneContext`, the camera UBO, each recording item's clone | A replay from another camera swaps some copies and not others |
| The planar-reflection replay swaps 4 of the 7 dispatch camera fields | `PlanarReflectionRenderPass::Execute` | The mirror camera block carries the main view's previous view-projection and jitter |
| `CommandBucket::SetViewState` has no production caller and does not upload the camera block | `CommandBucket::ReplayPackets` | Mesh draws under a bucket view override still read the main camera |
| `Renderer3D::Shutdown` resets device objects and leaves the camera, motion history, sampling indices and pending pass callbacks | `Renderer3DLifecycle.cpp` | A restarted renderer starts with the previous run's history and callbacks |
| `CommandDispatch::Shutdown` resets five buffer references and nothing else | `CommandDispatch.cpp` | View matrices, heap epochs, pass flags and prepass shader identities survive a device teardown |
| `ResetFrameCamera`, `ResetState`, `Prepare` and `Publish` each enumerate the fields they reset | same files | Every list is missing fields that were added later |
| A binding point records only which buffer occupies it | `VulkanBindingState`; nothing on OpenGL | A buffer of another layout is a valid occupant (#1437, fixed by #1471 for one tenant) |

## 2. The four owners

| Owner | What it is | Created | Advanced | Destroyed or reset | May be reset by |
|---|---|---|---|---|---|
| **Device** | Objects made from the graphics device and never replaced while it lives: primitive meshes, shader programs, the buffer *objects* behind every fixed binding, placeholders, the GPU cull pool | `Renderer3D::Init` | never | `Renderer3D::Shutdown`, while the context is alive | the renderer only |
| **Scene** | What a loaded world put in the renderer: GPU Scene records, acceleration structures and their deform caches, material tables, skin and leaf profile slots, per-entity and per-instance previous transforms, environment maps, the scene-published weather and water settings and their clocks | empty at `Init` | once per physical frame (`BeginScene` moves this frame's transforms to "previous") | `Renderer3D::ResetSceneState` on a scene change; `Shutdown` | the renderer; a scene change through `ResetSceneState` |
| **View** | One camera and everything measured against it: render and culling camera, previous view-projection and position, jitter and its sequence index, the temporal-projection observation, upscale state, LOD inputs, the retained Hi-Z pyramid's validity, the auto-exposure clock. Texture histories belong to the view too and live in `TemporalHistoryRegistry`, keyed by view | default at `Init` | once per view per frame (`EndScene` rotates current to previous) | `RendererViewState{}` assignment: `Shutdown`, `ResetFrameCamera` | the renderer; never by a scene change, which only invalidates histories |
| **Recording** | One replay's mutable state: the view it draws from, the bind caches, the upload objects it writes, which buffer occupies each binding point, counters. One primary context for the render thread, one per worker item | primary: `CommandDispatch::Initialize`; item: `CommandDispatchRecordingState::Prepare` on the render thread before the fork | caches emptied by `ResetState` each frame and by `Prepare` / `Publish` around a fork | primary: `CommandDispatch::Shutdown` (whole-struct assignment); item: with its `FrontendRecordingContext` | the dispatcher; a pass only through the `Invalidate*` calls and `ScopedAlternateView` |

A fifth group is **frame hand-offs** (`RendererFrameState`): requests the scene or the editor publishes
for one frame and the renderer drains (ray-traced light requests, groom requests, draw links, pending
pass callbacks, selection outline ids, statistics). Created empty, cleared at `BeginScene` or when
drained, reset by assignment at `Shutdown`.

Two rules follow from the table and are the reason for the structure:

1. **An owner's state is one struct, and reset is assignment.** `s_Data.View = {}` cannot miss a field
   added next month. A field-by-field reset list is a defect waiting for its next field.
2. **A context resets only what the table gives it.** A scene change does not restart the view's
   sampling sequence or clocks; a recording item never writes the primary's state; `Shutdown` of one
   owner does not tear down a singleton another owner brought up without re-arming it (§5).

## 3. Migrated state

`Renderer3DData` keeps its device objects as direct members and gains three sub-structs.

| Struct | Member | Fields (groups) | Init | Mutation | Frame advance | Destruction |
|---|---|---|---|---|---|---|
| `RendererViewState` | `s_Data.View` | render camera, culling camera and frustum, previous VP / position, jitter, sequence indices and seed, temporal-projection observation, upscale state, LOD inputs, `OcclusionHZBValid`, auto-exposure clock, `MotionHistoryResetPending` | value-initialised | `BeginScene`, `RenderPipeline::PrepareFrame`, `SetCullingCameraFrozen` | `EndScene` rotates current to previous; `PrepareFrame` advances the sequence indices | assignment in `Shutdown` and `ResetFrameCamera` |
| `RendererSceneState` | `s_Data.Scene` | entity and instance transform history, global IBL identities, cloudscape / underwater / water-disturbance / foam / wake publications, cloud, fog, wind and water clocks and frame indices, primary directional light | value-initialised | scene publication setters, `UploadExecutionState` | `PrepareFrame` (transform history), `UploadExecutionState` (clocks) | assignment in `ResetSceneState` and `Shutdown` |
| `RendererFrameState` | `s_Data.Frame` | per-frame requests, GPU Scene draw links and counters, caster sinks, pass-published texture identities, planar-reflection request, selection outline, pending callbacks and fluid draws, statistics | value-initialised | setters during the frame | cleared at `BeginScene` or when drained | assignment in `Shutdown` |

`CommandDispatchData` (one per recording context) is split the same way: `DispatchView View`,
`Uploads` (buffer objects), `Cache` (every "already bound" answer), `Counters`. `Prepare` and
`Publish` assign `Cache = {}`; `Shutdown` assigns the whole primary context and the pass-flag struct.

The GPU objects that belong to the scene or the view (`GPUScene`, `RayTracingScene`, the deform
caches, `HZBGenerator`, the render graph and its history registry) keep their own `Shutdown` and
stay direct members: assignment would destroy a GPU object without the ordering §5 needs.

## 4. The contract #1352 builds on

Two real scene views need exactly these six properties. Each names the production call site that
consumes it in this change.

| # | Property | Consumed by |
|---|---|---|
| V1 | The view is one value. Everything measured against a camera is in `RendererViewState` or keyed by view in `TemporalHistoryRegistry`; nothing else stores a camera, only snapshots rebuilt from it each frame | `Renderer3D::BeginScene` / `EndScene`, `RenderPipeline::PrepareFrame` |
| V2 | The recording view is set in one call. `CommandDispatch::SetView(DispatchView)` replaces seven setters; the dispatcher never holds half of one camera | `RenderPipeline::PrepareFrame` |
| V3 | A replay from another camera is a scope. `CommandDispatch::ScopedAlternateView` installs the whole alternate view in the calling recording context, uploads its camera block, suspends the main view's screen-space lanes, and restores all of it on every exit path. The alternate view declares its own history: none, unless it supplies one | `PlanarReflectionRenderPass`, `CaptureStateGuard` (probe and impostor bakes), `DDGIProbeUpdatePass`, `CommandBucket::ReplayPackets` |
| V4 | Scene state advances once per physical frame and is never reset by a view. Resetting or destroying a view touches no scene or device state | `ResetFrameCamera`, `ResetSceneState` |
| V5 | Serial and worker replay read the same context type. A worker item's context is a copy of the primary's view, prepared before the fork, with its own uploads and empty caches; nothing an item needs is created inside it | `CommandBucket::Execute` / `ExecuteParallel`, `VulkanRendererAPI::RecordParallelOrdered` |
| V6 | A binding point's occupant is checked, not assumed (§5a) | every draw and dispatch on both backends |

What #1352 still has to build is everything that makes the second view exist: a second
`RendererViewState`, a view key other than zero in the history registry, per-view Hi-Z storage and
exposure, and the split of `BeginScene` into a per-frame and a per-view half. None of that is a
lifetime question any more.

## 5. Shutdown and re-arming

`Renderer3D::Init` brings up `CommandDispatch`, `FrameDataBufferManager`, `FrameResourceManager`, the
profiler and the GPU timer pools, so `Renderer3D::Shutdown` shuts them down. A Vulkan device test also
shuts the dispatcher and the timer pool down, and must, because their GPU objects belong to the
device it is about to destroy ([cross-test-renderer-state.md](../agent-rules/cross-test-renderer-state.md)).
That stays: the rule is not "never shut down what you did not create" but **"what you shut down, its
owner re-arms on its own backend before its next use"**. `Renderer3D::ReclaimSharedRenderState` does
that at every `BeginScene`. The facilities `Renderer3D::Shutdown` releases without having created them
are lazily created 3D-only statics whose only unconditional teardown is this one
([lazy-static-release-ownership.md](../agent-rules/lazy-static-release-ownership.md)); the audit in §7
lists each with its creator.

## 5a. The binding contract

A uniform block read is described by four facts, and a draw or dispatch is valid only when all four
agree with what the bound program declares:

| Fact | Declared by | Checked against |
|---|---|---|
| identity | the buffer object bound to the point in this recording context | must exist |
| byte range | the buffer's size | must cover the block's reflected size |
| layout | `UniformBlockContract` on the buffer: the GLSL block name it was built for | must equal the reflected block name when the buffer declares one; every buffer on a multi-tenant binding must declare one |
| producer and lifetime intent | the contract's block name, and the binding's policy in `ShaderBindingLayout`: *frame-published* (one owner, restored by anyone who displaces it) or *pass-scoped* (no owner; each reader binds before it reads) | reported in the violation |

A violation is logged once per program and binding with both layouts named, counted, and the shader is
given the zero-filled null block instead of the occupant: it reads zeros, never another tenant's
fields. The check runs where the occupant is already looked up, so it is not a second resource
stack. OpenGL keeps the same occupant record Vulkan had. `OLO_BINDING_CONTRACT=0` turns the check off
for measurement.

It does not detect a buffer of the *same* layout holding the wrong content: two tenants that share a
layout need the frame comparison in §6, not this check.

## 6. Tests that pin it

| Property | Test |
|---|---|
| restart leaves no view, scene or frame state | `RendererOwnership.RestartStartsFromAFreshViewSceneAndFrame` |
| scene reset keeps the view and the device | `RendererOwnership.SceneResetDropsSceneHistoryAndKeepsTheView` |
| device teardown leaves no recording state | `RendererOwnership.DispatcherShutdownLeavesNoRecordingState` |
| alternate view is whole and restored | `CommandDispatchRecording.AlternateViewIsWholeAndRestoredOnEveryExit`, the planar-reflection evidence on both backends |
| serial and worker replay agree | `CommandDispatchRecording.SerialAndWorkerReplayReadTheSameView`, the parallel-recording device suite |
| wrong occupant is refused | `UniformBlockContract.*` on both backends; the vegetation device test with its binds removed |
| dirty predecessor changes nothing | the `PoisonPassScopedBindings` and `DirtyAttachmentState` operations in the renderer state-machine harness |

## 7. What is still global

Every process-wide object that remains is audited against §4, with the reason it does not break the
contract or the child issue that owns it, in the remainder audit that lands with this change.
