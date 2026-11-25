# A scene view is one struct, and a physical frame is one step of the world

Issue #1352 (review item CAP-01, programme #1359). [ADR 0025](0025-renderer-state-has-four-owners.md)
named the renderer's four owners and left one thing unbuilt: the second view. This ADR decides what a
view owns, how a caller renders two of them in one frame, what advances once per frame instead of once
per view, and what one view can do to another (nothing).

The public `Renderer3D` facade stays, and a caller that never creates a view sees no difference.

## 1. What coupled two views at `c341429e4`

Read from the source, then pinned by the tests in §7.

| Finding | Where | Effect with a second `BeginScene` in the same frame |
|---|---|---|
| Every `BeginScene` rotates object poses to "previous" | `RenderPipeline::PrepareFrame` | The second view's previous pose is this frame's: zero object motion |
| Every `EndScene` extracts and commits the GPU Scene | `Renderer3D::EndScene` | Previous transforms rotate again, a different render origin re-encodes every record and rebases the TLAS |
| Wind, snow, wake, spray, precipitation and the cloud and fog clocks step per `EndScene`, centred on that camera | `UploadExecutionState` | The second view steps them with a zero time step and drags their windows to its camera |
| One time baseline for eye adaptation and for the temporal upscaler | `ConfigurePassesForFrame` | The second view's time step is zero: its exposure never adapts, its locks never decay |
| One jitter index, one cloud and one fog reprojection index | `RendererFrameClock` | Each view walks every other phase of its sequence |
| One Hi-Z pyramid, one light grid, one shadow map set, one occlusion-query ring | `Renderer3DData`, `OcclusionQueryPool` | A view culls against the other's depth, reads the other's light tiles, and finds the other's shadow pages marked |
| Every pass keeps its history in members of one pass object | every temporal pass | Two cameras accumulate into one history |
| The scene's animation clock and streaming budget open per render | `Scene::ProcessScene3DSharedLogic` | The second view's water and foliage velocity is zero; a frame may upload twice its budget |
| The descriptor heap's transient ring advances per graph execute | `RenderGraph::Execute` | On Vulkan the next frame is handed a sub-ring the first view's commands still index |

## 2. A view owns a whole render stack, and the active one lives in `s_Data`

A view is `RendererViewResident`, one struct:

| Member | Why it is the view's |
|---|---|
| `View` (`RendererViewState`) | the camera and everything measured against it (ADR 0025) |
| `ViewClock` | the sequences that pace one view's accumulation: jitter, upscale phase, cloud and fog reprojection, the exposure and upscaler time baselines |
| `Settings`, `PostProcess` | a view's path, sample count, upscale mode and effect stack are its own |
| `RGraph`, `Pipeline` | the frame graph with its history registry, and every pass object: whatever a pass keeps between frames is kept per view without any pass knowing views exist |
| `OcclusionHZB` and its toggles | the pyramid holds the depth this view rendered |
| `ForwardPlus` | the light grid is sized by this view's resolution and filled from its camera |
| `Shadow` | the cascades fit this view's frustum, and the virtual shadow map marks pages from this view's depth one frame ahead |
| the decal-visibility ring | a two-frame query ring |
| `Reported` | what the view last logged about its own graph. A latch two views shared would flip every frame they differ, repeat its log line and, outside Dist, re-run the compiled-hazard validation every view frame |

`Renderer3DData` derives from it, so the **active** view's copy is the base sub-object of `s_Data` and
every reader keeps writing `s_Data.View`, `s_Data.RGraph`, `s_Data.Settings`. Every other view's copy
is parked in its slot. `ActivateView` moves the whole struct out and the target's in. A member added
to the struct belongs to a view without being named anywhere else, which is ADR 0025's rule that an
owner's state is one struct. Whatever is not in it is shared: device objects, the scene
(`RendererSceneState`, the GPU Scene, the acceleration structures), the frame's hand-offs and the
frame clock.

The alternative was to keep one pipeline and key each pass's state by view. It needs every pass with
a member that survives a frame to know about views, and the next such member added without a key is
a leak nothing reports. A whole pipeline per view isolates by construction. Its cost is memory, and it
is measured ([multi-view-cost-1352.md](../analysis/multi-view-cost-1352.md)).

Four things outside `s_Data` name a view-owned object and are re-pointed on activation: the debug
runtime's active graph, the out-of-band ledger, the dispatcher's light grid, and the occlusion-query
pool and state manager, which keep one instance per view. The slot of the view that became active is
emptied by assignment, so a member whose move leaves its source holding a GPU object cannot keep one
alive there.

The history registry of a view's graph is bound to the view's id
(`TemporalHistoryRegistry::BindToView`): every key is stored under it, whatever view the pass wrote.

