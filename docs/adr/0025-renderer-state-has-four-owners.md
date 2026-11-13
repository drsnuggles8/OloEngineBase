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
| One flag clears both the camera's motion history and the objects' | `RenderPipeline::PrepareFrame` | A view reset drops object poses; there is no scene reset at all |
| `ResetFrameCamera`, `ResetState`, `Prepare` and `Publish` each enumerate the fields they reset | same files | Every list is missing fields that were added later |
| A binding point records only which buffer occupies it | `VulkanBindingState`; nothing on OpenGL | A buffer of another layout is a valid occupant (#1437, fixed by #1471 for one tenant) |

## 2. The four owners

| Owner | What it is | Created | Advanced | Destroyed or reset | May be reset by |
|---|---|---|---|---|---|
| **Device** | Objects made from the graphics device and never replaced while it lives: primitive meshes, shader programs, the buffer *objects* behind every fixed binding, placeholders, the GPU cull pool | `Renderer3D::Init` | never | `Renderer3D::Shutdown`, while the context is alive | the renderer only |
| **Scene** | What a loaded world put in the renderer: GPU Scene records, acceleration structures and their deform caches, material tables, skin and leaf profile slots, per-entity and per-instance previous transforms, environment identities, the scene's cloud, underwater and water publications | empty at `Init` | once per physical frame (`PrepareFrame` moves this frame's transforms to "previous") | `Renderer3D::ResetSceneState` on a scene change; `Shutdown` | the renderer; a scene change through `ResetSceneState` |
| **View** | One camera and everything measured against it: render and culling camera, previous view-projection and position, jitter, the temporal-projection observation, upscale state, LOD inputs, the retained Hi-Z pyramid's validity. Texture histories belong to the view too and live in `TemporalHistoryRegistry`, keyed by view | default at `Init` | once per view per frame (`EndScene` rotates current to previous) | `RendererViewState{}` assignment: `Shutdown`, `ResetFrameCamera` | the renderer; never by a scene change, which only invalidates histories |
| **Recording** | One replay's mutable state: the view it draws from, the bind caches, the upload objects it writes, which buffer occupies each binding point, counters. One primary context for the render thread, one per worker item | primary: `CommandDispatch::Initialize`; item: `CommandDispatchRecordingState::Prepare` on the render thread before the fork | caches emptied by `ResetState` each frame and by `Prepare` / `Publish` around a fork | primary: `CommandDispatch::Shutdown` (whole-struct assignment); item: with its `FrontendRecordingContext` | the dispatcher; a pass only through the `Invalidate*` calls and `ScopedAlternateView` |

Two smaller groups sit beside them. **Frame hand-offs** (`RendererFrameState`) are requests the scene
or the editor publishes for one frame and the renderer drains: ray-traced light requests, groom
requests, draw links, pending pass callbacks, selection outline ids, statistics. The **frame clock**
(`RendererFrameClock`) is the sampling-sequence indices and the time accumulators with their
baselines; it advances once per physical frame, `ResetFrameSequences` restarts it, and a scene change
does not.

Two rules follow from the table and are the reason for the structure:

1. **An owner's state is one struct, and reset is assignment.** `s_Data.View = {}` cannot miss a field
   added next month. A field-by-field reset list is a defect waiting for its next field.
2. **A context resets only what the table gives it.** A scene change does not restart the view's
   camera history or the clock; a view reset does not drop object poses; a recording item never
   writes the primary's state.

## 3. Migrated state

`Renderer3DData` keeps its device objects as direct members and gains four sub-structs.

