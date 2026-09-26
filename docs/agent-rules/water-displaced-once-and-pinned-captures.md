# Only one stage may displace a surface, and only a pinned clock makes an A/B

Three rules from issue #1470. Each one cost a wrong conclusion before it was written down.

## 1. "Tessellation off" still runs the tess-eval stage

**If a program has a tess-eval stage, the vertex stage must not displace.** Every water program
(`Water.glsl`, `Water_Depth.glsl`) has a TES, and every water draw is a patch list, so the TES always
runs. A tess factor of 0 means a tess level of 1, not "no TES".

The water vertex stage used to displace whenever the factor was 0, which is the `WaterComponent`
default, and the TES then displaced the displaced point again. The result was double the swell and
double the choppy drift, sampled at the wrong XZ. It affected every default water scene for as long
as the branch existed. The physics side (`WaterSurface::SampleHeight*`) displaces once, so boats
floated on a surface that was not the one drawn.

What stayed green: every water test. The tess-on path, which does displace once, was the one the
visual tests happened to exercise. The shore, cascade and wake text tests asserted that "both
displacing stages" called the shared sums, which pinned the bug instead of catching it.

Pinned by `WaterRendering.VertexStageNeverDisplaces`, a counted text contract, and by
`WaterSingleDisplacementTest`: tess off must render the same frame as tess on at level 1, and a
different frame from amplitude 0.

## 2. A live-clock capture is not a backend A/B for anything animated

**Pin the clock before comparing two renders of an animated scene.** Until #1470 the editor's
`olo_benchmark_capture` rendered with the live clock. A GL-vs-Vulkan pair of the integrated
benchmark therefore compared two wave phases of a 10 m tile under a 180 m swell, and the difference
read as "Vulkan renders different geometry": a diagonal edge on one backend, a vertical one on the
other, 620 px apart. With the clock pinned to the manifest's `t0 + n * dt`, the two agree to a 1 px
median edge offset. The measurements are in
[water-parity-1470](../testing/evidence/integrated-renderer-1338/water-parity-1470/README.md).

A pinned clock can sit below the wall clock an editor has been reading. `FramePacer::FrameDelta`
makes that backwards step a zero-length frame rather than a negative `Timestep`.

## 3. Switching off one term of a `max()` proves nothing about the others

**To find which term saturates, switch every term off together, then re-enable one at a time.**
While isolating Vulkan Forward's white water, switching foam sources off one by one left the
surface white at every step, because another term was saturated each time. A
`pow(1 - x, power)` term is not disabled by a huge `power` when `x` is 0: `pow(1, 500)` is 1.

The foam isolation needed a temporary fragment output of the terms themselves (`r`, `g`, `b` =
three terms) and then of their inputs. That output named the saturated term in one run, after four
toggle runs had not (#1486).
