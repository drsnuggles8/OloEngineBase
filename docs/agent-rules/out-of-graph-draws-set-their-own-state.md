# A draw outside the render graph sets the state it draws with and puts the caller's back

Applies to every immediate draw that is not a render-graph pass: the sky bakes
(`SkyBake::RenderSkyToCubemap`), the IBL convolutions (`IBLPrecompute`), the impostor and
reflection-probe captures, the asset preview. A pass inside the graph sets its own state every
frame and is not the subject here.

## Rules

1. **Set culling, the depth test and blending yourself.** Never draw under whatever the previous
   draw of the process left. A draw that covers its whole target with one cube seen from the
   inside, or one fullscreen triangle, takes a `ScopedFullCoverDrawState`
   (`Renderer/CaptureStateGuard.h`): culling off, depth test off, blend off.
2. **Hand back the state you found, not a default.** `RenderCommand::IsCullingEnabled()`,
   `IsDepthTestEnabled()` and `IsBlendEnabled()` answer on both backends. `CaptureStateGuard`
   restores the cull switch it was constructed under. `IsBlendEnabled()` answers for the global
   flag `SetBlendState` writes, never for one attachment's opinion: on OpenGL
   `glIsEnabled(GL_BLEND)` reports "some draw buffer is on", so the backend reads draw buffer 0
   by index, or its own record while a per-attachment opinion stands
   (`GLAttachmentBlendOpinion.TheGlobalFlagIsReportedUnderAnOpinionAndAfterARawFlip`).
3. **Test the draw under more than one caller state** and compare the results:
   `ProceduralSkyBakeTest.TheBakeIsTheSameUnderWhateverStateItIsCalledWith` bakes under back-face
   and front-face culling, a depth test with no write, a `Greater` depth func and additive
   blending, and expects the same four textures each time, on OpenGL and in the Vulkan test
   process. Set the caller's state and issue the draw inside one `RunInFrame`: Vulkan drops work
   recorded outside a frame, and on OpenGL the fixture puts the context back when the frame ends.
4. **In a test process a leaked switch lasts until the process ends.** `RendererAttachedTest`
   puts every frame back to the state it started in, so the state one test leaves is the state
   every later frame starts with. When a GPU test's number depends on what ran before it, print
   `GLStateSnapshot::Capture()` at the start of the test, once alone and once after the
   predecessor, and compare the two before reading any pass.

## What happened (#1352)

`Breeds/BreedShowcaseEvidenceTest.TheCardHandOverStepsTheCoatWithinTheNearLaddersBar/Samoyed`
passed alone and in a full run, and failed when a name filter left out the suites between it and
`ImpostorBakeEvidenceTest.ConeBakesViewDependentOctahedralAtlas`: the cards' energy against their
strands went from 0.956 to 0.948 at 21 m, over a 5% bar. A binary without the branch did the same,
to fifteen digits.

Bisecting the 216 earlier tests of the filter left that one 17 ms test. The state dump named the
difference: back-face culling off when the dog test started alone, on after the impostor bake.
`CaptureStateGuard` ended every bake with `EnableCulling()`, whatever it had found.

The consumer was not a coat pass. Forcing culling off at the start of the test restored the
numbers, so the state was read every frame, and the test's own split by lobe pointed at the sky:
with the leak, the default arm read exactly what the "no sky" arm reads. The scene's procedural
sky re-bakes when the test changes it, and the bake drew its cube under the inherited switch.
Measured on a 64-texel Preetham sky (sum of the SH DC term's three channels):

| caller state | sky cube | irradiance | prefilter | BRDF table (sum) |
|---|---:|---:|---:|---:|
| culling off, depth test off, blend off | 4.154 | 4.154 | 4.154 | 195,834 |
| back-face culling | 1.977 | 1.253 | 1.977 | 195,834 |
| front-face culling | 2.177 | 1.415 | 2.176 | 0 |
| depth test on, func `Greater` | 0 | 0 | 0 | 0 |

The skybox cube's faces do not all wind the same way seen from inside, so either cull mode drops
some of them, and the irradiance convolution of a half-drawn cube is drawn half again. Vulkan had
the same fault with the two cull modes exchanged (back-face culling 2.176 / 1.415 / 2.170 and an
empty BRDF table, front-face 1.977 / 1.253 / 1.971).

A live editor did not show it because a frame there starts in the state the previous frame ended
in, which is the same every frame. The test process showed it because its frames are restored to
their starting state (rule 4), and because the coat test measures a ratio near its bar.
