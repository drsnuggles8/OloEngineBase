# The offline reference path tracer — how to use it, and what it cannot tell you

Issue #709. Code: `OloEngine/src/OloEngine/Renderer/PathTracing/`. Tests:
`OloEngine/tests/Rendering/PathTracing/`.

CLAUDE.md's rendering rule says contract tests prove the formula and screenshots prove "did it
change?". Neither answers **"is it correct?"**. Golden images are worse than neutral on that
question: they lock in whatever shipped, so a rebase after a "looks fine" change silently blesses a
regression. The reference tracer is the missing oracle — an unbiased Monte Carlo integrator that
shades with the engine's *own* BRDF and computes transport by brute force.

Read this before validating a GI / lighting / BRDF change against it, and before extending it.

---

## 1. What it is

- **CPU, not compute.** Decided in phase 0 and worth restating: an offline reference is
  offline *by definition*, so the only thing a GPU implementation buys is speed — at the cost of
  a scene TLAS upload, a compute traversal, a dependency on a live GL context (which kills
  "runs headless so it can gate CI"), and a collision with the in-flight RHI/bindless work.
  The CPU tracer reuses `BoundingVolumeHierarchy` directly, whose const queries are already
  documented thread-safe once built. If you ever *do* need the GPU version, note that the
  headless CI gate is the thing you must not lose.
- **`ReferenceBRDF.h` is a hand transcription of `PBRCommon.glsl`**, function for function, quirks
  included — see §3.
- **`ReferenceScene`** is not the ECS `Scene`. It is a literal-constructible, GL-free description
  (geometry + BVH, instances with a top-level BVH, punctual lights, emissive geometry, a uniform
  environment) so a fixture is twenty lines and every consumer runs headless.
- **`PathTracer`** is a unidirectional integrator with next-event estimation, power-heuristic MIS,
  Russian roulette and a uniform environment.

---

## 2. Determinism is a contract, not a nicety

Two renders of the same scene + camera + settings are **bit-identical, at any thread count**.
`PathTracerCornellBoxTest` asserts it as an exact hash, including a parallel-vs-sequential pair.

That is not perfectionism. A reference that is only reproducible up to thread scheduling forces
its own gate to use a tolerance, and that tolerance then has to be loose enough to swallow
scheduling noise — which is precisely the band a real regression hides in.

It rests on **two** things, and both are load-bearing:

1. `PathSampler` is **stateless**: every value is a pure function of
   (pixel, sample index, dimension, seed). No shared stream, no lock, no dependence on which pixel
   was traced first.
2. A single pixel's samples are summed by **one thread in ascending sample order**, so the
   floating-point accumulation order is fixed. Parallelism is over *rows only*.

Splitting a pixel's samples across threads is the obvious "optimisation" and it destroys the
contract while producing a statistically identical, visually indistinguishable image. Don't.

---

## 3. The BRDF port is deliberately unfaithful to textbook math

The reference must reproduce **what the renderer computes**, not what it should compute. Three
quirks are ported on purpose, and each is pinned by a test so a "cleanup" surfaces here rather than
as a silent brightness change:

- `distributionGGX` clamps its denominator with `max(denom, EPSILON)` where `EPSILON` is **1e-4**.
  Below roughness ≈ 0.274 that clamp engages at the NDF peak, and the engine's specular lobe
  therefore *collapses* as roughness → 0 instead of becoming a mirror. Pinned by
  `ReferenceBRDF.GgxNdfPeakIsEpsilonClampedAtLowRoughness`.
- `cookTorranceBRDF` uses the Schlick-GGX `k = (r+1)²/8` remap, not the height-correlated Smith —
  even though `PBRCommon.glsl` also *has* `visibilitySmithGGXCorrelated`. The lit passes call the
  former. (That second function is mirrored here too, as `VisibilitySmithGGXCorrelated`, even
  though nothing shades with it: a GLSL function with no C++ mirror is one `ReferenceBRDFGpuParity`
  cannot detect drift in, and #904 was exactly such a drift sitting undisturbed. See THE ALPHA
  LEDGER in `PBRCommon.glsl` for why the two G terms legitimately differ.)