| Struct | Member | Fields (groups) | Init | Mutation | Frame advance | Destruction |
|---|---|---|---|---|---|---|
| `RendererViewState` | `s_Data.View` | render camera, culling camera and frustum, previous VP / position, jitter, temporal-projection observation, upscale state, LOD inputs, `OcclusionHZBValid`, `MotionHistoryResetPending` | value-initialised | `BeginScene`, `RenderPipeline::PrepareFrame`, `SetCullingCameraFrozen` | `EndScene` rotates current to previous | assignment in `Shutdown` and `ResetFrameCamera` |
| `RendererSceneState` | `s_Data.Scene` | entity and instance transform history and its `PoseHistoryResetPending` flag, global IBL identities, cloudscape / underwater / water-disturbance / foam / wake publications, primary directional light | value-initialised | scene publication setters, submission | `PrepareFrame` moves current poses to previous | assignment in `ResetSceneState` and `Shutdown` |
| `RendererFrameState` | `s_Data.Frame` | per-frame requests, GPU Scene draw links and counters, caster sinks, pass-published texture identities, planar-reflection request, selection outline, pending callbacks and fluid draws, statistics | value-initialised | setters during the frame | cleared at `BeginScene` or when drained | assignment in `Shutdown` |
| `RendererFrameClock` | `s_Data.Clock` | jitter, upscale-phase, stochastic, cloud and fog sequence indices and their seed; cloud, fog, wind, exposure and water time accumulators and baselines | value-initialised, baselined in `Init` | `PrepareFrame`, `UploadExecutionState` | once per physical frame | assignment in `Shutdown`; `ResetFrameSequences` restarts it from a seed |

`CommandDispatchData` (one per recording context) is split the same way: `DispatchView View`,
`Uploads` (buffer objects), `Cache` (every "already bound" answer) and `Tally` (counters).
`ResetState`, `InvalidateBindingCaches`, `Prepare` and `Publish` assign `Cache = {}`; `Shutdown`
assigns the whole primary context and the pass-flag struct.

The GPU objects that belong to the scene or the view (`GPUScene`, `RayTracingScene`, the deform
caches, `HZBGenerator`, the render graph and its history registry) keep their own `Shutdown` and
stay direct members: assignment would destroy a GPU object without the release order they need.

## 4. The contract #1352 builds on

Two real scene views need exactly these six properties. Each names the production call site that
consumes it in this change.

