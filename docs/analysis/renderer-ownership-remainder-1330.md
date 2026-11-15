# Renderer ownership: what is still process-wide after #1330

The audit [ADR 0025](../adr/0025-renderer-state-has-four-owners.md) §7 points at. Issue #1330 closes
only when every remaining process-wide object in the 3D renderer's submission and replay path is
either shown not to break the contract in ADR 0025 §4 or owned by a scored child issue. This is that
list, grouped by the argument that covers each group. It was built from a file-by-file inventory at
`bba596110` and re-checked after the migration.

## How to read it

Four arguments cover almost everything. Each row names the one that applies.

- **Device**: created from the device once, never replaced while it lives, released by the teardown
  that owns it. Two views and a scene reload cannot observe it.
- **Frame reset**: emptied at the start of every frame by an assignment (`CommandDispatch::ResetState`
  assigns the bind cache and the counters; `Renderer3D::BeginScene` clears the hand-offs), so nothing
  written outside a frame or in the previous one reaches a draw.
- **Recording**: reached only through the calling recording context (the thread-local selected by
  `ScopedFrontendRecordingContext` or the Vulkan worker context), so the primary and every item have
  their own.
- **Diagnostic**: warn-once latches, counters and logs. They change what is printed, never what is
  drawn.

## Shown not to break the contract

