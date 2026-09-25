# A Forward/Deferred diff measures everything the two paths do differently

Issue #1457. Tests: `PointLightEvaluatorParityGpuTest` (L2, the two evaluators on one light and
surface), `DeferredPointLightParityEvidenceTest` (the real pipeline on all three paths).

## The rules

1. **Before blaming a shading function for a path difference, turn off everything the paths apply
   differently, then A/B one thing at a time.** In the live editor that means:
   - **screen-space AO**: every path applies it to the ambient term since #1452, but Forward and
     Forward+ give foliage none until it joins the forward prepass (#1474);
   - **bloom**;
   - **editor overlays**: the grid, gizmos, probe-volume lines.

   A difference that survives all of that is a shading difference. Measure a same-config control
   first ([live-verification-noise-floor.md](live-verification-noise-floor.md)).
2. **Split the question into evaluator and frame.** An L2 probe runs both evaluators on one light and
   surface, packed from one canonical record through the production adapters. A frame test then
   covers what the probe cannot reach: the cluster lookup, the reconstructed position, the target
   sizes.
3. **A per-pixel grid is sized to the target it shades, every frame.** Anything indexed by
   `gl_FragCoord` against a stored extent breaks the first time the render resolution differs from
   the window, which is any upscale.
4. **A write mask names attachments by number, and the numbers mean different things per path.**
   Forward's attachment 0 is colour; Deferred's is G-Buffer albedo.
5. **Deferred's floor is not zero.** It shades a position reconstructed from depth. That is
   sub-millimetre off, and it flips a pixel only at a silhouette or a shadow boundary (a PCF tap
   changes side). Bound the count (0.02 % of the frame); do not raise the per-pixel tolerance.

## What #1457 found

- **The reported gap was AO, not the tile evaluator.** DDGITest's red point light differed by up to
  22 levels live. With GTAO off, every probe agreed within 1 level on GL and Vulkan. With GTAO on
  and the light off, the paths were identical. Deferred was the brighter one on every differing
  pixel, as it must be when Forward multiplies direct light by AO. The evaluators agree to 1e-5.
- **The Forward+ light grid was sized to the window.** `Renderer3D::OnWindowResize` set its extent;
  under an upscale the scene renders at 0.667x, and fragments looked their tile up in a grid 1.5x
  too large. Lights vanished in screen-sized rectangles on Forward+ and Deferred (242k pixels of
  696k). `SceneRenderPass` now resizes the grid to the shaded target before culling.
- **Debug lines and joints wrote only G-Buffer albedo on Deferred** (`colorAttachmentWriteMask =
  0x01`). The pixel kept the sky's emissive and unlit flag, so every skeleton, joint and camera
  gizmo line drew as the background. Live Forward-vs-Deferred went from 28.7k differing pixels to 35.
  Since #1472 such draws go to `ForwardOverlayPass` on Deferred instead (over sky a depth-off line
  writes no depth, so the G-Buffer route still lost it); the G-Buffer surface-lane mask remains for
  the no-overlay fallback, chosen inside `DrawMesh` by where the draw lands.
- **GTAO darkens an unoccluded flat plane by view angle**: 1.0 from above, 0.71 at 45 degrees, 0.34
  grazing, on both paths. It is filed as #1463; it is why AO read about 0.5 on DDGITest's open
  floor.
