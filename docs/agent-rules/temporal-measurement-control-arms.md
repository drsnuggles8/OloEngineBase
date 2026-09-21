# Measuring a temporal resolve: the control arm is where it goes wrong

**The rules, first.** Every one of these cost a rebuild-and-remeasure cycle on #1256, and each was
caught only because something in the test asserted on the CONTROL rather than on the result.

1. **A control arm that reads exactly zero is broken, not clean.** A difference of `0.000000` between
   two frames means nothing rendered, nothing moved, or nothing was compared — not that the feature
   is perfect. Assert a floor on the control before you compare the arms.
2. **"Feature off" is not a control when the engine reconfigures around the feature.** Turning the
   thing off may turn something else off with it, and then the arms differ in two ways.
3. **A subject-presence guard must measure the SUBJECT**, against a capture with the subject
   removed. "Is any pixel lit" answers 100% on every frame with a sky in it.
4. **Assert that whatever you called moving actually moves.** A test named for a windy meadow will
   pass happily on a still one, and its numbers will look excellent.
5. **A live pixel A/B cannot measure a stochastic subject**, on any backend. Measure it headless.

The instruments themselves are `Renderer/TemporalSequenceMetrics.h`; the model is
[temporal-reactivity-separation.md](temporal-reactivity-separation.md). This page is about pointing
them at something real.

---

## "TAA off" is not a control for a groom

`SelectGroomComposition` **refuses the stochastic composition mode when no temporal resolve is
running**, because a stochastic estimator with nothing to converge it is just noise. The decision is
in `RenderPipeline.cpp`, from `GroomFrameState::TemporalResolveActive`, and it also accounts for a
temporal upscaler having taken over from engine TAA.

So a hair A/B of `TAAEnabled = true` against `TAAEnabled = false` compares **two different
composition modes**, not one mode with and without a resolve. Measured: the off arm's frame-to-frame
delta is exactly `0`, because a non-stochastic coat under a fixed camera is byte-identical every
frame. "TAA on shimmers more than TAA off" was true and meaningless.

**The arm that isolates the variable is `TAAEnabled = true` with `TAAFeedback = 0`.**
`OloTemporalMotionFeedback` returns 0, so no history is blended and the output is the current frame,
while the coat stays stochastically composited. One variable. On that comparison the resolve cuts
shimmer **11.3x** on Forward, 11.4x on Forward+, 11.9x on Deferred.

The same trap applies to any feature the pipeline gates a second feature on. Check what else reads
the flag before using it as an off switch.

## The dead band is inert on the TAA path, and that is correct

Setting `CoverageNoiseDeadBand` to `0` in `PostProcess_TAA.glsl` reproduces every number **byte for
byte**. That is not a stale shader and not a dead feature:

`PostProcess_TAA.glsl` clamps the previous coverage into the previous 3x3 neighbourhood's range
before the shared model sees it, so `|current - previous|` is already exactly zero for anything that
merely resampled — jitter or motion. The magnitude dead band is the **second** line of defence, for
when the whole neighbourhood has moved. Expect it to be inert in any scenario where the coverage
change is a resample.

**Prove the shader is reaching the GPU before concluding anything from an unchanged number.**
Identical numbers are also what a stale program looks like — the shader cache key does not cover
`#include`s, so editing only an include serves the previous program. Here, forcing
`confidence = 0.0` at the blend site moved the same
measurement from `0.00212` to `0.0247` — a 10x swing that proves the edit took, so the dead-band
result was a real null and not a cache hit.

## A presence guard that cannot fail is not a guard

The first subject-presence check counted pixels whose luma cleared a threshold and returned
**100% on every cell**, because the sky fills the frame. It would have passed on a capture with no
subject in it at all.

Measure against a capture with the subject **removed** and count the pixels that differ. And measure
the right subject: parking a whole terrain-plus-foliage entity makes "subject" mean the hill as well
as the plants, and a frame that is 95% bare ground still scores 100%. `MeasureFoliageFraction` in
`TemporalSubjectSequenceEvidenceTest.cpp` switches the layer off and leaves the terrain standing, for
exactly that reason — the reframe it forced took the meadow from 2% of the frame to 44%.

## Foliage wind needs four knobs, and may still not move

`WindStiffness`, `WindBranchWeight` and `WindLeafWeight` all default to **0**, and with them at zero
a `WindStrength` of any size displaces nothing. Set all four (`FoliageWindEvidenceTest` is the
reference for values that work).

Even then, measure it. On an authored-mesh layer at `MinScale` 1.6–2.4 with all four knobs set,
wind-on against wind-off differed by **0.012% of pixels**, and a capture ten seconds of mock time
later by **0.010%** — both the noise floor. A camera dolly supplied the motion for that test instead,
which is the harder temporal case anyway because every plant edge reprojects and disoccludes.

If you need motion you can rely on, move the camera. If you need the wind specifically, assert it
moved before you measure anything downstream of it.

## A live pixel A/B has no signal on a stochastic coat

Measured in the editor on `GroomReference.olo`, Vulkan, per render path — two captures of the
**identical** configuration against a TAA on/off pair:

| path | same-config control | TAA on vs off |
|---|---|---|
| Forward | 3.11% of pixels, max 138/255 | 4.73%, max 147 |
| Forward+ | 2.76%, max 102 | 4.75%, max 144 |
| Deferred | 0.14%, max 16 | 0.68%, max 25 |

The control and the signal are the same size. The coat re-jitters every frame by construction, so
there is nothing for a two-frame image comparison to find. **What a live session can answer** is
whether the pass runs, whether the backend complains, and what it costs — screenshots, the log, and
`olo_perf_pass_timings`. Take the image A/B headless, under mock time.

## Grooms render unlit on the Deferred path — on both backends

`GroomReference.olo` renders its grooms brightly on Forward and Forward+ and **nearly unlit on
Deferred**, and this is *not* a Vulkan defect: same scene, same camera, OpenGL Deferred does the same
thing. Confirm a suspected backend fault against the **same scene and the same pose on the other
backend** before reporting it — the first comparison here was against a different scene and invented
a Vulkan bug that does not exist.

A synthetic coat lit by a plain `DirectionalLightComponent` *is* lit on Deferred, so this is a
property of that scene's lighting setup rather than of grooms in general.

## Appendix — what the numbers were

GL, 320x240, `TemporalSubjectSequenceEvidenceTest`, coat under a static camera, resolve against the
no-history arm:

| cell | no history | resolved | gain |
|---|---|---|---|
| GL Forward | 0.0244 | 0.00216 | 11.3x |
| GL Forward+ | 0.0244 | 0.00215 | 11.4x |
| GL Deferred | 0.0242 | 0.00202 | 11.9x |
| GL Deferred, MSAA 4x | 0.0242 | 0.00203 | 11.9x |
| GL Forward, upscale Quality | 0.0255 | 0.00358 | 7.1x |
| GL Forward, upscale Performance | 0.0254 | 0.00580 | 4.3x |
| GL Forward, 449x307 | 0.0206 | 0.00176 | 11.7x |

**Upscaling costs the resolve roughly in proportion to the render-scale reduction** — fewer render
pixels means a noisier per-pixel coverage estimate, and the spatial upscale re-amplifies it. That is
the one cell of the matrix where the number is materially worse, and it is worth knowing before
recommending an upscale preset for a hair-heavy scene.