A view's GPU objects have one bring-up and one teardown, `InitializeActiveViewGPU` and
`ReleaseActiveViewGPU`. `Init` and `Shutdown` call them for the main view, `CreateView` and
`DestroyView` for every other one, so an object added there exists in every view.

## 3. A physical frame is one step of the world

`Renderer3D::BeginFrame` / `EndFrame` bracket one physical frame. Every `BeginScene` / `EndScene` pair
inside renders one view of that frame, and a view renders at most once in it. The first view begun
is the frame's **primary view**. A `BeginScene` outside a bracket is a frame of its own, as before.

| Advances once per physical frame, in its primary view | Advances once per view frame |
|---|---|
| object pose history (`Prev`/`Curr` transforms) | the camera history (`EndScene` rotates current to previous) |
| the GPU Scene extraction and commit, the deformed, vegetation and groom surface caches, the path tracer's tables | the view's jitter, upscale phase, cloud and fog reprojection indices |
| the acceleration-structure build and the skeletal deform dispatch | the view's history registry, Hi-Z pyramid, light grid, shadow maps |
| the stochastic frame index | eye adaptation, by the time since this view last rendered |
| the render origin, which every view of the frame renders about | the cascades, the cloud shadow map and the volumetric shadow cascades, refitted to the view |
| wind, snow accumulation and ejecta, wake and foam, spray, precipitation, cloud and fog time | the GPU pass timers and the profiler frame |
| the scene's animation clock, the foliage interaction field, the streaming upload budget | |
| foliage: the placement rebuild, its publication, and which plants hold streamed detail | |
| `FrameResourceManager` (allocators, deferred deletions) and the descriptor heap's transient ring | |

A later view **links** its draws to the records the frame committed
(`Renderer3D::ExtractGPUSceneMesh` builds the same instance key and stages nothing) and `EndScene`
resolves the links. It extracts nothing a second time.

`Scene::RequestAdditionalView` is the scene-side entry: the scene renders the requested view in the
same bracket as its main view, before or after it.

## 4. Lifetime rules

| Operation | What it touches |
|---|---|
| `CreateView(desc)` | a new slot and a new id, never reused in this renderer run. Settings start as a copy of the creating view's; sequences start where the run's seed starts every view's |
| `OnWindowResize` under a `ScopedView` | that view's graph and light grid. Its histories restart; no other view's object is recreated |
| `ResetFrameCamera` under a `ScopedView` (a cut) | that view's `RendererViewState` and histories |
| `DestroyView` | that view's graph, passes, histories, pyramid, light grid, shadow maps and query pool. Refused for the main view, the active view and between `BeginScene` and `EndScene` |
| `ResetSceneState`, `ResetGPUScene`, a GPU Scene record change | scene state, and the histories of **every** view |
| `ResetFrameSequences` | the frame clock and every view's `ViewClock`, pass sequences and histories |
| `Shutdown` | every view but the main one through the `DestroyView` teardown, then the main view's through the same one |

`Renderer3D::InvalidateTemporalHistories` reaches every view for a cause that happened to the world
(`SceneReset`, `SceneMutated`, `SamplingSequenceReset`, `BackendChanged`) and the active view for the
rest. A `Manual` reset is the active view's, because a camera cut is. The world's own discontinuities
that no history can reproject across (a streaming tier switched, foliage was regenerated, a coat
changed representation) go through `Renderer3D::InvalidateWorldTemporalHistories`, which performs the
same reset in every view, the temporal upscaler's private history included: the world notices such a
change while one view happens to be active, and it happened to all of them.

## 5. Ray tracing

Supported, on one structure per frame. The frame's primary view builds the acceleration structures
about the frame's render origin; a later view's ray-traced passes trace that build with their own
histories. The frame's extraction gathers the ray-tracing-only producers (vegetation and groom
proxies, the path tracer's tables) for what **any** live view asks for, so a view that traces is
served whichever view happened to render first.

## 6. Limits, stated

- **One render origin per frame, the primary view's.** A view far from it renders with the float
  precision of that distance. Two views that each need their own origin are two frames.
- **Camera-following simulation follows the primary view.** The wind field, the snow clipmap, the
  wake and foam lattice, spray and the precipitation volume are centred on the primary view's camera
  and stepped there. A later view sees them where its frustum overlaps them.
- **Foliage detail residency follows the primary view.** Which plants hold streamed mesh detail is
  decided once per frame from the primary view's camera. A later view looking elsewhere draws the
  stand-ins that camera left there. Culling, density thinning and fading are each view's own.
- **Views per frame are bounded by the transient descriptor ring where the backend runs behind the
  CPU.** On Vulkan the views of a bracketed frame allocate from one sub-ring
  (`kDescriptorHeapTransientSlots`, 1024), because the ring may advance once per frame. A view past
  the limit binds the slot-based way; the heap counts it (`Stats::TransientOverflowsHeld`) and reports
  it once per session. OpenGL's single ring holds nothing: each view has all of it.
