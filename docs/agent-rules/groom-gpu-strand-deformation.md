# Deforming a bound coat on the GPU (#1427)

Read before touching `Groom/GroomGpuDeformation.{h,cpp}`, `BuildGroomStrandRestMesh`,
`GroomRenderPass::{AcquireGpuDeformedGeometry, UploadDeformation, AcquireDrawnPose}`,
`include/GroomStrandDeform.glsl`, or the `u_GroomDeformModes` branch of `GroomStrand.glsl`. The
deformation itself (root frames, previous poses, invalidation) is
[groom-surface-binding.md](groom-surface-binding.md)'s; this file is about where it runs.

## The rules

1. **Build a bound coat's stream once, in each root's bind frame, and move it in the vertex
   shader.** `BuildGroomStrandRestMesh` writes every point as
   `conjugate(RestRotation) * (rest - RestOrigin)`, which is the half of `ApplyGroomRootTransform`
   that never changes. Per frame the pass uploads one `GroomDeformRootRecord` per drawn strand
   (origin and rotation, this frame and last) plus the guide slots and displacement samples; the
   shader evaluates `Origin + Rotation * local` and adds the guide displacement. The stream is then
   cached and immutable like an unbound groom's.

2. **One walk for both builds.** The rest stream and the CPU-deformed stream come out of
   `WalkStrandSegments` in `GroomStrandMesh.cpp`: the same selection, coat shape, per-role strides
   and segment budget. A second copy of that walk would drift, and the first symptom would be one
   stiff hair in a moving coat.

3. **Keep the CPU path as the reference, behind a lever, and hold the two to EXACT agreement.**
   `RendererSettings::GroomGpuDeformation` (MCP `groomdeformation`, the renderer panel checkbox)
   selects the pre-#1427 rebuild. `GroomGpuDeformationTest` reconstructs every corner the way the
   shader does, through `EvaluateGroomDeformedPoint` over the same packed bytes, and compares it
   with `BuildGroomStrandMesh` **bit for bit**. That is possible because the arithmetic is the same
   arithmetic in the same order. The one exception is a root held at rest: the GPU path carries it
   through its bind frame and back, so that case gets a 1e-6 tolerance and says why. The GL render
   A/B is `GroomBindingVisualEvidenceTest.TheGpuDeformedCoatIsTheCpuDeformedCoat`.

4. **The same sixteen floats, two meanings, and the mode lane decides.** The rest stream reuses
   `GroomStrandVertex` so the GL attribute block and the Vulkan pull stride do not change:
   `Position` and `Other` are the two endpoints (bind-local) and `PrevPosition` carries the root
   slot, the other endpoint's parameter and which end this corner is. `u_GroomDeformModes.x` is 0
   for every unbound groom and for the CPU path, so a static coat's stream and shader arithmetic
   are unchanged by construction. The cache key carries the path and the binding, so flipping the
   lever never serves one path the other's stream.

5. **Hold an integer in a float lane as a VALUE, never as bits.** The root slot is
   `static_cast<f32>(slot)`. A bit-cast small integer is a denormal, and a vertex fetch may flush it
   to zero, which would move every strand onto root 0. Integers in the storage buffer are read as
   `uvec4` and floats are reinterpreted with `uintBitsToFloat`, which is exact in that direction.

6. **The storage buffer rides binding 79 under the rebound-per-use rule.** The SSBO namespace is
   full (`ShaderBindingLayout.h`), so `SSBO_GROOM_DEFORMATION` is an alias of `SSBO_TERRAIN_VT`.
   The pass binds a buffer, the coat's own or a one-record placeholder, before EVERY strand draw.
   Terrain rebinds before each of its own uses, and `GroomStrand.glsl` declares nothing else at 79.
   A consumer that reads 79 without binding it first breaks the sharing.

7. **On Vulkan, write the frame buffer as a recorded transfer, not a frame-arena snapshot.**
   `StorageBufferUsage::StreamCommandOrdered` routes `SetData` through `UploadBufferSubData`: a
   staged `vkCmdCopyBuffer` in the frame's command stream. A snapshot of a multi-megabyte
   per-frame payload for every coat would overflow the 16 MiB arena slot, and that overflow is
   how the coats went bald on Vulkan before #1446.

8. **The coat bake still needs the pose on the CPU, and gets it from the same bytes.**
   `AcquireDrawnPose` evaluates the drawn centrelines with the CPU twin (`EvaluateGroomDeformedPose`)
   from the frame buffer the GPU reads. Only a coat that asked for a self-shadow pays for it, and
   the rest stream's centrelines (`GroomRestPoseSegment`, 40 bytes a segment) are kept only for such
   a coat. The bake's own cost is #1445's.

9. **The ray-traced proxies do not read this stream.** `RayTracing::GroomSurfaceCache` builds its
   own CPU mesh from the request (#1253). Moving the raster deformation to the GPU neither breaks
   nor speeds it up.

## Measured

`GroomAnimalsAcceptanceEvidenceTest.CostScalesAcrossAHerd`, Release, RTX 4090, 1280x720, Forward,
TAA on, same box and session for both arms (sibling builds were running, which is why the "before"
GPU times are about 3x the issue's table; the ratios are the finding):

| herd, scheduler off | GroomPass GPU ms | wall ms/frame | CPU build ms | upload MiB/frame |
|---|---|---|---|---|
| 3 | 1023 -> 120 | 1106 -> 187 | 313 -> 6.5 | 375 -> 11 |
| 7 | 1261 -> 185 | 1471 -> 297 | 502 -> 11 | 595 -> 18 |
| 12 | 1077 -> 61 | 1274 -> 194 | 467 -> 10.5 | 459 -> 17 |

- **The upload was ~90% of the pass's GPU time.** With the old path's refill skipped (a scratch
  lever, drawing a stale pose), GroomPass fell from 1023 ms to 88 ms on the 3-animal herd. What
  remains after #1427 (61-185 ms) is that draw: the strand shading itself, not the deformation.
- **GPU and CPU paths render the same pixels**: 0 px differ on GL in all nine {Forward, Forward+,
  Deferred} x {front, oblique, side} cells and on Vulkan in the pass suite (second frame, i.e. a
  refill), against a 9 000-49 000 px negative control.