| # | Property | Consumed by |
|---|---|---|
| V1 | The view is one value. Everything measured against a camera is in `RendererViewState` or keyed by view in `TemporalHistoryRegistry`; nothing else stores a camera, only snapshots rebuilt from it each frame | `Renderer3D::BeginScene` / `EndScene`, `RenderPipeline::PrepareFrame` |
| V2 | The recording view is set in one call. `CommandDispatch::SetView(DispatchView)` replaces seven setters; the dispatcher never holds half of one camera | `RenderPipeline::PrepareFrame` |
| V3 | A replay from another camera is a scope. `CommandDispatch::ScopedAlternateView` installs the whole alternate view in the calling recording context, uploads its camera block, withholds the main view's screen-space lanes, and restores all of it on every exit path. The alternate view declares its own history: none | `PlanarReflectionRenderPass`, the one production replay from another camera. A bucket's view override (`CommandBucket::SetViewState`) replays through the same scope; it has no production caller, is the seam #1352 replays a second view through, and is held to the scope by a test |
| V4 | Scene state advances once per physical frame and is never reset by a view. Resetting a view touches no scene state and the reverse | `ResetFrameCamera`, `ResetSceneState` (the editor's scene swap, `Scene::OnRuntimeStart`, `Scene::OnSimulationStart`) |
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
So the issue's "no context shuts down a singleton it did not own" holds in two parts, and the second
is a deliberate exception rather than a claim that none exists:

1. **None of the owned contexts shuts anything down.** A scene reset, a view reset, an alternate-view
   scope and a recording item's teardown leave every process-wide facility up
   (`RendererOwnership` asserts the dispatcher, the frame data buffer and the timer pool after each).
   Only `Renderer3D::Shutdown` takes down what `Renderer3D::Init` brought up.
2. **Two device-level shutdowns cross owners and stay.** A Vulkan device test shuts down the
   dispatcher and the timer pool it did not create, and `Renderer3D::Shutdown` releases lazily created
   3D-only statics whose only unconditional teardown it is
   ([lazy-static-release-ownership.md](../agent-rules/lazy-static-release-ownership.md)). For these the
   rule is **"what you shut down, its owner re-arms on its own backend before its next use"**:
   `Renderer3D::ReclaimSharedRenderState` does that at every `BeginScene`.

A renderer can be restarted in one process: `Shutdown` followed by `Init` gives a renderer with no
view, scene, frame, clock or recording state from the previous run.

## 5a. The binding contract

A uniform block read is described by four facts. A draw or dispatch reads a block only when they
agree with what the bound program declares:

| Fact | Declared by | Checked against |
|---|---|---|
| identity | the buffer object that occupies the binding point in this recording context | the occupancy record: `VulkanBindingState`, and `OpenGLBlockContract`, which is the same record for OpenGL |
| byte range | the buffer's size | must cover the block's size, from SPIR-V reflection on Vulkan and from the linked program on OpenGL |
| layout | the GLSL block name passed to `UniformBuffer::Create` | must equal the block's name when the buffer declares one |
| producer and lifetime intent | that block name, and the binding point's policy in `ShaderBindingLayout` | named in the refusal |

The policy has three values. Most binding points host **one layout**; the byte range is the whole
check. Eight host **several layouts** (`IsMultiLayoutUBOBinding`: 0, 2, 7, 8, 14, 18, 30 and 65, which
has eleven); every buffer the engine or the editor creates there declares its block, enforced on the
sources by `UniformBlockContractSourceTest`, which also derives the set of eight from the shaders.
Six of the eight are **pass-scoped** (`IsPassScopedUBOBinding`: 7, 8, 14, 18, 30, 65): nothing publishes
a buffer there for the frame, so each reader binds its own before it reads. Binding 8 joined them in
#1687: every pass whose shader declares the motion-blur matrices calls `Renderer3D::BindMotionMatrices`,
and the scene buckets get it from `CommandDispatch::BindSceneResources`, before any fork. The other two
are **frame-published**: the frame's camera and the dispatcher's material block are there for every pass
to read. The camera has eleven other tenants of the same layout (shadow items, the particle batch,
the bakes) and readers that never bind it (the groom and fluid passes), so its owner keeps it there:
the render graph runs `CommandDispatch::PublishFrameCamera` before every pass, and once before a
recording group forks, which binds the frame's block again if another buffer occupies the point. A
tenant that draws camera readers of its own afterwards, inside the same pass, still puts the camera
back itself (the sky and IBL bakes hold a `CommandDispatch::ScopedFrameCameraRestore`, which
re-uploads the frame's camera block on every exit). The material block needs no such rule: the
dispatcher asks the occupancy record before each draw and re-binds when another buffer is there
(#1688).

The occupancy record is also the only answer to "is this buffer already bound".
`RendererAPI::GetUniformBufferOccupant` reads it in the calling recording context, and the dispatcher's
redundant-bind check uses that where it used to keep a table of what it had last bound itself. That
table could not see a bind issued by anyone else, so it was a second owner of the same fact.

A refused read is logged once per program, binding and occupant with both blocks named, counted
(`BindingContract::ViolationCount`), and the shader is given a zero-filled block instead of the
occupant: it reads zeros, never another tenant's fields. The substitution is that program's alone on
both backends: Vulkan withholds the address for the refused draw, and OpenGL binds the occupant again
before the next program is validated, so one refusal does not zero the binding point for later readers. The check runs where the occupant is already
looked up, so it is not a second resource stack. `OLO_BINDING_CONTRACT=0` turns it off.

What it does not detect: a buffer of the *same* layout holding the wrong content (two camera
tenants), and a test's ad-hoc buffer that declares nothing, which is judged on its byte range alone.
The first needs the frame comparison in §6. The camera case is closed by the pass-entry publication
and the restore rule above, and pinned by
`FramePredecessorPoison.ASameLayoutTenantLeftOnTheCameraBindingReachesNoPass` and
`RendererOwnership.ASkyBakeLeavesTheFramesCameraBlockBound`.

## 6. Tests that pin it

| Property | Test |
|---|---|
| restart leaves no view, scene, frame or clock state | `RendererOwnership.RestartStartsFromAFreshViewSceneFrameAndClock` |
| scene reset drops scene history and keeps the view and the clock | `RendererOwnership.SceneResetDropsSceneHistoryAndKeepsTheViewAndTheClock` |
| view reset drops the view and keeps the scene and the clock | `RendererOwnership.ViewResetDropsTheViewAndKeepsTheSceneAndTheClock` |
| device teardown leaves no recording state | `CommandDispatchRecording.DispatcherShutdownLeavesNoRecordingState`; on a real Vulkan device, `VulkanParallelRecordingDevice.TheNextDeviceFindsNoUniformOccupantLeftByTheLastOne` |
| what a device teardown shut down is re-armed at the renderer's next frame | `RendererOwnership.WhatADeviceTeardownShutDownTheRendererReArmsAtItsNextFrame`, both backends |
| alternate view is whole and restored | `CommandDispatchRecording.AlternateViewIsWholeAndRestoredOnEveryExit`; with a real draw on both backends, `RendererOwnership.AnAlternateViewReplayAndARecordingItemHandTheFrameBackWhole`; the planar-reflection evidence on OpenGL |
| serial and worker replay agree | `CommandDispatchRecording.SerialAndWorkerReplayReadTheSameView`; `FrameWorkerReplay.WorkerReplayDrawsTheSerialFrameAndEveryItemHoldsTheContract` (a forked scene bucket against `OLO_VK_PARALLEL_RECORDING=0`, and a refusal raised inside the items); the parallel-recording device suite |
| a bucket's view override replays inside the scope | `CommandDispatchRecording.ABucketViewOverrideReplaysInsideTheAlternateViewScope` |
| no owned context shuts a singleton down | the `ExpectRendererSingletonsAlive` checks in `RendererOwnership.*`: after a scene reset, a view reset, an alternate-view replay and a recording item's teardown |
| the contract's verdicts | `UniformBlockContract.*` |
| the binding tables and every creation site | `UniformBlockContractSource.*` |
| an ordinary frame is refused nothing; a dirty predecessor changes nothing | `FramePredecessorPoison.APassDoesNotReadWhatItsPredecessorLeftOnASharedBinding`, three paths, both backends |
| another layout on a frame-published binding is refused before a shader reads it | `FramePredecessorPoison.AnotherLayoutOnAFramePublishedBindingIsRefusedBeforeAnyShaderReadsIt` (the light list) |
| a same-layout tenant left on the camera binding reaches no pass | `FramePredecessorPoison.ASameLayoutTenantLeftOnTheCameraBindingReachesNoPass`, three paths, both backends |
| a refused read does not take the binding from the next program | `RendererOwnership.ARefusedReadDoesNotTakeTheBindingFromTheNextProgram`, both backends |
| a bind behind the dispatcher's back cannot make it skip a bind | `RendererOwnership.ABindBehindTheDispatchersBackCannotMakeItSkipTheCameraBind`, both backends |
| a bake leaves the frame's camera block bound | `RendererOwnership.ASkyBakeLeavesTheFramesCameraBlockBound` (the face bake, the IBL convolution and the three sky generators), both backends |
| a scene activation replaces every scene render setting, skin diffusion included | `SceneRenderSettingsActivationTest.*` |
| a pass does not inherit its predecessor's blend state or write masks | `FramePredecessorPoison.APassDoesNotInheritItsPredecessorsBlendOrWriteMasks`, three paths, both backends |
| the check changes no frame it does not refuse | `FramePredecessorPoison.TheContractChangesNoFrameItDoesNotRefuse` |

## 7. What is still global

Every process-wide object that remains is audited against §4, with the reason it does not break the
contract, in
[renderer-ownership-remainder-1330.md](../analysis/renderer-ownership-remainder-1330.md).