- `kD = 1 - F` with `F` evaluated at the **half vector**: the common, mildly non-reciprocal
  formulation. It ships, so it is what the reference integrates.

**Sampling roughness is floored at `MIN_ROUGHNESS` (0.04); evaluation roughness is not.** Widening
only the sampling distribution keeps the estimator unbiased (the sampled lobe strictly contains the
evaluated one) while keeping the density off the numerical cliff at α → 0.

### A PDF describes the SAMPLER, never the integrand

The single nastiest bug in building this — caught by numerics before it ever ran, and worth
generalising.

`PdfGGX` was first written on top of the *same* `DistributionGGX` the BRDF evaluates, on the
reasonable-sounding grounds that "the sampled PDF and the evaluated D then can't drift apart". That
is exactly backwards. `ImportanceSampleGGX` inverts the **standard, unclamped** GGX CDF
(`cosθ = √((1-ξ)/(1+(α²-1)ξ))`); it has never heard of `EPSILON`. Below roughness ≈ 0.27 the
engine's clamp caps `D` right where the sampler concentrates its samples, so a clamped PDF
**under-reports** the density and `f / pdf` explodes.

Measured on the white furnace, clamped PDF vs. the analytic hemisphere integral:

| roughness | metallic | analytic | clamped PDF | correct (unclamped) PDF |
|---|---|---|---|---|
| 0.10 | 0.0 | 0.961 | **2.179** | 0.962 |
| 0.10 | 1.0 | 0.035 | **1.000** | 0.035 |
| 0.25 | 1.0 | 0.893 | 0.988 | 0.893 |
| 0.50 | 1.0 | 0.860 | 0.860 | 0.860 |

A reference returning 2.18 for a directional albedo is *creating energy*, and it would have blessed
every too-bright specular bug it exists to catch. Note the shape of the failure: it is invisible
above roughness 0.5, converges beautifully, and looks like a plausible image. There is nothing to
notice.

