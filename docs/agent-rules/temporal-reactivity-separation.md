# Temporal reactivity: separate the causes, and give the coverage term a dead band

**The rules, first.**

1. **Keep the causes of history rejection apart.** Surface motion, coverage change and
   material/profile change are three different reasons a pixel's history is stale. Carry them as
   three scalars, not one blended confidence. A single number cannot be tuned — retuning it for
   hair retunes it for skin — and it cannot be attributed, so a test or a debug view can never say
   *which* cause fired.
2. **A reactive term driven by a stochastic estimator needs a DEAD BAND sized to that estimator's
   own per-frame noise.** Without one the term reacts to the noise the temporal resolve exists to
   average away, and the subject gets *worse* with the feature on than off — while every still
   capture looks fine.
3. **Combine the causes by multiplying the fractions that survive each**
   (`(1-m)(1-c)(1-p)`), not by taking a max or a sum. A max lets the largest cause mask the other
   two; a sum saturates past 1 and needs a clamp that quietly turns three small causes into a total
   rejection.
4. **An unassigned GLSL struct field is undefined.** Adding a field to a shared record struct means
   visiting every factory that assigns fields individually, not just the ones that read the new one.

The model is `Renderer/SurfaceHistory.h` (`TemporalReactivity`, `EvaluateTemporalReactivity`) and
its twin `OloEditor/assets/shaders/include/SurfaceHistory.glsl`. The measurement instruments are
`Renderer/TemporalSequenceMetrics.h`. Both arrived with issue #1256.

---

## Why one confidence number is not enough

Before #1256 the shared history model separated surface motion (`MotionMismatch`) and material
identity (`MaterialMismatch`), and had **no coverage channel at all**. For an opaque surface that
is fine. For hair and foliage it is the dominant failure:

> A strand or leaf whose fractional pixel coverage changes frame to frame presents as the **same
> instance, the same primitive, the same material, at the same depth**.

Every existing test passes. Every still capture looks right. The history is accepted at full weight
and the old, denser value is dragged across the transition — which is what ghosting at a groom or
foliage LOD step actually is. Nothing in a depth/normal/identity model can see it, because nothing
about the *surface* changed; what changed is how much of the pixel the surface occupies.

So coverage is a channel in its own right, and the three causes stay separated because they are
tuned against different subjects. Hair needs a coverage response that would make skin swim; skin
needs a profile response hair never exercises. This is the regression the issue's own wording
warns about — *a history heuristic tuned on one subject regresses another*.

Keeping them apart buys attribution as well as tuning. "The coverage term fired" is a finding.
"Confidence was 0.4" is not.

## The dead band is the load-bearing part

This is the trap, and it is worth stating plainly because the naive implementation is worse than
no implementation:

A stochastic coverage estimator — which is what the strand compositor is, see
[groom-strand-visibility.md](groom-strand-visibility.md) and `Groom/GroomCoverage.h` — **moves its
per-pixel coverage every frame by construction.** That movement is zero-mean noise, and averaging
it away is the entire reason the temporal resolve exists.

A coverage term that reacted to the raw frame-to-frame delta would therefore drop history
**precisely where history is doing its job.** Hair would sparkle worse with the feature enabled
than disabled. And because the mean is unaffected, a settled screenshot of both arms looks
identical — the defect lives entirely in the frame-to-frame behaviour.

**Size the band to the WORST CONSECUTIVE DELTA, not to the amplitude.** Two frames of a zero-mean
`±A` jitter can differ by `2A`, so a band of `A` still admits the largest jumps — and those are
exactly the frames that move the output most, so that band measures **worse than no term at all**.
For a `±0.06` estimator a `0.08` band gives a predicted shimmer ratio of **1.15** against the
no-band control, and a `0.13` band gives **0.315**. Compute this before choosing the constant; the
shipped `CoverageNoiseDeadBand` default of `0.12` clears the `±0.06` the strand compositor shows,
and a noisier estimator needs it raised.

What separates the two cases is not the size of the delta on any one frame but whether the **mean**
has shifted:

- **below the dead band** — the estimator is converging. Keep accumulating.
- **above it** — the mean genuinely moved. A LOD step thinned the layer, a leaf rotated edge-on,
  an alpha test flipped. The history is stale.

This is the same reasoning, for the same reason, as the sub-pixel dead zone already in
`OloTemporalMotionFeedback`: a jittered pass moves ~1 px every frame by construction too, and
without the dead zone a stationary camera reads as motion.

`TemporalReconstructionSequenceTest.StochasticCoverageNoiseMustNotDriveTheReactiveTerm` pins this
in the direction that matters — shimmer — with the no-dead-band arm as its control, and asserts
that the control arm actually shimmers first. A test whose control does not move is broken, not
passing.