| State | Where | Argument |
|---|---|---|
| Primitive meshes, shader programs, the buffer objects behind fixed bindings, placeholders, GPU cull pool, MSAA caps | `Renderer3DData` direct members | Device. Created in `Init`, released in `Shutdown` in a fixed order; `RendererShutdownTest` checks every GPU-owning member has a release site |
| `GPUScene`, `RayTracingScene`, the three deform caches, material tables, `RayTracingProbe` | `Renderer3DData` direct members | Scene-owned GPU objects with their own `Shutdown`; `ResetGPUScene` drops the records on an asset reload. Not assigned, because the release order between them is the point (ADR 0025 §3) |
| Render graph, `TemporalHistoryRegistry`, the pass pipeline | `s_Data.RGraph`, `s_Data.Pipeline` | View-owned objects with their own teardown (`Pipeline->Reset()`, `RGraph->Shutdown()`); histories are keyed by view and invalidated by `ResetFrameCamera` and `ResetSceneState` |
| `HZBGenerator OcclusionHZB` | `Renderer3DData` direct member | Device object; its validity flag is in `RendererViewState`, so a view reset or a restart cannot use a stale pyramid |
| `ParallelSceneContext` | `Renderer3DData::ParallelContext` | A per-frame snapshot of the view rebuilt in `PrepareFrame` before submission workers start, read-only while they run. Its shader references are device objects, dropped in `Shutdown` |
| Renderer settings, post-process settings, MSAA caps, graph topology keys | `Settings`, `PostProcess`, `ActiveGraphPath` and friends | Application configuration: it must survive a restart, a scene change and a view reset, and `RendererStateCheck` restores it between tests. The eight scene render settings among them (post-process, snow, wind, snow accumulation, snow ejecta, skin diffusion, precipitation, fog) and the rendering path are the scene's: `SceneTransition::ApplySceneRenderSettings` overwrites all of them on every activation, which is their reset (#1689) |
| `CommandDispatch` dispatch table, bucket replay classifier and view-scope runner, packet dispatch resolver | `CommandDispatch.cpp`, `CommandBucket.h`, `CommandPacket.cpp` | Device. Function pointers set by `Initialize`, identical on every call, read-only during replay |
| `CommandDispatch` pass flags (`CommandDispatchFrameData`) | `CommandDispatch.cpp` | Frame reset, and frozen for a recording region: every setter asserts it is not called from an item. Assigned whole by `Shutdown` |
| `FrameDataBufferManager`, `FrameResourceManager`, packet allocators | `Commands/` | Frame reset (`Reset` and `BeginFrame` each frame); brought up and shut down by `Renderer3D` only |
| Bound-program facts: bindless flag, material-offset flag, `VulkanShader` / `VulkanComputeShader` current program | `Shader.cpp`, `Platform/Vulkan` | Recording. The material-offset flag is thread-local and published on every recording thread (#1436); the bindless flag is written by the render thread only and frozen across a fork; an item's current program lives in its worker context |
| `VulkanBindingState` (which buffer occupies each binding point), heap offset table, profiler recording stats | `Platform/Vulkan`, `HeapBindingSeam.cpp`, `RendererProfiler.cpp` | Recording. One per recording context; an item's copy is seeded at the fork and never copied back. The occupant is now checked against the reading block (ADR 0025 §5a) |
| `OpenGLBlockContract` occupancy record | `Platform/OpenGL` | Recording, on the one context OpenGL has. It mirrors driver state and is confirmed against the driver before a read is refused |
| Vulkan backend singletons (frame arena, descriptor caches and heaps, pipeline builder and cache, secondary pools, deferred reclaim, image and root-object registries) | `Platform/Vulkan` | Device. Leaked heap singletons released by `VulkanDevice::Shutdown`; each is locked or per-item where a worker touches it ([vulkan-parallel-recording.md](../agent-rules/vulkan-parallel-recording.md)) |
| `RenderCommand::s_RendererAPI` | `RenderCommand.cpp` | Device. Replaced only by backend selection before any use |
| `AssetPreviewRenderer`, `IBLPrecompute`, sky bakes: private shaders, meshes, framebuffers and buffers | `Renderer/Preview`, `IBLPrecompute.cpp`, the sky sources | Device objects with an owner teardown. They draw outside `BeginScene`/`EndScene` or behind `CaptureStateGuard`; what they leave in the dispatcher's texture and render-state caches is covered by Frame reset. Each bake re-uploads the frame's camera block before it returns, so its own camera buffer does not stay on the frame-published camera binding (#1687) |
| Shadow placeholders, `MeshPrimitives`, GPU timer pools, profiler, readback stats, debug-draw channels | lazily created statics | Device, each released by the narrowest unconditional teardown ([lazy-static-release-ownership.md](../agent-rules/lazy-static-release-ownership.md)); `Renderer3D::ReclaimSharedRenderState` re-arms the two a device test shuts down |
| Instance upload scratch (`thread_local` vector in `DrawMeshInstanced`) | `CommandDispatch.cpp` | Recording: one per recording thread, cleared and refilled by each draw before it is read. It holds capacity, never a value a later draw sees |
| `s_LastTemporalUpscaleTime` | `RenderPipeline.cpp` | A raw wall-clock sample for FSR2 lock decay. A restart or a second view reads one stale sample, which the 0.1 s clamp bounds. Not read under a mocked clock, where the delta is a nominal 1/60 s |
| Warn-once sets and flags, change-detection statics for logs, `CommandLifecycle` violation counters | submission and pipeline sources | Diagnostic |
| `InflightFrameManager::GetFrameUniformBuffer` | `InflightFrameManager.cpp` | No caller. Its buffer is the one `OLO_BLOCK_UNDECLARED` exemption from the block declaration rule and is marked where it is created |

## Found by this audit and fixed in the same change

Three remainders were filed as children first and then decided and fixed here.

| # | What it was | What was done |
|---|---|---|
| #1687 | A tenant that displaced a frame-published uniform binding did not put the owner back. The sky and IBL bakes left their camera buffer on binding 0, shadow items and the particle batch leave theirs for the next pass, and the Gaussian-splat sort block shares binding 8 with the motion-blur matrices | Binding 8 is pass-scoped: every reader binds the matrices (`Renderer3D::BindMotionMatrices`; the scene buckets through `BindSceneResources`). The camera's owner re-publishes it before every render-graph pass (`CommandDispatch::PublishFrameCamera`), and the bakes re-upload the frame's camera block on every exit. `FramePredecessorPoison` poisons 8 with another layout and 0 with a same-layout tenant after every pass, on three paths and both backends; `RendererOwnership.ASkyBakeLeavesTheFramesCameraBlockBound` pins each bake |
| #1688 | The dispatcher's `BoundUBOs` skip cache was a second record of what occupies bindings 0 to 7. It did not see a direct `Bind()` | The cache is gone. `BindUBOIfNeeded` asks the backend's occupancy record in the calling recording context (`RendererAPI::IsUniformBufferBound`; on OpenGL the record is trusted for a skip only for a point bound through the engine since the dispatcher last dropped its caches, because a raw GL bind goes around it). `RendererOwnership.ABindBehindTheDispatchersBackCannotMakeItSkipTheCameraBind` pins it |
| #1689 | Skin diffusion was the one scene-visible render setting with no scene owner: it lived only in the renderer and survived a scene change | `SkinDiffusionSettings` is a scene setting like the other seven: on `Scene`, in `SceneRenderSettings`, in the scene file, the play-mode copy, the save game (format 50) and the editor's undo snapshot. `SceneRenderSettingsActivationTest` pins activation, the round trip, a file without the node, and a corrupt value |

## Invalidation calls

The acceptance criterion asks for the invalidation calls as well as the globals. Outside the
dispatcher there are 41, to six functions. All of them write one object: the calling recording
context's skip cache (`CommandDispatchData::Cache`), which records what the dispatcher last put on
a texture unit, which program and vertex array it bound, which render state it applied, and whether
the GPU Scene material table is bound. Since #1688 it holds no uniform buffer record. It is
assigned whole at every frame start (`ResetState`) and at every replay entry
(`CommandBucket::BeginReplay`), and a recording item starts with an empty one. So the cache carries
nothing from one replay to the next, and nothing across a frame, a scene, a view or an item, which
is the contract in ADR 0025 §4.

The replay-entry reset is new with this issue, and the audit of these calls is what showed it was
needed. The calls below are the ones that exist; the defect was a call that did not.
`DeferredGPUOcclusionPass` and `GPUDrivenOcclusionPass` dispatch the phase-2 cull through the API
and then replay their recovered instances with no invalidation in between. When the scene pass had
ended on the same G-Buffer program, the dispatcher skipped its program bind and the instances were
drawn with the cull's compute program bound: one centred black rectangle in the G-Buffer albedo and
no instances. Which program the scene pass ends on follows the sort order, so it showed in one test
order of a full sweep (`RenderTargetExportEvidence.DeferredResolveIncludesRecoveredPhaseTwoInstances`
after a renderer restart) and nowhere else. A cache that needs every pass to remember a call has
that failure at every pass that forgets, so the cache now ends with the replay.

| Call | Sites | Who calls it, and why the cache cannot know |
|---|---|---|
| `ResetState` | 2 (`RenderPipeline.cpp`) | The frame start itself. The second mention is a comment |
| `InvalidateBindingCaches` | 6 (`GBuffer.cpp`, `DecalRenderPass` x2, `ForwardOverlayRenderPass`, `SceneRenderPass`, `WaterRenderPass`) | A pass entry that follows work which bound programs and textures around the dispatcher. Asserted not to run inside a recording item. Since the replay-entry reset these matter only for what a pass dispatches without a replay: the scene-resource publication before the fork, and `DecalRenderPass` executing its G-Buffer packets itself |
| `InvalidateRenderStateCache` | 20 (`CaptureStateGuard`, DDGI x2, decal x4, fluid x2, foliage x2, overlay, GPU occlusion, groom, overdraw, scene x2, debug draw, water x2) | A pass that set blend, depth, stencil or write masks through `RenderCommand` or the graph context, so the dispatcher's "last applied state index" no longer describes the context |
| `InvalidateTextureSlot` | 5 (`GroomRenderPass` x4, `VirtualGeometryPass`) | A raw bind of a texture unit behind the cache |
| `InvalidateTextureBinding` | 5 (`CloudShadowMap`, `VolumetricShadowMap`, `OpenGLTexture` x2, `OpenGLUtilities`) | A texture whose storage was recreated in place: the identity survives and the driver object does not |
| `InvalidateGPUSceneMaterialBinding` | 3 (`GPUFrustumCuller`) | The instance cull binds its indirect buffer on the storage binding the material table shares |

Every one of these is a writer outside the dispatcher telling it that driver state it mirrors has
changed. `CommandDispatchRecording.AReplayDoesNotTrustWhatWasBoundBeforeIt` shows that a replay does
not depend on any of them having been made before it. Two more checks cover the calls made inside a
replay and the state outside the cache: the state-machine
harness renders the `binding-cache-cold` arm with the whole cache forgotten before the frame and after every pass, and
requires the same frame; `FramePredecessorPoison.APassDoesNotInheritItsPredecessorsBlendOrWriteMasks`
leaves a predecessor's blend state and write masks after every pass on three paths and both backends.
The uniform-buffer half of this cache was the one part with a second, better record in the backend,
and it is gone (#1688). Texture units and render state have no such record to ask, so the cache and
its invalidation calls stay.

Per-view storage for a second view (a Hi-Z pyramid, exposure state and a non-zero view key in the
history registry) was not a remainder of this issue. #1352 built it on the contract:
[ADR 0026](../adr/0026-a-scene-view-is-one-struct-and-a-frame-is-one-step-of-the-world.md). Three rows
of the table above read differently since. The render graph, the history registry, the pass pipeline
and `HZBGenerator OcclusionHZB` are each **per view** (members of `RendererViewResident`), and so are
the renderer and post-process settings; `ParallelSceneContext` stays one shared per-view-frame
snapshot, rebuilt in `PrepareFrame` for the view being rendered. `s_LastTemporalUpscaleTime` is gone:
the baseline is the view's (`RendererViewClock::TemporalUpscalePrevTime`).
