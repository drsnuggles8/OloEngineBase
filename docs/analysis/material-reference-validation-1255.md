# Independent material references: skin, leaf and fibre (#1255)

**The rule this establishes: a specialised material model is judged against a reference computed by
a different route, and where no such reference exists the limitation is written down rather than
filled in with generic PBR.**

The engine's reference apparatus — `OloEngine/src/OloEngine/Renderer/PathTracing/ReferenceBRDF.h`
and the `Reference*Test` files beside it — is a faithful CPU mirror of `PBRCommon.glsl`'s
Cook-Torrance BRDF. It is the right oracle for a metal-rough surface and the wrong one for all
three models this issue covers: none of them *is* a Cook-Torrance surface, so "compared against the
reference tracer" would mean "compared against a model that cannot represent it". #1255's fourth
acceptance criterion names that failure outright.

This document records, per model, what the reference **is**, its units, its coordinate convention,
its tolerance and its declared limitations. The code is
[`OloEngine/tests/Rendering/PathTracing/MaterialReference.h`](../../OloEngine/tests/Rendering/PathTracing/MaterialReference.h);
every claim below is an assertion in one of the four test files listed at the end, so the argument
fails loudly when it stops being true instead of ageing quietly in a document.

---

## 0. The local / nonlocal split, which decides which questions have answers

Acceptance criterion 2 asks for local BSDF tests to be distinguished from nonlocal skin diffusion
and coat transport. The distinction is not a filing convenience — it decides what can be asked:

| Model | Class | Quantity | Energy? | Reciprocity? | Sampling consistency? |
|---|---|---|---|---|---|
| Fibre (hair/fur) | **local** | BCSDF of two directions | yes | yes — and the answer is *no*, see §1 | yes |
| Leaf transmission | **local** | lobe of two directions | yes — and it is unbounded, see §2 | yes — and the answer is *no* | n/a, nothing samples it |
| Skin diffusion | **nonlocal** | `R(r)`, a function of one length | shape only, see §3 | **no referent** | **no referent** |
| Coat transport | **nonlocal** | transmittance along a path | n/a | trivially yes, uninteresting | **the estimator is the question**, see §4 |

Asking a diffusion profile for reciprocity would be asserting a coincidence: it would pass or fail
on whatever the screen-space pass happened to do with directions it never consults. The file split
follows the table — `FibreScatteringReferenceTest.cpp` and `LeafTransmissionReferenceTest.cpp` are
the local pair, `NonlocalTransportReferenceTest.cpp` is the other two.

---

## 1. Fibre — a dielectric-cylinder cross-section walk

**The reference.** `MaterialReference::FibreCylinderWalk`. It builds the 3D entry geometry from two
explicit vectors, refracts with the full vector form of Snell, walks the interface events one at a
time and sums the path series term by term. A second, **stochastic** walk
(`FibreCylinderStochasticEnergies`) reflects or refracts by Fresnel probability and never forms the
recurrence's products at all — so a mistake shared by two energy-*splitting* schemes cannot survive
both.

**Units and conventions.**

| Quantity | Unit / convention |
|---|---|
| Fibre | infinite circular cylinder, **unit radius**, axis along local `+x` |
| `theta` | measured **from the plane perpendicular to the axis**: `sin(theta) = w.x`. The fibre convention, not the surface one |
| `h` | signed impact parameter in the normal plane, **radius units**, `[-1, 1]`; `gamma_o = asin(h)` |
| `sigma_a` | per unit length at unit radius — the head-on chord it multiplies is **2** |
| `phi` | azimuth **difference** about the tangent, radians |

> **A documentation discrepancy, not a defect.** `GroomFibreScattering.h` describes `sigma_a` as
> being "in units of the fibre DIAMETER". The expression it ships,
> `exp(-sigma_a * 2 cos(gamma_t) / cos(theta_t))`, is per **radius**: a head-on chord on a
> unit-radius cylinder is 2, so `sigma_a` multiplies 2 rather than 1. The reference uses the code's
> convention, because the code is what renders. Nothing behaves wrongly; the comment is one word
> out. Not changed here — `GroomFibreScattering.*` is off-limits to this slice (a sibling worktree
> is editing adjacent groom code), so it is reported instead.

