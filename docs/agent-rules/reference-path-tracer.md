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
  even though `PBRCommon.glsl` also *has* `geometrySmithHeightCorrelated`. The lit passes call the
  former.
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

## 5. The DDGI divergence this instrument found (acceptance criterion 2)

**A DDGI probe volume fitted to a room's air silently kills the entire infinite-bounce term.
Probe irradiance comes out at roughly half of ground truth.**

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

### Status: documented, not fixed

Filed rather than fixed because the fix is a design decision, not a patch. `ddgiIsInsideVolume` is
also the guard that keeps the feedback loop *contractive* (ADR 0006), so the options — extend the
sample volume by a bias margin, clamp the lookup to the volume instead of zeroing it, or make the
authoring rule explicit and enforced — need weighing against stability by whoever owns #632.

**If you author a DDGI volume today: make it enclose the surfaces you want bounce light from, not
just the space the camera moves through.**

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

Cost first: the headless gate renders 96×96 at 192 samples (~2 s single-threaded in a release
build); the whole `PathTracing/` headless set is ~5 s. Cost scales linearly in samples, variance as
1/√N. Reach for resolution when the assertion is *spatial*, for samples when it is *photometric*.

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

For something a human can actually read, set **`OLO_PATHTRACER_EVIDENCE=1`** and the same test also
writes a converged 192×192 / 256-spp `PathTracer_CornellBox_HiRes.png`. Run that in a **Release**
build: the tracer is ~40× slower under MSVC Debug (measured across the gate renders — 5 s of
Release work is 200 s of Debug), which is also why the default budgets look so small.

What a correct converged reference looks like, so a broken one is recognisable:

- **Cornell box**: red and green walls, a bright emitter quad in the ceiling with the ceiling
  *around* it dark (one-sided downward emission — if that ring is lit, the emitter is two-sided or
  its winding is flipped), a soft shadow under the block, and visible colour bleeding onto the floor
  and onto the block's side faces.
- **Cornell furnace**: a near-uniform bright field in which the geometry all but disappears. That is
  the point — every surface converges to the same radiance. Structure still visible in the corners
  is the enclosure's energy loss, not a bug. If you can clearly see the box, something is absorbing.