So: `DistributionGGXSamplingDensity` (unclamped, denormal floor only) feeds `PdfGGX`;
`DistributionGGX` (the engine's clamped form) feeds the BRDF. Both exist on purpose. The generalised
rule — **never reuse an evaluation-side numerical guard inside a density** — applies to any
importance-sampled integrator you add here.

`ReferenceBRDF.GgxSamplingMatchesItsDensity` pins it, and its low-roughness cases (0.05, 0.1, 0.2)
are the ones that matter: above ≈0.27 the clamped and unclamped NDFs coincide and a wrong PDF passes
the test.

### The anti-drift guard

`ReferenceBRDFGpuParityTest` renders `assets/shaders/tests/PbrBrdfParityProbe.glsl` — which calls
the production `cookTorranceBRDF` — over a (roughness × metallic × N·L) grid and diffs it against
the C++ port at 1% relative tolerance.

**If you edit `PBRCommon.glsl`'s BRDF, that test tells you to edit `ReferenceBRDF.h` too.** Without
it the failure mode is invisible: the reference keeps integrating the old formula, still converges
beautifully, and every comparison against it keeps "passing" while measuring a renderer that no
longer exists.

It is GPU-gated (SKIPs without GL 4.6), so a headless run does **not** cover it. Do not treat a
green headless suite as evidence that the port is current.

---

## 4. Validating something against the reference

### Compare the physical quantity, not the pixels

The obvious test — render both ways, diff the frames — answers the wrong question. A composited
frame folds in direct lighting, the ambient ladder, exposure and the tone curve. A disagreement
could come from any of them; an agreement can survive a completely broken GI field whenever the
direct term dominates.

DDGI's actual output is **irradiance at a probe, for a direction**, and that has a definition
independent of any renderer. `PathTracer::EstimateIrradiance` computes exactly it.
`DDGI_BlendIrradiance.glsl` pins its storage convention in a header comment ("the atlas stores full
irradiance E", via `E = π·Σ(w·L)/Σ(w)`), so the atlas texel and the traced integral are **the same
number in the same units** — no calibration constant, no fudge factor. That is what makes it a
ground-truth comparison rather than a correlation study.

Look for the same property in whatever you are validating: find the quantity the subsystem actually
computes, and check whether its units are pinned somewhere. If they are not, pin them first.

### Build both worlds from ONE description

`DDGIReferenceParityTest` drives the ECS scene and the `ReferenceScene` from a single table of
boxes. A hand-mirrored pair of scene descriptions is how this class of comparison goes quietly
wrong: a wall half a unit off, or an albedo of 0.5 on one side and 0.55 on the other, produces a
stable, plausible, entirely fictitious "divergence" that no amount of staring at the renderer will
explain.

Match the **light model** too, not just the geometry. The reference ports
`calculateAttenuation` — the engine's configurable constant/linear/quadratic falloff — rather than
physical inverse-square, and `ReferenceLight::AttenuationParams` must be packed the way
`Scene.cpp` packs it (`(1, 0, m_Attenuation, m_Range)` for a point light). If the reference used
"correct" physics here, every comparison would carry a distance-shaped divergence that is a
*convention* difference and would mask the transport bugs the instrument exists to find. **The
reference must differ from the raster path in transport, never in the light model.**

### Compare positions, not grid coordinates

DDGI relocates probes out of geometry by up to 0.45 of the grid spacing. Tracing the ground truth
at the *grid* position compares two different places in the room, and the result reads as a
transport error. Read `DDGIProbeUpdatePass::GetProbeRecords()[i].OffsetN` and go through
`DDGI::ProbeWorldPosition`.

### Assert the field's shape, not only its magnitude

A magnitude assertion needs a wide tolerance (a real-time GI approximation is allowed to be
approximate), and a wide tolerance passes for a lot of broken implementations. The **pairwise
ordering** of probes — does probe A rank above probe B in both worlds? — cannot be passed by a
global scale error or by a dead visibility term. Skip pairs the reference itself calls a tie, or
the assertion becomes a coin flip.

---

## 5. The DDGI divergence this instrument found, and what fixing it took (acceptance criterion 2)

**A DDGI probe volume fitted to a room's air silently killed the entire infinite-bounce term.
Probe irradiance came out at roughly half of ground truth.** Filed as **#751**, fixed there; the
diagnosis below is kept because it is the worked example of what this instrument is *for* — no
golden, no contract test and no screenshot could have produced it.

Measured, four probes, room with one point light, albedos 0.55–0.6 (`DDGIReferenceParityTest`):

| probe | DDGI | reference (4 bounces) | reference (direct only) | DDGI / full | DDGI / direct |
|---|---|---|---|---|---|
| (0, 0.8, 0) | 0.230 | 0.446 | 0.246 | 0.52 | **0.93** |
| (0, 3.2, 0) | 0.238 | 0.443 | 0.237 | 0.54 | **1.003** |
| (−3, 3.2, 0) | 0.342 | 0.560 | 0.341 | 0.61 | **1.005** |
| (3, 3.2, 0) | 0.097 | 0.236 | 0.098 | 0.41 | **0.989** |

DDGI reproduces the **direct** transport essentially exactly — to within 1% at three of four probes.
It contributes **zero** multi-bounce.

### The mechanism

A two-line chain, invisible from either end:

- `DDGI_Relight.glsl` computes the bounce term as
  `ddgiSampleIrradiance(u_PrevIrradiance, …, hitPos, …)` — the previous frame's irradiance **at the
  cached hit point**.
- `ddgiSampleIrradiance` (`include/DDGICommon.glsl`) opens with
  `if (!ddgiIsInsideVolume(worldPos)) return vec3(0.0);`.

Every cached hit point is **on a surface**. A volume fitted to the interior air therefore excludes
every wall, floor and ceiling — which is to say every surface the bounce light was supposed to come
from. The feedback term returns zero for all of them, on every probe, with no warning and no visual
signature beyond "the GI is a bit dim".

And air-fitted is the *natural* authoring: it is what `SandboxProject/Assets/Scenes/DDGITest.olo`
and `DDGIVisualEvidenceTest` both do (volume `[-7, 0.5, -5] … [7, 5.5, 5]` inside walls at ±8 / ±6).

### Confirmed by construction

`DDGIReferenceParityWideVolumeTest` runs the identical room, lights and reference, changing only the
volume bounds so the wall slabs fall **inside** it:

| volume | DDGI / direct | DDGI / full |
|---|---|---|
| fitted to the air | 0.93 – 1.005 | 0.41 – 0.61 |
| enclosing the walls | **1.74** | **1.12** |

With the walls inside, the bounce term comes alive and DDGI lands within 12% of full multi-bounce
ground truth — which is a perfectly respectable result for a real-time probe field.

### Status: fixed (#751)

It was filed rather than fixed on discovery because the fix is a design decision, not a patch:
`ddgiIsInsideVolume` was *also* what held the feedback loop's gain at zero, so relaxing it trades
against stability. What resolved that is in
[ADR 0007](../adr/0007-ddgi-hit-point-cache-gather.md) — the loop's Lipschitz constant is the
**albedo clamp** (0.9) and is independent of the volume test, which was only making the term dead —
and the shipped fix is the issue's option 3: the bounce path gathers through
`ddgiGatherIrradiance(…, volumeMargin, intensity)` with a margin of one probe spacing and intensity
1, while the lit passes keep the hard test through a zero-margin wrapper.

Measured after the fix, same four probes, same fixture:

| probe | DDGI | reference (4 bounces) | reference (direct only) | DDGI / full | DDGI / direct |
|---|---|---|---|---|---|
| (0, 0.8, 0) | 0.373 | 0.446 | 0.246 | 0.84 *(was 0.52)* | **1.52** *(was 0.93)* |
| (0, 3.2, 0) | 0.379 | 0.443 | 0.237 | 0.85 *(was 0.54)* | **1.60** *(was 1.003)* |
| (−3, 3.2, 0) | 0.498 | 0.560 | 0.341 | 0.89 *(was 0.61)* | **1.46** *(was 1.005)* |
| (3, 3.2, 0) | 0.186 | 0.236 | 0.098 | 0.79 *(was 0.41)* | **1.90** *(was 0.989)* |

A factor-of-two error became a 11–21% one, and it errs **low** — the residual is the margin's
smoothstep taper, not a missing term. The pass reports its own coverage for this volume as
**0.762**: the room's surfaces sit 0.29–0.42 of a probe spacing outside the bounds, so they gather
at 0.6–0.8 weight rather than 1.0, and the shortfall compounds through the bounce series. Landing
under ground truth rather than over is the side to err on for a feedback loop.

Two controls make that attribution rather than a story:

- **The wall-enclosing fixture is unchanged, to three digits.** `DDGIReferenceParityWideVolumeTest`
  still measures DDGI/direct **1.744** and DDGI/full **1.124**, exactly what it measured before the
  fix, and its coverage reads **1.000**. A volume that already enclosed its geometry needs no
  margin and gets none — the change is a no-op there.
- **That 1.124 is also the machinery's own error floor.** At full coverage this probe field lands
  12% *above* ground truth; the air-fitted case now lands 11–21% below. The two authorings no
  longer differ by a factor of two, they differ by the width of the approximation.


The **authoring rule still helps** — a volume more than one probe spacing away from the surfaces it
wants bounce light from still loses the term — but it is no longer the only thing standing between
an author and a silently dead feature. `DDGIProbeUpdatePass::GetBounceCoverage()` now reports the
**mean of `DDGI::VolumeWeight` over every captured hit point of every active probe** — the average
attenuation the bounce gather applies, each hit point counting once. It is a proxy for how much of
the bounce term survives rather than that fraction itself (a hit point's contribution is also
weighted by its radiance and cosine term, neither of which enters here), which is ample for the
authoring question it answers. The Light Probe Volume inspector warns below 50%. It reads **0.762** for the air-fitted fixture above, **1.000** for the
wall-enclosing one, and **0** for the pre-fix behaviour: the instrument's finding is now a number
the editor shows you. (It returns **-1** for "nothing to measure" rather than 1.0 — a diagnostic
whose job is to stop a dead feature reading as healthy must not confuse *unknown* with *fine*.)

### What generalises about the failure, not just the fix

Two things, and neither is about DDGI:

- **A guard can be load-bearing for a reason nobody wrote down.** `ddgiIsInsideVolume` was put
  there for a real reason (a lookup outside the grid has no probes to interpolate) and was
  *silently* also the thing keeping a feedback loop bounded. The way out was not to argue about
  the guard but to write the iteration down and find where the contraction actually comes from —
  at which point the guard turned out not to be load-bearing at all. If you are about to relax a
  guard "that is also what keeps X safe", derive X's real bound first; you may find it lives
  somewhere else entirely.
- **A missing guard is unobservable while the term it guards is zero.** ADR 0007 listed Lumen's
  10 cm `MinTraceDistanceToSampleSurface` anti-self-lighting rule as adopted; it never was. Nothing
  caught that for the feature's whole life, because the bounce term it guards was itself dead.
  Turning a dead path on is therefore also a *review* of every guard that path was supposed to
  have.

### The methodological point, which generalises

The comparison had to be structured to keep this finding legible. DDGI's radiance cache is
diffuse-only *by design* (`DDGI_Relight.glsl` shades `min(albedo, clamp)/PI * (directE + bounceE)`
and never calls `cookTorranceBRDF` — a probe hit point has no view direction, so the specular half
is meaningless there). So the reference is rendered in **both** shading models
(`ReferenceMaterial::LambertianDiffuseOnly`): DDGI is asserted against the Lambertian one, which
isolates its *transport*, and the Lambertian-vs-cookTorrance gap is reported separately as the
photometric divergence.

Had the two been folded together, their sum would have been reported as "DDGI's error" and the
factor-of-two would have been much harder to attribute. As it happens the photometric gap measures
**0.7%** for these rough dielectrics — negligible, and now known to be negligible rather than
assumed.

`LambertianDiffuseOnly` exists for exactly this and nothing else. A reference that quietly uses a
different BRDF than the renderer is not a reference.

---

## 6. Things that will bite you

- **A furnace scene needs `MaxBounces >= 2`.** The environment is only collected when a ray
  *escapes*, so `MaxBounces == 1` (direct lighting only) renders a furnace pitch black. Conversely
  `MaxBounces == 1` is exactly how you get a direct-only reference to difference against.
- **The emitter's winding decides whether the room is lit.** `AddQuadGeometry` derives the normal
  from `cross(p1-p0, p2-p0)`, and a one-sided emitter facing the wrong way renders a completely
  black scene — which reads as an integrator bug. The fixture helpers in
  `ReferenceSceneFixtures.h` each state the normal they produce; use them rather than open-coding a
  quad.
- **`MaxRadianceClamp` is a BIAS.** It is off by default and must stay off for anything claiming to
  be ground truth. It exists for eyeballing a noisy preview.
- **NEE off is a real cross-check, not a debug flag.** With and without NEE the integrator samples
  the same integral by different strategies; if they disagree beyond noise, the MIS weights are
  biased. That is the failure mode that converges beautifully to the wrong answer, and
  `PathTracerFurnace.NextEventEstimationDoesNotBias` is what catches it.
- **`AddInstance` rejects non-uniform scale, loudly.** The top-level traversal converts a running
  world-space `TMax` into instance-local units by a single scalar; under non-uniform scale no such
  scalar exists and the hit distances would be wrong in a way that still produces a plausible image.
- **Assert on region means, not per-pixel values.** A converged path-traced image is still a Monte
  Carlo estimate. A per-pixel assertion against it is a golden image with extra steps — the thing
  this instrument exists to replace.
- **Noise floor first.** Everything in `docs/agent-rules/live-verification-noise-floor.md` applies
  when you compare a reference against a *live* frame: measure the raster path's own frame-to-frame
  variation before attributing any difference to the renderer.

---

## 7. Sample count is sometimes a CORRECTNESS parameter

Cost first, per test, because the figures differ and a reader budgeting a new test needs the right
one:

| test | render | why that size |
|---|---|---|
| `PathTracerCornellBoxTest` physics/evidence | 64×64 @ 128 spp | region means; also the evidence PNG |
| `PathTracerCornellBoxTest` bounce | 48×48 @ 128 spp | frame/patch means only |
| `PathTracerFurnaceTest` Cornell furnace | 40×40 @ 160 spp | the sample count is a *correctness* parameter — see below |
| `PathTracerFurnaceTest` NEE-bias | 20×20 @ 96 / 768 spp | one region mean over ~160 pixels |

The whole `PathTracing/` headless set is ~5 s of work — but **~65 s under MSVC Debug**, which is
where the suite actually runs: the tracer measures ~40× slower there, and `ParallelFor` executes
inline because the test process starts no `FScheduler` workers. That 40× is why these budgets look
so small; they were cut from an initial 203 s with every threshold's measured margin recorded
beside it.

Cost scales linearly in samples, variance as 1/√N. Reach for resolution when the assertion is
*spatial*, for samples when it is *photometric*.

But note the trap in `CornellFurnaceCreatesNoEnergy`. The natural way to assert energy conservation
is "no pixel exceeds 1.0" — and that assertion is about the **sample count**, not the renderer. At
128 samples the (correct) render peaks at **1.069**, with 0.68% of channel estimates above 1.0; at
256 it peaks at **0.992** and none exceed 1.0. Nothing changed but the noise.

**The max of a Monte Carlo image is a noise statistic, not a bound on what is being estimated.** So
the energy claim is made on the frame *mean* (12288 channel estimates averaged — its own error is
negligible), the tail is bounded as a *fraction* rather than by an extreme order statistic, and the
per-pixel ceiling is parked at 1.5, where only gross creation reaches. A Russian roulette that
forgot to divide by the survival probability compounds into the tens; it does not sneak in at 1.06.

Measured reference values on the fixtures, for anyone recalibrating:

| quantity | value |
|---|---|
| single-scatter furnace, roughness 1.0, metallic 0 | 0.972 |
| single-scatter furnace, roughness 0.1, metallic 1 | 0.035 (the ε-clamp regime) |
| Cornell furnace (albedo 1, env 1), frame mean | 0.858 |
| Cornell box floor r/g beside the red vs green wall | 1.36 vs 0.89 |
| open floor vs the block's shadow | 3.3× |
| ceiling: direct-only vs 8 bounces | 0.000 → 0.114 |
| NEE vs BSDF-only region mean, worst channel | 1.3% apart |

## 8. Evidence

`PathTracerCornellBoxTest` writes `OloEditor/assets/tests/visual/PathTracer_CornellBox.png` and
`…_DirectOnly.png` at the gate's resolution (64×64 — small, because every assertion in that file is
a region mean and resolution buys them nothing but runtime). They are diagnostics, not goldens —
nothing compares against them, and the test never fails because it could not write one.

For something a human can actually read, pass **`--olo-pathtracer-evidence`** and the same test also
writes converged 192×192 / 256-spp frames from **two** angles —
`PathTracer_CornellBox_HiRes_HeadOn.png` and `…_Raking.png`. Two, because they cover different
things: head-on shows the emitter and the ceiling (the indirect-ONLY surface), while the raking pose
— inside the room, looking down across the floor — shows the block's side faces, where colour
bleeding is most legible (left face reads red, right face green). Run it in a **Release** build (see
the 40× Debug penalty above).

What a correct converged reference looks like, so a broken one is recognisable:

- **Cornell box, head-on**: red and green walls, a bright emitter quad in the ceiling with the
  ceiling *around* it dark (one-sided downward emission — if that ring is lit, the emitter is
  two-sided or its winding is flipped), and a soft shadow under the block.
- **Cornell box, raking**: the block's left face tinted red and its right face green. If both faces
  are neutral grey, indirect light is not carrying wall albedo — which is the whole phenomenon a GI
  implementation exists to produce.
- **Cornell furnace**: a near-uniform bright field in which the geometry all but disappears. That is
  the point — every surface converges to the same radiance. Structure still visible in the corners
  is the enclosure's energy loss, not a bug. If you can clearly see the box, something is absorbing.
