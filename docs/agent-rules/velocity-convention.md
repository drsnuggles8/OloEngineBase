# The screen-space velocity convention

Rules for anything that writes or reads the engine's motion vectors (SceneColor RT3 and the G-Buffer's
RT3, `FrameBlackboard::Scene::SceneVelocity` / `GBuffer::Velocity`). Issue #1552.

## The rules

1. **Velocity is unjittered.** Both view-projections carry their own frame's TAA jitter, and a jittered
   projection moves every vertex by one constant screen offset. So a plain difference of two jittered NDC
   positions moves by the jitter's change where nothing moved at all. Every writer takes it out:
   - a stage with the camera block calls `oloVelocityFromNdc(ndcCurr, ndcPrev)` (include/ScreenVelocity.glsl,
     which CameraCommon.glsl includes);
   - a vertex stage feeding a fragment stage that has no camera block passes its clip positions through
     `oloUnjitterClip(clip, u_JitterUV.xy / .zw)`. The fragment stage then keeps the plain
     `(ndcCurr - ndcPrev) * 0.5`, and says so in a comment;
   - a camera-velocity reconstruction from depth (TAA's and motion blur's fallback) subtracts the motion-blur
     block's `u_MotionJitterUV` the same way.

   A still camera over still content writes exactly zero
   (`RenderTargetExportEvidence.StillContentWritesNoVelocityWhileTheJitterMoves`).

2. **The offsets come from the uploaded projections and rotate with the previous VP.** `CurrJitterUV` is
   `TemporalUpscalePolicy::ProjectionJitterVelocityOffset(unjittered, jittered)` on the
   `RHI::AdjustProjectionForBackend` pair. It carries the backend's y flip and the image's sign: a
   perspective jitter in P[2][x] moves the image by minus it, an orthographic one in P[3][x] by plus it.
   `PrevJitterUV` rotates beside `PrevViewProjectionMatrix` at the end of the frame. Both reach the camera
   block (`CameraUBO::JitterUV`) and binding 8 (`MotionBlurUBOData::JitterUV`); every other view leaves zero.

3. **A hand-written camera block carries the full layout through `u_JitterUV`.** Rename a clashing
   member (`_cameraPrevViewProjection` where binding 8 already declares `u_PrevViewProjection`), but keep
   every offset. The stages of one shader must declare identical layouts: the engine's SPIR-V link
   validation rejects a mismatch, so extending one stage's block means extending them all.

4. **The resolves read `SceneVelocity`, every writer's.** That covers TAA, motion blur, FSR2 and the
   depth/velocity upscale. On the forward paths it is the same handle as `GBuffer.Velocity`. On Deferred,
   `SceneVelocitySeedPass` copies the G-Buffer's RT3 into SceneColor RT3 before the forward overlay, so the
   groom, foliage, particles and water drawn over the lit frame write their own motion over it. The passes
   that run before those (SSR, SSGI, ReSTIR, RT shadows, the deferred lighting) read `GBuffer.Velocity`
   (`RenderTargetExportEvidence.DeferredTemporalResolveSamplesTheSeededSceneVelocity`).

5. **FSR2 is told the vectors carry no jitter** (`TemporalUpscalerConfig::MotionVectorsIncludeJitter =
   false`). Told they do, it cancels a jitter that is not there: a pixel of reprojection error every frame.

## The failure that taught it

Before #1552 every velocity carried the jitter's change: 0.00073 UV, 1.4 px at 1080p, on a static
surface at a static camera. TAA reprojected its history by it, which resampled the history every frame.
A stochastic groom coat kept 3x of its shimmer reduction where it had had 11x (#1332 exposed it by
letting Forward TAA read the coat's own velocity). A/B on the shader showed the velocity forced to zero
restored 11x, and subtracting the jitter in TAA alone made Deferred worse, because cleared and background
pixels carry zero, not the delta. The convention has to hold at the writers. Separately, Deferred TAA read
the G-Buffer's velocity, which the groom never writes, so a walking coat on Deferred had no motion in its
resolve at all.