**Tolerances.**

| Assertion | Tolerance | Why that number |
|---|---|---|
| `cos(psi) = cos(theta_o) cos(gamma_o)` | `1e-12` absolute | both sides f64, an identity |
| Bravais index, chord length | `1e-9` relative | f64, an identity through one `asin` |
| Internal vs external Fresnel | `1e-12` absolute | reversibility, exact |
| Per-lobe attenuation, production vs walk | `3e-5` relative | production accumulates 32 f32 adds; this is the mantissa, not a model gap |
| Stochastic vs deterministic walk | `4e-3` absolute | ~4 standard errors at 200k samples |
| Pdf normalisation over the sphere | `0.015` absolute | the midpoint rule's error on a lobed density at 256×256 |
| Sampled albedo vs the walk | `1.5e-2` absolute | ~5 standard errors at 60k samples |

**What the reference establishes that no existing test did.** `GroomFibrePropertyTests` compares the
shipped h-quadrature against the *same model's* converged quadrature, and `GroomFibreGpuParityTest`
compares the shader against the C++ twin. Both are valuable and neither can see a sign error in
Chiang's attenuation recurrence: both arms would reproduce it. Three production formulas are
consequences of the reference rather than inputs to it — the incidence cosine, Bravais' modified
index and the closed-form geometric tail — and each is correct at normal incidence, which is why a
screenshot cannot separate them.

**Declared limitation — reciprocity.** The far-field model with a cuticle tilt is **not a reciprocal
BCSDF**, and this is structural rather than a bug. The tilt rotates the *outgoing* longitudinal
angle only, which is exactly what separates the R and TRT highlights on a real head and the entire
reason the parameter exists; the attenuations also see `cos(theta_o)` alone. On the specular cone at
zero tilt the residual is float noise; at the shipped 2° tilt it is an order of magnitude above
that, and it grows with the tilt. The test asserts the ordering over three tilts rather than a
threshold, because a threshold would pin one machine's float noise.

---

## 2. Leaf — a plane-parallel translucent slab

**The reference.** `MaterialReference::SlabRandomWalk` — a Monte Carlo random walk through a
plane-parallel slab with Henyey-Greenstein scattering, which is what a leaf lamina physically is.

**Units and conventions.**

| Quantity | Unit / convention |
|---|---|
| Slab | `z` in `[0, tau]`, `tau` the **optical thickness**, dimensionless, in mean free paths |
| `+z` | from the **lit** face towards the **shaded** face |
| `mu` | cosine with `+z`; a photon leaving the shaded face has `mu > 0` |
| Boundaries | **index-matched** — declared limitation, see below |
| Phase function | Henyey-Greenstein, asymmetry `g` an argument (0.7 in the tests) |

**The honest headline: there is no fit, and there cannot be one.** The production term,
`oloFoliageTransmissionDirect` in `assets/shaders/include/FoliageSurface.glsl`, is a
phenomenological wrap-and-forward lobe. It is not derived from transport, it carries no optical
thickness, and it is not normalised. So there is no parameter setting at which it *equals* a
transport solution, and a test asserting one would be measuring a fit that does not exist. What the
comparison can be about — and is — is three things:

1. **What the reference has and the model does not.**
   - *Reciprocity.* The slab's BTDF is reciprocal to within 8% across the sampled pairs (three
     standard errors of two independent 120k-sample walks). The production lobe is not, by more
     than 25% across the authored range, and structurally: its wrap half,
     `max(dot(-N, L), 0) * wrap`, does not mention `V` at all.

     > **The reciprocal quantity is the BTDF, not the binned energy.** The walk injects unit
     > *irradiance*, so the energy leaving a cosine bin is `f · mu_o · 2 pi dmu` — the exit cosine
     > is in there. Comparing the binned energies instead reports a reciprocity failure of exactly
     > `mu_o/mu_i`, which is what the first version of this test did (measured 0.6163 against a
     > predicted 0.613 — the match is how the missing factor was identified rather than mistaken
     > for a broken solver). `SlabResponse::TransmittedBtdf` carries the cosine; the raw
     > `TransmittedByCosine` array deliberately does not, and says so.
   - *An energy bound.* The slab's transmittance is a fraction and cannot exceed 1. The production
     lobe's directional-hemispherical transmittance **exceeds 1** at authored values inside the
     shipped ranges — `wrap = 1`, `thickness = 1` is enough, because the wrap term is constant over
     the hemisphere and integrates to `wrap * thickness * pi * cos(theta_l)`.

   Neither is a defect report. An artist-facing translucency **gain** is a legitimate design and
   #1234 chose it deliberately for a lamina whose physical thickness is never authored. Both *are*
   limitations that have to be written down, because together they mean **no absolute comparison
   against a transport reference can ever be made for this model** — only the shape comparisons in
   (2) and (3).

2. **What both have.** A forward-scattering peak on the light direction, falling monotonically away
   from it. Asserted as an ordering in both, because the two share no parameter and only the
   ordering transfers.

3. **Where the correspondence breaks — flatness is authored, not physical.** The wrap half,
   `max(dot(-N, L), 0) * wrap`, does not mention `V`, so `lobe(wrap = w) - lobe(wrap = 0)` is the
   same number at every view direction, to the last bit. It is a **floor** the author sets: raising
   `wrap` lifts the lobe's minimum over the hemisphere without moving its peak. Transport has no
   such slider — a slab's flatness is *determined* by its optical thickness, and the only way to
   flatten it is to make the leaf thicker, which also darkens it. That is the difference between a
   phenomenological term and a physical one, and it is why no parameter fit between the two exists.

   *An earlier draft of this claimed the whole lobe goes isotropic at `wrap = 1` and measured a 0.83
   spread instead: at a grazing backlight the forward half is still the larger term. The narrower
   statement above is the one that is exactly true, and it is asserted as an identity rather than a
   tolerance.*

**Declared limitation of the reference itself.** The slab's boundaries are index-matched. A real
leaf's cuticle has an index step and it matters for the specular sheen — but the production term
this judges has no refractive boundary at all, so adding one to the reference would measure the
difference between two unrelated decisions.

**The transcription seam, and its keeper.** The leaf lobe's only implementation is GLSL, and an L1
test cannot run a shader, so the L1 comparison runs against
[`ProductionLeafLobeMirror.h`](../../OloEngine/tests/Rendering/PathTracing/ProductionLeafLobeMirror.h) —
a C++ transcription. That is exactly the substitution
[`substituted-seams-compound.md`](../agent-rules/substituted-seams-compound.md) warns about. It does
not stand alone: `MaterialReferenceAovParityTest` renders
`assets/shaders/tests/ShaderUnit_FoliageTransmissionSweep.glsl`, which **calls** the production
function, over 4096 view/light directions and two authored profiles, and pins the transcription
against it at `2e-3` relative. Neither file is sufficient; the pair is.

---

## 3. Skin diffusion — a semi-infinite searchlight random walk. NONLOCAL

**The reference.** `MaterialReference::SearchlightRandomWalk` — the Monte Carlo transport that
Christensen and Burley's normalised-diffusion fit was *fitted to*. Production evaluates the fit;
this evaluates the thing fitted. Nothing is shared but the medium's definition. The walk's own
correctness is checkable against a closed form: its diffuse reflectance reproduces
`A = 1 - sqrt(1-a) H(1)` with the standard rational `H(1) = 3/(1 + 2 sqrt(1-a))` to within 1% of
reflectance across the band.

**Units and conventions.**

| Quantity | Unit / convention |
|---|---|
| Medium | semi-infinite, homogeneous, **index-matched**, **isotropic** scattering |
| Length | **mean free paths** of the reduced extinction coefficient, `sigma_t' = 1` |
| `ScatterRadiusMM` | the mean free path, in millimetres. Authored as 1 in the tests, so one millimetre is one reference unit and the comparison has no conversion factor to get wrong |
| `ScatterColor` | the **DIFFUSE SURFACE ALBEDO** — see below. *Not* the single-scattering albedo |
| Beam | pencil, normally incident (the *searchlight* configuration) |