- **Views without a bracket are separate frames.** Two `BeginScene` calls with no `BeginFrame` advance
  the world twice, exactly as before this change.
- **One scene per bracket.** A second scene's view in a frame another scene extracted is refused its
  draw links, and says so once.
- **Accumulations a pass owns are per view.** DDGI's camera cascades and the virtual shadow map's page
  cache converge once per view. That is why the reflection and light probe bakers still render
  through the main view, as before: a bake view created for the bake would capture unconverged
  lighting. They activate the main view whichever view their caller had active, resize and re-pose
  it for the bake's duration and put its size back.
- **Other alternate-camera renders are not scene views.** Planar reflection renders inside its own
  view's frame and mirrors that view's camera. Asset thumbnails (`AssetPreviewRenderer`) and the sky
  and IBL cubemap bakes draw with their own shaders and framebuffers and never go through
  `Renderer3D`'s views. The engine has no portal, minimap or camera-to-texture component.
- **Shared budgets.** Virtual-geometry residency feedback and the representation streaming budget are
  one per frame; views compete for them.
- **A scene's render settings configure the active view.** `SceneTransition::ApplySceneRenderSettings`
  writes the main view's path and post-process stack; another view keeps what its owner gave it. The
  world's settings (fog, snow, wind, precipitation, skin diffusion) are shared by every view.
- **No editor surface.** Nothing in the editor creates a second view. The API is
  `Renderer3D::CreateView` and `Scene::RequestAdditionalView`.

## 7. Tests that pin it

| Property | Test |
|---|---|
| each of two views renders what a single view renders from its camera, both orders, three paths, both backends | `AllPaths/MultiViewRendering.EachOfTwoViewsRendersWhatASingleViewRendersFromItsCamera` |
| a temporally resolved, upscaled view and a native view keep their own jitter and histories | `AllPaths/MultiViewRendering.ATemporallyResolvedViewAndANativeViewKeepTheirOwnHistories` |
| path and sample count differ per view | `MultiViewConfigurations.PathAndSampleCountDifferPerView` |
| eye adaptation is per view and steps by the view's own time | `MultiViewConfigurations.EachViewAdaptsItsExposureToWhatItSees` |
| resize, cut or destroy one view: the other keeps its histories, pyramid, shadow maps and exposure | `MultiViewLifetime.ResizingCuttingOrDestroyingOneViewLeavesTheOtherIntact`, `MultiViewLifetime.ResizingOrCuttingTheMainViewLeavesTheSecondIntact` |
| the scene advances once per physical frame | `MultiViewSceneAdvance.TwoViewsAdvanceTheSceneOncePerPhysicalFrame`, `...AMovingObjectBlursTheSameInBothViews` |
| histories are keyed by view | `MultiViewSceneAdvance.AViewsHistoriesAreKeyedByItsOwnId`, `TemporalHistoryRegistryViewBinding.*` |
| a world invalidation reaches every view, a view's own only itself | `MultiViewSceneAdvance.AWorldInvalidationReachesEveryViewAndAViewsOwnOnlyItself` |
| each view draws the meadow a single view draws from its camera, and the meadow advances once per frame | `MultiViewFoliage.EachViewDrawsTheMeadowASingleViewDrawsFromItsCamera` compares exact frames with stable GPU compaction (#1695) |
| a planar reflection mirrors its own view's camera | `MultiViewAlternateCamera.EachViewReflectsFromItsOwnCamera` |
| a probe bake renders through the main view whichever view is active | `MultiViewBake.ABakeStartedUnderAnotherViewRendersThroughTheMainView` (OpenGL) |
| every view traces the frame's one acceleration structure | `MultiViewRayTracing.BothViewsTraceTheFramesAccelerationStructure` (ray-tracing device only) |
| the transient descriptor ring advances once per bracketed frame | `HeapFixture.AHeldFrameResetKeepsTheFramesViewsAndAdvancesTheRingOnce` |
| the views of a held frame share one sub-ring, and running out is counted as the frame's view limit | `HeapFixture.AnOverflowUnderAHoldIsCountedAsTheFramesViewLimit`, `MultiViewCost.ASecondViewCostsItsOwnStorageAndLeavesNothingBehind` |
| a single-ring heap defers nothing under a hold | `HeapFixture.AHoldOnASingleSubRingDefersNothing` |
| the main view beside a second view, over generated sequences | state-machine pair `isolated-vs-shared-view` |
| the comparison catches a shared camera history and a per-view world advance | `MultiViewNegativeControl.*`, `RendererStateMachineNegativeControl.SharedViewCameraHistoryIsCaught` |
| no parked slot keeps a GPU object past shutdown | `Renderer3D::DebugLiveGpuOwningStatics` (`ExtraSceneViews`, `ActiveViewParkedState`), `RendererShutdownTest` |
