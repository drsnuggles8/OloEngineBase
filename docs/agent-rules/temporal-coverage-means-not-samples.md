# A coverage term compares an estimator's mean, not its samples

**The rule.** Where a pixel's coverage comes from a stochastic estimator, the temporal coverage term
compares the two frames' **neighbourhood means**, never a pixel against last frame's values. TAA does
this wherever the current 4x4 neighbourhood holds a fractional coverage (strictly between 0 and 1): it
compares the 8x8 box means of the two frames' RT3 `.b`. A neighbourhood of only 0s and 1s, an opaque
edge, keeps the range test (the current pixel clamped into last frame's 3x3 range). Issue #1552.

The code is the coverage block in `OloEditor/assets/shaders/PostProcess_TAA.glsl`. The pins are
`TemporalSubjectSequenceEvidenceTest.TheResolveCutsAStochasticCoatsShimmerManyFoldOverNoHistory` (the
noise must not fire it) and `TheResolveLetsGoOfACoatWhoseCoverageHalves` (a real change must).

## What a stochastic coat writes into `.b`

Under `StochasticAlpha` each strand fragment survives a hashed test against its widened alpha, and the
survivor writes that alpha into RT3 `.b`. So every frame a different strand wins each pixel, or none
does and the pixel shows what is behind: 0 over the background, 1 over the body. The per-pixel value
is a fresh draw every frame from roughly `{0 or 1} ∪ [0.29, 0.60]` (the alpha spread measured on
`TemporalSubjectSequence`'s hair). That is far wider than the `±0.06` estimator the dead band was
first sized against in [temporal-reactivity-separation.md](temporal-reactivity-separation.md). No
per-pixel band can absorb it without also absorbing the changes the term exists for.

## The range test fails both ways on it

The range test answers "is this pixel's coverage still within what its neighbourhood held last
frame". That is right for a deterministic field that only moves, such as jitter or motion resampling
an alpha-tested leaf. On a stochastic coat the 3x3 range is only a sample of nine draws, and it fails
in both directions. Measured by reading back RT3 every frame and running both tests on the CPU:

| still hair, 320x240 | range test | 8x8 means |
|---|---|---|
| coat pixels firing per frame at rest | 1.8 % | 0.1 % |
| mean reactivity at rest | 0.014 | < 0.0001 |
| coat pixels firing when every strand halves in width | 15 % | 92 % |
| mean reactivity at that step | 0.07 | 0.40 |

- **At rest it fires.** Most firings were a hole whose nine neighbours all drew a strand. Each one
  throws a converged pixel back to a single sample, whose next frame-to-frame change is about ten
  times a converged pixel's at feedback 0.9. The resolve kept 7.5x of the 12.1x it reaches with the
  term removed.
- **At a real step it is blind.** The holes put 0 into nearly every range, so a halved alpha still
  lands inside it.

An 8x8 box mean carries an eighth of the per-pixel noise, and it is the quantity a LOD step actually
moves. With the hybrid the stochastic coat accumulates 11.8x to 12.0x on every path.

## Why the opaque edge keeps the range test

Jitter moves an opaque silhouette by up to a texel between frames. That shifts a box mean by up to one
row in eight, and fired an 8x8 mean test on 0.6 % of a sphere's pixels (a 5x5 test on 4 %). It never
leaves the 3x3 range, so the range test fires on none of them. The decision reads the **current**
frame through `textureGather`, which returns texels whatever the sampler's filter. A bilinear fetch of
the previous frame at a moving `prevUV` blends an opaque edge into fractional values and would send
every moving silhouette down the estimator branch.

## The dead band is load-bearing again

Before this change the coat's RT3 was the cleared value behind it, and setting
`CoverageNoiseDeadBand` to 0 changed nothing ([temporal-measurement-control-arms.md](temporal-measurement-control-arms.md)
recorded that null). It is no longer a null: in the estimator branch the band is what keeps the
means' residual noise from firing, and at 0 the still coat's gain falls from 11.9x to 7.1x.

## A stochastic estimator keeps its feedback in motion

TAA's motion ramp halves the history weight above 5 px a frame, trading history for the current
frame: a good trade where the current frame is exact. A stochastic coat's current frame is one draw of
noise, and its velocity is each strand's own, so the ramp kept the noise and dropped the signal. On
the dog's walk from behind, two draws of the same frames differed by 0.51 of what the samples alone
make them differ on the tail and the fringe; 0.41 and 0.45 without the ramp. Lag rose from 0.37 to
0.44 of the distance to four frames back. Removing the colour clip for these pixels (0.51 on both) or
widening it to a 5x5 neighbourhood (0.50) did not help; the ramp was the cost.

**Key the exemption on STOCHASTIC, not on fractional.** An alpha-tested leaf writes fractional coverage
too, and its current frame is exact. Exempting every fractional pixel left the meadow's over-blurred
control arm ghosting where it should blur: it retained 0.81 of the detail against the shipping arm's
0.78, and `AMeadowInMotionKeepsItsDetailUnderTheResolve` failed its own instrument check. So the
groom marks its pixels under `StochasticAlpha`: RT3 `.a`, the material profile a strand does not have,
is `OLO_STOCHASTIC_COVERAGE_MARK` (-1, `include/SurfaceCoverageMark.glsl`). TAA reads the mark from the
current 4x4 and takes `max(.a, 0)` as the profile, so a coat pixel that shows the lawn behind it next
frame has not changed material.

## How the numbers were found

A test hook after `TAAPass` read the RT3 texture back each frame, and the CPU re-ran candidate tests
on consecutive frames: point against 3x3 and 5x5 ranges, range against range, 3x3/5x5/7x7/8x8 mean
differences, and the hybrids. It ran on three subjects: still hair, opaque skin, windy foliage. It
also ran with two forced steps, half the strand width and half the strand count. Neither the range
test nor any one mean test was right on every subject; the hybrid was. Write the capture before
choosing a constant: four candidates that each looked principled were wrong on at least one
subject.