Isotropic scattering is not a simplification of convenience: the fit is stated for the
similarity-reduced medium, so the reference has to be in the same reduced form or the two are
different media.

> **The parameter bridge, and the factor of two that hides in it.** `ScatterColor`'s header calls it
> "the fraction of light entering the surface that leaves it again rather than being absorbed" —
> the diffuse surface albedo of the half-space, which is what the shape fit `s(A)` takes. It is
> **not** the medium's single-scattering albedo, and the two are far apart: a surface returning 85%
> of the light needs a single-scattering albedo of 0.997, because a photon has to survive dozens of
> events to get back out. Driving the reference walk with `ScatterColor` directly compares two
> different media and reports the Burley profile as **twice too wide at the median** — which is
> what the first version of this test did, and the error was entirely in the test.
> `SingleScatteringAlbedoForDiffuseAlbedo` inverts it in closed form
> (`u = (1-A)/(1+2A)`, `a = 1 - u²`), and `TheAuthoredScatterColorIsADiffuseSurfaceAlbedo` asserts
> the walk comes back with the authored reflectance rather than trusting the inversion.

**What the comparison measures.**

| Authored `ScatterColor` | Quantile agreement, walk vs fit | Support holds |
|---|---|---|
| 0.35 – 0.70 | within **10%** at the 25/50/75/90% quantiles | > 99% of the walk |
| 0.85 | fit runs **~13% narrow** at the median, ~23% at q75, ~**35% at q90** | **97%** |
| 0.95 | ~24% / ~44% / ~83% | **93%** |

Two things follow, and both land on the channel that matters — the default `ScatterColor` is
`(0.85, 0.55, 0.45)`, so green and blue sit inside the accurate band and **red**, the channel that
carries the visible bleed past a terminator, sits above it:

1. **The fit runs narrow at high albedo and the error grows into the tail.** It tracks the near
   field and loses the tail. The test asserts the *direction* and the *growth across quantiles*
   rather than a value — a scale error would give the same ratio at every quantile, so the growth
   is what says this is a tail effect.
2. **The support radius truncates real transport.** The outermost tap sits at the radius holding
   99.5% of the *profile's* energy, which is 97% of the walk's at an authored 0.85. A bounded,
   deliberate artefact — a head that reads subtly crisper at the terminator than transport says —
   and this is its size.

**The two statements that must not be confused.** `SkinBurleyProfile` is **unit-normalised**: its
integral over the plane is exactly 1 at every albedo. The transport it approximates is not — a
surface authored at 0.35 returns about a third of the beam. So the profile says **where** the energy
goes and nothing about **how much**; the renderer supplies the rest by multiplying the diffuse
irradiance it blurs. The obvious mistake is to read the fit as a reflectance and apply the albedo
twice, which darkens every head by a factor that reads as a lighting change.

**A second approximation the profile comparison cannot see.** The pass is a two-pass **separable**
blur. A separable kernel whose 1D weights are the profile's line-spread function reproduces a
straight **edge** exactly and a **point** not at all — the outer product of two line-spread
functions is not the radial profile, and cannot be, because the profile is not separable. The kernel
*is* unit mass (each 1D pass sums to one by construction, which is why a flat region's brightness
does not change), and its 2D outer product differs from the radial profile by more than 20% relative
at the worst sampled cell. That is a deliberate trade against a 2D gather; this records its size.

**The aligned AOV comparison, and what it found.** `SkinDiffusion.glsl` evaluates no profile — it
consumes a tap table the CPU built — so there is nothing on the device to probe and no rendered AOV
that would add information. The comparison is therefore at the **pass output**: a step edge (a
shadow terminator crossing skin, which is what the whole design is about) convolved with the shipped
taps, against the edge response derived from the walk's radial histogram by the arcsine law.
Alignment is exact by construction: no camera, no exposure, no tonemap.

