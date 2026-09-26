# A pass that unpacks screen uv into view space takes proj11 from the reconstruction projection

Applies to: every compute or fullscreen pass that turns a pixel's uv and depth back into a
view-space position with hand-built constants, the XeGTAO-style
`NDCToViewMul = (2 / proj00, 2 / proj11)` / `NDCToViewAdd = (-1 / proj00, -1 / proj11)` pair.
Today that is `GTAORenderPass` and `SphereProxyAORenderPass`.

## The rule

Build those constants from `RHI::AdjustProjectionForShaderReconstruction(projection)`, never from
the raw projection. On Vulkan the projection seam flips clip y, so memory row 0 (uv v = 0) is the
**top** of the view there and the bottom on OpenGL. The reconstruction projection carries exactly
that row flip, and it is identity on GL, so the change cannot move a GL frame. Depth linearisation
(`proj[2][2]`, `proj[3][2]`) is untouched by it.

If the pass then turns a **screen** direction into a **view** direction (GTAO's slice direction
`omega`), take the axis signs from the unpack: `vec3(omega * sign(u_NDCToViewMul), 0.0)`. Screen +y
is view −y on Vulkan.

This is the uv half of [capture-paths-take-the-projection-seam.md](capture-paths-take-the-projection-seam.md),
whose table names the same variant for "shader arithmetic against uv/depth". Constants built by
hand never pass through a matrix upload, so nothing in that guide's checklist catches them.

## Why nothing caught it: a symmetric algorithm hides a mirror

With the raw projection, every position a Vulkan pass reconstructs is mirrored about the
horizontal, while the view normals it reads are not. An algorithm that treats both sides of a
direction the same survives that. GTAO before #1463 seeded both horizons at −1 and measured them
against the normal, so a mirrored slice integrated to about the same number, and on master Vulkan
GTAO looked plausible. The port to XeGTAO's conventions pairs sample 0 with the positive side of the
slice. On Vulkan that pairing was now wrong on every slice with a vertical component: the issue's
45-degree floor read AO **0.216** live on Vulkan and 0.996 on GL. Sphere-proxy AO had the same
mirror on master, darkening each proxy's vertically mirrored screen position.

So a sign-sensitive rewrite of an old screen-space pass can surface a row-order bug that has been
there all along. If a fix works on GL and collapses on Vulkan, check the unpack before the new
maths.

## The diagnostic that found it

- Probe the raw AO target on both backends at the same camera (`olo_render_probe_pixel`,
  `olo_render_capture_target`). A frame-wide collapse on one backend only is a convention
  problem, not a sampling one.
- `GTAOMathTest` has a per-pixel CPU mirror of the slice loop with a top-down-rows camera. Its
  broken arm (omega verbatim) predicted 0.211 for the live 0.216, before any engine code changed.
  Reproduce a live number on the CPU first, then trust the fixed arm.

## Checklist for a new screen-space pass

- `grep -n "\[1\]\[1\]"` in the pass: every screen-to-view constant comes from the reconstruction
  projection.
- Any bounds built from those constants use `min`/`max`, not an assumed sign (SphereProxyAO's tile
  AABB already did).
- Verify on Vulkan with a probe of the raw target, not a screenshot of the composed frame.
