# Every specular lobe takes the pixel filter

**Rule.** When a surface carries more than one GGX lobe (a base and a coat, such as an eye's tear film
or a lip's saliva layer), widen every lobe by the same screen-space normal spread. Filtering only the
base leaves the coat to point-sample a delta light's glint. On every path the kernel comes from the
place where the normal is built:

- **Forward and Forward+** widen the coat lane they shade with (`skinOralLane.y`), using the kernel
  taken next to the base filter.
- **Deferred** cannot take the derivative in the lighting pass, because a fullscreen derivative
  straddles silhouettes. The G-Buffer writer parks the kernel in RT5's green channel next to the
  skin thickness (`oloSkinPackGBufferLanes`), and the lighting pass widens its coat from it. A
  lightmapped skin draw keeps its baked light in RT5 and loses the kernel. That case is counted as
  `DeferredCoatFilterLaneUnavailable`.

The filter is `SkinVarianceKernel` / `oloSkinVarianceKernel`: `min(2 sigma^2 (|dN/dx|^2 + |dN/dy|^2), 0.18)`
added to alpha squared. The strength is the profile's `NormalVarianceStrength`. 0.25 is Tokuyoshi and
Kaplanyan's published value, and the strength the dog's eye and mouth use.

## What it looked like (#1533)

The showcase dog's eye, seen from below at about a metre, showed a white disc with a black core in
roughly one still in six. GL stills happened to miss it, so it read as a Vulkan defect. It was not:

- The tear film's coat sat at the 0.04 roughness floor, which makes the GGX peak about 1.2e5, under the
  sun, a delta light. The cornea is a smooth sphere a few dozen pixels wide, so the glint is far smaller
  than a sample.
- Frames whose TAA jitter landed on the glint read up to 845 in SceneColor, where the eye around it
  reads about one. The bloom spread that into the disc. Other frames read about one, so a single still
  either showed it or did not.
- On GL the same probe found 774. The mouth's coats (teeth at 0.04, tongue at 0.06) spiked to 810 in
  16 of 25 frames.

Turning the coat off took the eye rect from 14/100 frames above 5 to 1/100, which identified the source.
With a strength of 0.25 the glint became a steady catch-light of 6-12 in every frame, on both backends.

The black core was a second fault: TAA's post-resolve sharpen ran unbounded on linear HDR, so next to
a 460 sample a pixel's 3x3 mean was about 50 against its own value of 1, and the mask drove it below
zero. It is now clamped to the 3x3 range it sharpens (`PostProcess_TAA.glsl`, step 5;
`VulkanPassSuite.TaaSharpenStaysInsideTheRangeItSharpens`). Any bright sample, not only an eye's,
printed that ring.

## How to look for it

A single screenshot is a sample of one jitter phase. Measure many frames:

- `olo_render_target_stats` on `SceneColor` with a `rect` reports the finite max and the NaN/Inf counts.
  Call it 25-100 times with `forceFrame`, or with `afterPass` to see which pass writes the value. Count
  the frames above a threshold rather than reading one maximum.
- Narrow the source with rects (eye, mouth, nose, fur, grass) before reading any shader.
- A per-profile control is a hot reload away: an `.oloskin` edit reaches the next frame.

`DogShowcaseEvidenceTest.TheWetSurfacesCatchTheSunWithoutFireflies` holds the dog's skin and eye
pixels below 100 over 32 jittered frames, with the strength-0 control required to break the same
ceiling.
