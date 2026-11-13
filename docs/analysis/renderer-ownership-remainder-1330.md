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
| `s_LastTemporalUpscaleTime` | `RenderPipeline.cpp` | A raw wall-clock sample for FSR2 lock decay, deliberately not mockable. A restart or a second view reads one stale sample, which the 0.1 s clamp bounds |
| Warn-once sets and flags, change-detection statics for logs, `CommandLifecycle` violation counters | submission and pipeline sources | Diagnostic |
| `InflightFrameManager::GetFrameUniformBuffer` | `InflightFrameManager.cpp` | No caller. Its buffer is the one `OLO_BLOCK_UNDECLARED` exemption from the block declaration rule and is marked where it is created |

## Found by this audit and fixed in the same change

Three remainders were filed as children first and then decided and fixed here.

| # | What it was | What was done |
|---|---|---|
| #1687 | A tenant that displaced a frame-published uniform binding did not put the owner back. The sky and IBL bakes left their camera buffer on binding 0, and the Gaussian-splat sort block shares binding 8 with the motion-blur matrices | Binding 8 is pass-scoped: every reader binds the matrices (`Renderer3D::BindMotionMatrices`; the scene buckets through `BindSceneResources`). The bakes re-upload the frame's camera block before they return. `FramePredecessorPoison` now poisons 8 after every pass on three paths and both backends; `RendererOwnership.ASkyBakeLeavesTheFramesCameraBlockBound` pins the camera |
| #1688 | The dispatcher's `BoundUBOs` skip cache was a second record of what occupies bindings 0 to 7. It did not see a direct `Bind()` | The cache is gone. `BindUBOIfNeeded` asks `RendererAPI::GetUniformBufferOccupant`, which reads the backend's occupancy record in the calling recording context. `RendererOwnership.ABindBehindTheDispatchersBackCannotMakeItSkipTheCameraBind` pins it |
| #1689 | Skin diffusion was the one scene-visible render setting with no scene owner: it lived only in the renderer and survived a scene change | `SkinDiffusionSettings` is a scene setting like the other seven: on `Scene`, in `SceneRenderSettings`, in the scene file, the play-mode copy, the save game (format 50) and the editor's undo snapshot. `SceneRenderSettingsActivationTest` pins activation, the round trip, a file without the node, and a corrupt value |

Per-view storage for a second view (a Hi-Z pyramid, exposure state and `ParallelSceneContext` per
view, and a non-zero view key in the history registry) is not a remainder of this issue. It is what
#1352 builds on the contract, and ADR 0025 §4 lists it.
