# New renderer state belongs to a view or to the world, and the world advances once per frame

Applies to: anything added to `Renderer3D`, a render pass, `Scene`'s render path, or a per-frame
renderer singleton (issue #1352, [ADR 0026](../adr/0026-a-scene-view-is-one-struct-and-a-frame-is-one-step-of-the-world.md)).

The renderer can draw several scene views in one physical frame. Each view owns a whole render stack
(`RendererViewResident`); the frame's first view, its **primary view**, advances what the world owns.
A change that ignores this passes every single-view test and breaks the second view.

## Rules

The stories behind the rules are in
[renderer-views-and-the-physical-frame-details.md](renderer-views-and-the-physical-frame-details.md).

1. **Decide the owner of every new piece of state before you write it.** Measured against a camera,
   accumulated per camera, or sized by a resolution: it is the view's. Put it in a pass member (each
   view has its own pass objects) or in `RendererViewResident`. One per world whatever looks at it:
   `RendererSceneState`, `RendererFrameClock` or a scene-level object.
2. **Advance world state only under `Renderer3D::IsPrimaryViewFrame()`.** That covers
   `PrepareFrame`, `UploadExecutionState`, `EndScene`, a pass's `Execute` and
   `Scene::ProcessScene3DSharedLogic`. A step that runs per view rotates "previous" onto this frame's
   value for every later view, and a `SampleMockableDt` baseline shared by two views hands the second
   a time step of zero. Uploads and binds stay per view: only the step is gated.
3. **A sequence that paces one view's accumulation goes in `RendererViewClock`,** not the frame
   clock: jitter, a reprojection phase, an adaptation time baseline. `ResetFrameSequences` restarts
   it on every view through `ResetActiveViewSequences`; add it there.
4. **A process-wide per-frame ring is view state.** A singleton that swaps read and write buffers per
   `BeginScene` (the occlusion query pool was one) hands each view the other's results. Keep one
   instance per view and select it in `Renderer3D::ActivateView`, or cycle it once per physical frame
   if it holds nothing a view measures (`FrameResourceManager`, the descriptor heap's transient ring).
5. **Whatever outside `s_Data` points at a view-owned object is re-pointed in `ActivateView`.** Today:
   the debug runtime's graph, the out-of-band ledger, the dispatcher's light grid, the occlusion
   singletons. A pointer left behind draws one view with another view's object.
6. **A member of `RendererViewResident` must move, and its moved-from state must own nothing.** Hold a
   type that cannot move by `std::unique_ptr`. A type whose "move" copies leaves a second owner in
   the parked slot. [Story](renderer-views-and-the-physical-frame-details.md#a-copy-in-a-parked-slot-outlived-shutdown).
7. **A reset says whose it is.** A camera cut, a resize and a settings change are the active view's.
   A scene change, a GPU Scene mutation and a sequence restart reach every view: use
   `ForEachViewResident` (touches only the view's own objects) or `ForEachView` (activates each view,
   for code that goes through `Renderer3D` accessors). World code that drops histories because the
   world's content changed (a streaming tier, a regenerated meadow) calls
   `Renderer3D::InvalidateWorldTemporalHistories`, never `InvalidateTemporalHistories(Manual)`: the
   second reaches only the view that happened to be active.
8. **Activate with `Renderer3D::ScopedView`, never between `BeginScene` and `EndScene`.** A test that
   creates a view destroys it in `TearDown` before the fixture restores settings: the restore is the
   main view's. A view an open scope returns to cannot be destroyed; `Renderer3D::CanDestroyView`
   says whether a view can, without asserting.
9. **Compare a view with the single-view renderer, and settle the configuration first.** The
   reference for a view is the main view alone, posed, sized and configured like it, the same number
   of frames after `Renderer3D::ResetFrameSequences`. Render a few frames between configuring and
   restarting. [Story](renderer-views-and-the-physical-frame-details.md#a-reference-measured-inside-a-settings-transition).
10. **A pair that compares frames needs the lane the fault reaches,** and the subject on screen.
    Run the comparison against the unfixed code first: a test that passes there measures nothing.
    [Story](renderer-views-and-the-physical-frame-details.md#the-negative-control-that-was-missed),
    [story](renderer-views-and-the-physical-frame-details.md#a-coat-with-no-motion-behind-it-in-the-later-view).
11. **A "log it once" latch in view code is a member, not a function-local `static`.** Two views of
    different configuration flip a shared latch every frame. Put it in `RendererViewReported`. The
    build-stats latch also gates the compiled-hazard validation, so a shared one validated every
    view frame.
12. **A view-owned GPU object is created in `InitializeActiveViewGPU` and released in
    `ReleaseActiveViewGPU`.** `Init`, `CreateView`, `Shutdown` and `DestroyView` all go through the
    pair; an object created in `Init` alone exists in the main view only.
13. **A capture of the scene from another camera goes through `ProbeSceneCapture::CaptureFaces`.**
    It renders through the main view and reads the graph's `SceneColor` back. `Scene::RenderScene3D`
    never writes into a framebuffer you bound, and a test that calls a baker must look at what it
    returned. Faces arrive with their rows bottom-up on both backends; code that reads a render
    target back itself asks `RHI::RenderTargetRowsAreBottomUp`.
    [Story](renderer-views-and-the-physical-frame-details.md#a-bake-that-read-its-own-clear-colour).
14. **Scene-side history that a per-view publish advances is advanced on its first publish of the
    frame.** `Scene::ProcessScene3DSharedLogic` and everything under it (the groom publish) run once
    per view. State that turns "this frame" into "last frame" there is keyed by a per-frame serial
    the frame entry points advance (`Scene::m_GroomFrameSerial`), and a later publish reuses what the
    first decided and read. `IsPrimaryViewFrame` is the wrong key for it: a probe capture's faces are
    each a frame of their own to the renderer.
    [Story](renderer-views-and-the-physical-frame-details.md#a-coat-with-no-motion-behind-it-in-the-later-view).
