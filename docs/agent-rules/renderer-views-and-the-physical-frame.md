# New renderer state belongs to a view or to the world, and the world advances once per frame

Applies to: anything added to `Renderer3D`, a render pass, `Scene`'s render path, or a per-frame
renderer singleton (issue #1352, [ADR 0026](../adr/0026-a-scene-view-is-one-struct-and-a-frame-is-one-step-of-the-world.md)).

The renderer can draw several scene views in one physical frame. Each view owns a whole render stack
(`RendererViewResident`); the frame's first view, its **primary view**, advances what the world owns.
A change that ignores this passes every single-view test and breaks the second view.

## Rules

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
   the parked slot. [Story](#a-copy-in-a-parked-slot-outlived-shutdown).
7. **A reset says whose it is.** A camera cut, a resize and a settings change are the active view's.
   A scene change, a GPU Scene mutation and a sequence restart reach every view: use
   `ForEachViewResident` (touches only the view's own objects) or `ForEachView` (activates each view,
   for code that goes through `Renderer3D` accessors). World code that drops histories because the
   world's content changed (a streaming tier, a regenerated meadow) calls
   `Renderer3D::InvalidateWorldTemporalHistories`, never `InvalidateTemporalHistories(Manual)`: the
   second reaches only the view that happened to be active.
8. **Activate with `Renderer3D::ScopedView`, never between `BeginScene` and `EndScene`.** A test that
   creates a view destroys it in `TearDown` before the fixture restores settings: the restore is the
   main view's.
9. **Compare a view with the single-view renderer, and settle the configuration first.** The
   reference for a view is the main view alone, posed, sized and configured like it, the same number
   of frames after `Renderer3D::ResetFrameSequences`. Render a few frames between configuring and
   restarting. [Story](#a-reference-measured-inside-a-settings-transition).
10. **A pair that compares frames needs the lane the fault reaches.**
    [Story](#the-negative-control-that-was-missed).
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
    returned. [Story](#a-bake-that-read-its-own-clear-colour).

## Stories

### A copy in a parked slot outlived shutdown

`HZBGenerator` declared a defaulted destructor and no move operations, so moving a view's state
copied its three `Ref`s. After the first activation of a second view the main view's slot held a
second reference to the main view's pyramid, shader and parameter block; `Renderer3D::Shutdown`
released the copy in `s_Data` and the slot's kept them. Every OpenGL test passed. The Vulkan process
passed all 14 tests and then exited with code 3: one texture and one shader survived teardown and VMA
reported 351,232 bytes alive. The generator is move-only now, the slot of the view that became
active is emptied by assignment, and `DebugLiveGpuOwningStatics` reports a parked slot that owns
anything.

### A reference measured inside a settings transition

`ATemporallyResolvedViewAndANativeViewKeepTheirOwnHistories` failed for one arrangement only: 973
pixels, in the second view, in whichever order ran second. It looked like a history crossing views.
It reproduced with one view and no second view anywhere: after a size or upscale change the first
frames differ from the same frames rendered after a bare `ResetFrameSequences`, because the scene
band is resized when the graph next compiles and the restart does not undo that. The reference ran
straight after a resize; the second run of the same view did not. Both arms now render
`kConfigureSettleFrames` before the restart. What settled it was a scratch test that removed the
second view, not a hypothesis about it.

### A bake that read its own clear colour

`LightProbeBaker` bound a framebuffer, called `Scene::RenderScene3D` six times and read the
framebuffer back. The scene renders into the render graph's targets, so every face was the clear
colour, the probe's energy was zero and `BakeProbeAtPosition` reported every probe invalid: the
editor's raster "Bake Light Probes" stored nothing. `ReflectionProbeBaker` had the same defect fixed
long before; ADR 0007 recorded the light baker's as "probably capturing black" and nothing ran it.
The one test that called the raster bake discarded its result. A multi-view test that asked the bake
whether it was valid found it, with one view. Both bakers now share `ProbeSceneCapture`, and
`LightProbeRasterBakeTest` checks which side of the probe is red.

### The negative control that was missed

The state-machine pair `isolated-vs-shared-view` passed 214 comparisons, and its negative control
(`FaultShareViewCameraHistory`: one camera history for every view) passed too. The harness renders
with no temporal effect, so a wrong previous view-projection changed nothing it captured: every
tracked target is rewritten from the current camera each frame. The fault reaches exactly one lane in
such a frame, the motion vectors, and the harness did not capture them. It does now
(`ResourceNames::Velocity`), and the control is caught. Before trusting a comparison, ask which
target the fault you fear would move.