Measured largest disagreement, as a fraction of a unit step: **0.040** at an authored 0.55, **0.045**
at 0.85. For scale, `SkinDiffusion.cpp` already records that a 17-tap line-spread kernel sits
**0.052** from a true 2D convolution of the *same* profile — so almost all of this is the
discretisation #1241 measured and chose.

The interesting part is what did *not* happen: **the fit's tail error barely reaches the
terminator.** At 0.85 the profile runs 13–35% narrow, and the edge response moves by a tenth of its
error. An edge response is a *cumulative* quantity dominated by where the bulk of the energy sits,
and the fit's error is in the tail, which carries little of it. So the two effects do not add, and
the pass's terminator is dominated by tap discretisation rather than by the profile's accuracy. The
test asserts that as a direction plus a ceiling.

**Conditional renderer axes, settled.**

- **Upscale / non-native resolution — a cell, and it passes.** The pixel radius is a function of the
  target's own height, so the **world** footprint is invariant: doubling the height doubles the
  pixel radius exactly, and 70% resolution gives 70% of the pixels. The kernel itself carries no
  resolution at all — normalised offsets, a millimetre support — so the profile a pixel receives is
  the same object at every resolution. Both halves asserted.
- **Field of view — a cell, and it passes.** Halving the FOV doubles the footprint at a fixed depth,
  exactly.
- **MSAA — not a cell.** Nothing in this slice reads an AOV before a resolve; the references are
  CPU-side and the two GPU probes render to a single-sample RGBA32F target with no resolve in the
  path at all.

---

## 4. Coat transport — a stochastic fibre medium. NONLOCAL

**The finding, stated first.** `GroomCoatShadow::CoatTransmittance` returns `exp(-kappa * tau)` with
`tau = E[crossings]`. The transmittance a fragment footprint actually receives is
`E[exp(-kappa * N)]`. Jensen's inequality puts the second at or above the first, with equality only
when `N` has no variance — so **the shipped form is a lower bound on transmittance: it over-darkens
a disordered coat**, by an amount that is a property of the coat's disorder rather than of any
parameter. Measured largest gap across a `kappa` sweep on a Poisson medium: above 0.02 in
transmittance.

This is a statement about a model, not a bug in an implementation, and #1255 does not ask for it to
be fixed. It asks for it to be measured, so the next person changing coat shadowing knows which
direction the error points and roughly how big it is.

**The reference.** Two things, checking different halves:

- `CoatBundleTransmittance` traces a jittered bundle through a medium this file *generates*, with an
  intersection test written in the test header — so it shares no code with the production
  ray-cylinder routine.
- `PoissonMediumTransmittance` is **analytic**: for a Poisson medium the crossing count is Poisson
  distributed and `E[exp(-kappa N)] = exp(-mu (1 - e^-kappa))` exactly, with no sampling error at
  all. The Monte Carlo bundle is checked against it to `0.04` at 4096 rays, and the medium's
  Poisson-ness is confirmed by `Var[N] / E[N] = 1` to within 15%.

**The control, without which the finding is a coincidence.** A **dense, touching** lattice — fibres
along `+y` at `(x_i, z_j)` with the radius set to half the pitch, so the rows tile the `z` axis and a
ray along `+x` falls inside exactly one and crosses all of its columns. Every ray in the bundle gets
the same count, the crossing variance collapses, and `exp(-kappa E[N])` becomes exact. Measured at a
matched optical depth of 1.2: dispersion `Var/E` below **0.1** against the Poisson medium's **1.0**,
and a relative transmittance gap below **0.005** against more than four times that. If the gap were
an artefact of the estimator rather than of the medium's disorder, it would appear here too.

The two arms are compared at **matched optical depth**, not at matched `kappa`: Jensen's gap depends
on both the variance and the mean, so a fixed `kappa` across media of different densities would
confound them.

> **"Regular" is a property of a medium *and* a ray direction, never of the medium alone.** The
> first version of this control laid the fibres along `+z` and fired along `+x`, so a ray crossed
> either a whole row of forty or none of it. The count was bimodal, its variance was **thirty times**
> its mean, and the "ordered" medium was more disordered than the Poisson one it was controlling
> for. A lattice viewed down its own axis is the worst case, not the best.

