# The strand depth prepass (#1533)

Read before touching the draw at the end of `GroomRenderPass::Execute`, the `u_GroomModeFrame.w`
lane, or the discard block at the top of `GroomStrand.glsl`'s fragment stage.

## The rules

1. **Draw every coat twice with the same program: depth first, shading at depth EQUAL.** The first
   draw has colour writes off and `u_GroomModeFrame.w = 1`. It runs the coverage test and writes
   depth, and returns before any shading. The second draw has depth writes off and the compare at
   `Equal`, so only the fragment that won the depth test is shaded. A dense coat overlaps itself
   tens of times per pixel, and before this every layer was shaded in full: the showcase dog's
   GroomPass fell from 24-54 ms to 3.6-6.5 ms at 1920x1080 with the look unchanged.

2. **The coverage decision must be a pure function of the fragment.** The second draw keeps
   exactly the fragments the first did only because the discard depends on nothing but the
   fragment's pixel, frame index, segment id and alpha. A coverage test that read anything the
   first draw changes (a counter, a random state, the depth buffer) would shade holes.

3. **Same program, and `invariant gl_Position`.** Equal depth between two draws is a promise the
   language makes for one program with the same inputs; the declaration makes it explicit rather
   than a property of the driver. Two programs, even textually identical ones, void it
   (DepthNormalPrepass.glsl says the same for the scene prepass).

4. **The prepass vertex stage may skip only what does not feed `gl_Position`.** It skips last
   frame's deformation (the velocity) and the rest-bake lookup
   ([groom-coat-rest-bake.md](groom-coat-rest-bake.md)). Anything that moves the vertex must run in
   both draws.

5. **Upload and bind the params UBO before each of the two draws.** The lane differs between them,
   and the Vulkan backend's UBOs are arena-versioned: binding without the fresh upload publishes the
   previous allocation (the pass's own comment on the draw explains the failure it caused).

6. **Leave the depth mask on and the compare at Less.** The shading draw ends with depth writes
   off; the next geometry pass that inherits that renders without depth, which reads as a sorting
   bug several passes away (see PreparedFullscreenPass).
