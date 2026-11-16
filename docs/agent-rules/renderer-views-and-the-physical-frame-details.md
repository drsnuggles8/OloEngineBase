# Scene views and the physical frame: the stories

The failures behind the rules in
[renderer-views-and-the-physical-frame.md](renderer-views-and-the-physical-frame.md) (issue #1352,
[ADR 0026](../adr/0026-a-scene-view-is-one-struct-and-a-frame-is-one-step-of-the-world.md)). Each
section is linked from the rule it taught.

## A copy in a parked slot outlived shutdown

`HZBGenerator` declared a defaulted destructor and no move operations, so moving a view's state
copied its three `Ref`s. After the first activation of a second view the main view's slot held a
second reference to the main view's pyramid, shader and parameter block; `Renderer3D::Shutdown`
released the copy in `s_Data` and the slot's kept them. Every OpenGL test passed. The Vulkan process
passed all 14 tests and then exited with code 3: one texture and one shader survived teardown and VMA
reported 351,232 bytes alive. The generator is move-only now, the slot of the view that became
active is emptied by assignment, and `DebugLiveGpuOwningStatics` reports a parked slot that owns
anything.

## A reference measured inside a settings transition

`ATemporallyResolvedViewAndANativeViewKeepTheirOwnHistories` failed for one arrangement only: 973
pixels, in the second view, in whichever order ran second. It looked like a history crossing views.
It reproduced with one view and no second view anywhere: after a size or upscale change the first
frames differ from the same frames rendered after a bare `ResetFrameSequences`, because the scene
band is resized when the graph next compiles and the restart does not undo that. The reference ran
straight after a resize; the second run of the same view did not. Both arms now render
`kConfigureSettleFrames` before the restart. What settled it was a scratch test that removed the
second view, not a hypothesis about it.

## A bake that read its own clear colour

`LightProbeBaker` bound a framebuffer, called `Scene::RenderScene3D` six times and read the
framebuffer back. The scene renders into the render graph's targets, so every face was the clear
colour, the probe's energy was zero and `BakeProbeAtPosition` reported every probe invalid: the
editor's raster "Bake Light Probes" stored nothing. `ReflectionProbeBaker` had the same defect fixed
long before; ADR 0007 recorded the light baker's as "probably capturing black" and nothing ran it.
The one test that called the raster bake discarded its result. A multi-view test that asked the bake
whether it was valid found it, with one view. Both bakers now share `ProbeSceneCapture`, and
`LightProbeRasterBakeTest` checks which side of the probe is red.

Review of that fix found the next one. A render target stores its rows bottom-up on OpenGL and
top-down on Vulkan, the face orientations are chosen for the first, and both bakers consume a face
by position. The capture handed a Vulkan face over as it was read back, so every side face was
stored upside down. The red and blue walls of the test are left and right of the probe and could
not see it. `ProbeSceneCapture` turns the rows over, and the test asks whether the lit floor is
below the probe, in both test processes.

## The negative control that was missed

The state-machine pair `isolated-vs-shared-view` passed 214 comparisons, and its negative control
(`FaultShareViewCameraHistory`: one camera history for every view) passed too. The harness renders
with no temporal effect, so a wrong previous view-projection changed nothing it captured: every
tracked target is rewritten from the current camera each frame. The fault reaches exactly one lane in
such a frame, the motion vectors, and the harness did not capture them. It does now
(`ResourceNames::Velocity`), and the control is caught. Before trusting a comparison, ask which
target the fault you fear would move.

## A coat with no motion behind it in the later view

`Scene::PublishGroomStrandRequests` runs once per view. It already held the solver to one step per
frame (the groom clock is zeroed after the first publish) and the LOD state to one advance. Three
things still advanced per publish: the binding's previous body-to-coat matrix, its verdict on whether
last frame is comparable, and the rotation of the simulation's displacement pair. A view rendered
after another one therefore drew a coat whose previous frame was the frame itself. The coat stood in
the right place and its motion vectors said it had not moved, so the later view's temporal resolve
and motion blur accumulated a different image: 44,908 pixels of a 1280 x 720 main view, against the
same view alone, when a second view was rendered ahead of it. A reviewer found the matrix by reading;
the verdict and the pair were found by asking what else in that function turns this frame into the
last one.

The first two versions of the test passed against the unfixed code. The fixture renders without a
temporal resolve, so nothing read the coat's previous frame; and its runtime camera stands sixty
units back so that its gizmo stays out of the editor captures, where the coat is a few pixels. The
test now switches the resolve and motion blur on, moves the camera in, and first requires a simulated
run to differ from an unsimulated one by more than 2,000 pixels.

The history is keyed by `Scene::m_GroomFrameSerial`, which `BeginGroomFrame` advances at every frame
entry point: a coat's first deformation under a serial decides and rotates, a later one reuses. The
solver is told about a discontinuity by the first deformation only; told again by a later view it
would re-seed the particles the first view had just stepped.

## A bake under an upscaler filled a corner of each face

`ProbeSceneCapture::CaptureFaces` set render scale 1 for its faces and read the `SceneColor` target
back as `resolution` x `resolution` texels. With an upscale preset on, the pipeline sizes the scene
band itself, every frame, to `floor(size * UpscaleModeToRenderScale(preset))`, whatever
`Renderer3D::SetRenderScale` says. A 128-texel face was an 85-texel target, read into the start of
a 128 x 128 buffer; the rest of the buffer was whatever the previous face left. A reflection probe
baked in the editor with FSR on reflected a small, sheared copy of the room.

No test baked under an upscaler. `ProbeBakeVisualEvidenceTest.DeferredUpscaledShowsBothBakesTheRightWayUp`
does, and its first picture showed it: the metal sphere's lower band read (11.7, 42.0, 13.5) where
the other four configurations read (64.7, 180.7, 66.2). The capture now turns the preset off for
its duration (`ScopedCaptureSettings`) and fails, with the two sizes in the message, when the target
it is about to read is not the size of a face.