**The footprint trap, reproduced independently.** `GroomCoatShadow::ReferenceSettings` documents
that a bundle footprint narrower than the coat's strand pitch reports a confident **zero**. The same
trap is reproduced here against an independent medium and an independent tracer, so the warning is a
property of the *configuration* rather than of that one implementation — which is what a reader
needs before trusting any coat measurement. On a lattice at pitch 0.05, a 0.005 footprint aimed down
a gap reads under half the wide-footprint crossing count, and its own variance is *smaller*, so
nothing in its output says it is wrong.

---

## 5. Reachability, per model and per path

Criterion 3 asks for comparisons across representative parameter ranges and light directions. What
is reachable, and how it is evidenced:

| Model | Reference evaluation | Production AOV | Backends / paths |
|---|---|---|---|
| Fibre | CPU cylinder walk (L1) | `GroomFibreAmbientParityProbe.glsl` — the shipped GLSL's four lobes over 64 light directions, linear RGBA32F (shaderpipe) | GL only. Both GPU probes are fullscreen draws with no camera and no path selection, so forward / forward+ / deferred do not enter; the Vulkan cell is live-only by construction — the headless fixtures need a GL 4.6 context and skip without one |
| Leaf | CPU slab walk (L1) | `ShaderUnit_FoliageTransmissionSweep.glsl` — 4096 view/light directions, two profiles differing in one axis, two controls (shaderpipe) | as above. The lobe has **one** implementation that forward, forward+, deferred and the impostor path all call (`FoliageSurface.glsl`), so one probe covers all four — that is the property #1234 built and this reuses rather than re-establishes |
| Skin | CPU searchlight walk (L1) | none on the device — the pass consumes a tap table, see §3. The pass-output edge comparison is CPU-side | n/a |
| Coat | CPU bundle + analytic (L1) | none — coat transport is a CPU-built representation sampled by the shadow term, not a shading function | n/a |

**Not reachable, and why.** Serialization (`scene YAML`, `asset pack`, `save-game`): this slice adds
no persisted format — the references are test code and the one new asset is a test probe shader.
Scripting (`C#`, `Lua`), physics (`Box2D`, `Jolt`): nothing in scope touches either. Assets
(`loose`, `cooked`): the new probe shader is a test asset under `assets/shaders/tests/` and is not
cooked; no reference *fixture* assets are added, so
`tests/scripts/generate_reference_fixture_scenes.py` is untouched.

**No renderer or shader code changed**, so there is no visual change to verify: this slice reads the
production models and adds references and one test-only probe. The probe's compilation is covered by
the shaderpipe test, which skips cleanly without a GL 4.6 context.

---

## Where the evidence lives

| File | Layer | What it holds |
|---|---|---|
| [`MaterialReference.h`](../../OloEngine/tests/Rendering/PathTracing/MaterialReference.h) | — | the four independent references |
| [`ProductionLeafLobeMirror.h`](../../OloEngine/tests/Rendering/PathTracing/ProductionLeafLobeMirror.h) | — | the leaf lobe transcription, and its keeper named |
| [`FibreScatteringReferenceTest.cpp`](../../OloEngine/tests/Rendering/PathTracing/FibreScatteringReferenceTest.cpp) | L1 | §1 |
| [`LeafTransmissionReferenceTest.cpp`](../../OloEngine/tests/Rendering/PathTracing/LeafTransmissionReferenceTest.cpp) | L1 | §2 |
| [`NonlocalTransportReferenceTest.cpp`](../../OloEngine/tests/Rendering/PathTracing/NonlocalTransportReferenceTest.cpp) | L1 | §3, §4 |
| [`MaterialReferenceAovParityTest.cpp`](../../OloEngine/tests/Rendering/PropertyTests/MaterialReferenceAovParityTest.cpp) | shaderpipe | §5's AOV column |
| [`ShaderUnit_FoliageTransmissionSweep.glsl`](../../OloEditor/assets/shaders/tests/ShaderUnit_FoliageTransmissionSweep.glsl) | — | the leaf sweep probe |
