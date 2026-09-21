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

The naive implementation is worse than no implementation. A stochastic coverage estimator — the
strand compositor, see [groom-strand-visibility.md](groom-strand-visibility.md) — **moves its
per-pixel coverage every frame by construction**, and averaging that away is the entire reason the
resolve exists. A term reacting to the raw delta drops history precisely where history is doing its
job: hair sparkles *worse* with the feature on than off, and because the mean is unaffected, a
settled screenshot of both arms looks identical.

**Size the band to the WORST CONSECUTIVE DELTA, not to the amplitude.** Two frames of a zero-mean
`±A` jitter can differ by `2A`, so a band of `A` still admits the largest jumps — and those are
exactly the frames that move the output most, so that band measures **worse than no term at all**.
For a `±0.06` estimator a `0.08` band gives a predicted shimmer ratio of **1.15** against the
no-band control, and a `0.13` band gives **0.315**. Compute this before choosing the constant; the
shipped `CoverageNoiseDeadBand` default of `0.12` clears the `±0.06` the strand compositor shows,
and a noisier estimator needs it raised.

What matters is not one frame's delta but whether the **mean** shifted: below the band the
estimator is converging, above it a LOD step or a leaf turning edge-on genuinely moved it. Same
reasoning as the sub-pixel dead zone already in `OloTemporalMotionFeedback`.

`TemporalReconstructionSequenceTest.StochasticCoverageNoiseMustNotDriveTheReactiveTerm` pins it in
the direction that matters — shimmer — and asserts its no-dead-band control actually shimmers
first. A test whose control does not move is broken, not passing.

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

**Run them headless, under mock time.** A live pixel A/B cannot answer any of this on a moving
subject — **69 % of pixels move between two captures of the same windy scene** — so the six minimal
reproductions run over the shipping model with nothing changing but the thing under test, each with
an in-run control arm.

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

## Where the coverage signal comes from

G-Buffer RT3 is `RGBA16F`: `.rg` velocity, `.b` coverage, `.a` material profile. It was widened
from `RG16F` rather than given its own attachment because **every writer had to be visited either
way** — an unwritten MRT component is undefined, not zero — and widening costs no new attachment
slot, texture binding or blackboard handle.

The three subjects fill `.b` with their own quantity:

| Subject | Coverage written | Why that quantity |
|---|---|---|
| Groom | the widened strand alpha | a sub-pixel strand is widened to one pixel and pays in alpha, so this IS coverage; it is also the value that moves every frame under stochastic composition |
| Foliage, near | cutout alpha x LOD fade | a density LOD step moves it while instance, primitive, material and depth all hold still |
| Foliage, impostor | `card.Coverage * card.DistFade` | so coverage does not jump across the impostor hand-over |

Everything else writes the opaque default `vec4(velocity, 1.0, 0.0)`.

**`GBufferCoverageChannelContractTest` is the forcing function.** It scans every shader for a
velocity write that does not cover four channels, because that mistake compiles cleanly, does not
warn, and produces whatever the previous tile left in memory — stable enough on one driver to pass
every capture you take.

## MSAA sharpens the coverage signal rather than damaging it

An MSAA resolve averages every colour attachment, including RT3, so a partially-covered edge pixel
resolves its coverage against the cleared 0 of the samples nothing wrote. Measured live on
`FoliageMeadowToWoodland`, OpenGL Deferred, the coverage range widens with sample count:

| MSAA | coverage range | total GPU |
|---|---|---|
| 1x | `[0.50, 0.996]` | 4.04 ms |
| 2x | `[0.50, 0.996]` | 5.18 ms |
| 4x | `[0.25, 0.996]` | 7.88 ms |
| 8x | `[0.07, 0.996]` | 11.40 ms |

That is the right direction: a leaf covering two of eight samples genuinely covers a quarter of the
pixel, and at 1x the same pixel could only answer "covered" or "not". Do not "fix" the widening
range by excluding RT3 from the resolve — averaging coverage with the cleared value IS the
sub-pixel measurement, the same reasoning `GBuffer.h` gives for RT5's irradiance-and-coverage pair.

**Confirm MSAA actually applied before trusting any of this.** The renderer-settings tool
acknowledges an `msaa` change without necessarily applying it; the GPU cost scaling 4 -> 11 ms
across the sweep is the evidence that it took, not the ack.

## The consumer, and the double-counting trap

TAA reads last frame's RT3 through a `SurfaceGeometry` history plane and feeds the coverage and
profile terms into the `confidence` argument that used to be a hard-coded `1.0`.

The plane is **extracted from the velocity target, not written by TAA** — the trick
`RayTracedShadowPass` already uses for its own surface plane — so TAA stays a single-attachment
pass. Making it MRT would have been the larger and riskier half of this change and it buys nothing.

**TAA passes `MotionMaxReactivity = 0`, and that is load-bearing.** TAA already scales feedback by
motion through `OloTemporalMotionFeedback`. Feeding the model's motion term into `confidence` as
well would count the same motion twice: feedback would fall as the SQUARE of the motion ramp, and a
camera pan would lose far more history than either mechanism intends. Zeroing the term in the
shared evaluator says that once, where the model can see it, rather than hand-assembling the
product of the other two in the shader where it would drift.
