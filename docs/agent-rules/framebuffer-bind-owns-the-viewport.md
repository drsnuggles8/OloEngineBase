# Framebuffer::Bind() owns the viewport, on every backend

**Rule.** Binding a framebuffer sets the viewport to that target's *active* size,
`GetActiveViewportWidth/Height()`: the render-viewport override when one is set, the
specification otherwise. A backend's `Bind()` must do this, and a pass that sets its own
viewport after binding must take it from the same two getters, never from `GetSpecification()`.

## Why

The OpenGL `Bind()` always did it (`glViewport` to the target). The Vulkan `Bind()` only
published the target, so any pass that relied on `Bind()` inherited the **last viewport
recorded**, which is whatever the previous pass drew at.

That stayed invisible while every target in a frame was the same size. It broke with the FSR1/FSR2
scene band (#1397): the post chain runs at display size, the next frame's scene pass binds the
reduced band, and on Vulkan the scene drew at 1024×683 into a 705×471 target. The upscaler then
presented the band's corner, magnified by `1 / renderScale`. At Performance the subjects of
`ReferenceHead.olo` were cropped out of frame entirely.

The groom pass sets its viewport explicitly, so it drew at the band's true size. That was right
for the target, but it disagreed with the magnified scene, and every coat floated beside its body
(#1430). The pass that was *correct* looked like the broken one.

The groom pass did have its own bug, but a different one: it used the spec, not the active
viewport, so under a DRS render scale (`Renderer3D::SetRenderScale`) it drew its coat at full size
over a body drawn into the sub-rectangle. It was off by 5 body radii in
`UpscaleFramingEvidenceTest.GroomCoatFollowsTheBoundRenderViewport` before the fix.

## How to check

- `VulkanDrawPath.FramebufferBindResetsTheViewportToItsTarget` records a stale display-sized
  viewport, binds a smaller target and draws a left-half quad. It must cover exactly half.
- `UpscaleFramingEvidenceTest` measures three subjects' centroids and areas at native, Quality
  and Performance on every GL path, with both upscalers. A negative control makes sure it rejects
  the #1397 crop.
- Vulkan is live-only. The quickest check is `olo_renderer_settings_set upscale performance` and a
  screenshot: framing must match native, and `SceneColor` must report the reduced size.

The fullscreen composite passes (OIT resolve, deferred lighting's debug overlay, selection outline)
still use the spec. That is correct for the upscale band, where spec == band, but not under a DRS
render scale. Fixing them needs the UV-bounds handling that DRS itself has not finished.