## A graded term and a hard rejection are not two ways of saying the same thing

The coverage channel has both: a reactive term that saturates at a change of 0.35, and a hard
`CoverageMismatch` rejection at 0.5. The thresholds overlap, so the rejection can never be what
decides the blend **weight** — which looks redundant and is not. They differ in what they do to the
**accumulator**:

- a saturated reactive term drives confidence to 0, so no history is blended this frame, but
  `HistoryLength` survives and the accumulator resumes at its converged rate;
- a rejection makes the history invalid, so `AccumulateTemporalMoments` resets `HistoryLength` to 1
  and reseeds the moments.

So a change in `[0.35, 0.5]` says *ignore the history this frame*, and one past 0.5 says *the
accumulator is describing a surface that is gone; start over*. Collapsing them makes a brief
thinning throw away a long convergence.

**This was found by breaking the rejection on purpose and noticing a test still passed.** The
LOD-transition reproduction is defended by the reactive term, not the rejection, so disabling the
rejection changed nothing it asserted. A negative control is not a formality: run it, and when
something survives it, that is a gap in the tests, not luck.

## Measure across frames or do not measure

The three defects are invisible in a settled frame. Two settled captures are equally settled
whatever the accumulator is doing, so a resolve that ghosts, sparkles, or has quietly blurred away
half the detail passes every still comparison in the suite.

`TemporalSequenceMetrics` gives each defect its own instrument, deliberately not collapsed into one
"temporal error" number — a fix that trades shimmer for ghosting would read as an improvement:

| Defect | Instrument | Sequence shape |
|---|---|---|
| Shimmer | `MeasureShimmer` — mean frame-to-frame delta | static configuration |
| Ghosting | `MeasureGhosting` — settling frames, residual area | a step to a new steady state |
| Detail loss | `MeasureDetail` — retained spatial variance | against an unfiltered reference |

Detail loss is not optional garnish. A resolve can beat both shimmer and ghosting by blurring
everything to a flat field, and retained variance is the number that catches it doing so.

Two implementation notes that are easy to get wrong:

- **`MeasureGhosting` scans backwards** for the first frame from which every *later* frame is
  within tolerance. Scanning forwards for the first frame under tolerance reports a resolve that
  dips through the target and comes back out as settled on the dip — which is exactly what an
  overshooting history does.
- **Pixels neither side touches are excluded** from every metric, or a larger empty border drives
  every number toward zero and makes two resolutions incomparable. Same choice, same reason, as
  `GroomCoverage::CompareCoverage`.

## Run it headless, under mock time

A live pixel A/B cannot answer any of these questions on a moving subject: **69 % of pixels move
between two captures of the same windy scene**, so there is no signal to measure. The six minimal
reproductions #1256 asks for — disocclusion, camera cut, animated deformation, LOD transition,
alpha coverage, dynamic resolution — all run headless over the shipping model, where the only
thing changing is the thing under test.

Each one carries an **in-run control arm**, because a temporal test that also passes with the
feature disabled is not a test. The assertion is a comparison between two measurements taken in
the same process, never against a tuned constant.

## Adding a field to the shared record

`SurfaceHistoryRecord` has a GLSL twin, and the twin's factories assign **every field
individually**. An unassigned GLSL struct field is undefined — not zero — so adding a field and
leaving the existing factories alone plants a landmine that only goes off when someone later
enables the matching test bit.

When you add one:

- give the C++ field a default that makes the new test **inert for existing callers** (coverage
  defaults to 1.0, fully covered, so an opaque caller's delta is exactly zero);
- gate both the finiteness check and the rejection behind an opt-in `TestMask` bit;
- visit every factory in `OloEditor/assets/shaders/` that builds the record — at the time of
  writing: `PostProcess_SSGIResolve.glsl`, `RayTracedShadowResolve.glsl`,
  `ReSTIR_DI_TemporalReuse.glsl`, `ReSTIR_GI_TemporalReuse.glsl`;
- extend `tests/ShaderUnit_SurfaceHistory.glsl` and assert the GPU result **against the C++ model
  run on the same inputs**, never against a hard-coded constant. Constants let a retune of the
  shipping defaults drift the two implementations apart while both still pass.

## What is not wired yet

The coverage channel is live in the model and in the GLSL twin, but **no pass supplies a real
per-pixel coverage signal**, because there is nowhere to put one: RT3 is `RG16F` and full, and
`GBuffer.h` requires every G-Buffer writer to write every target. Producing the AOV is a G-Buffer
widening across both backends and all three render paths — tracked separately rather than folded
in. Until then every production consumer passes the inert 1.0 and the coverage terms cannot fire
on real pixels.
