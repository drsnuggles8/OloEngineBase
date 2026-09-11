# ReSTIR GI — the measure, the shift, and the reconnection Jacobian

**What this is:** the design record for ReSTIR GI (issue #1169, #979 Phase 3, second half) —
constrained one-bounce diffuse path reuse. It is written **before** the reuse passes, because #1169
requires that and because the term it exists to pin, the reconnection Jacobian, is *the term that is
silently wrong when the image merely looks plausible*.

It is the GI companion to
[resampled-estimator-measure-convention.md](../agent-rules/resampled-estimator-measure-convention.md),
which is the general rule this follows and which came out of #1140 (ReSTIR DI).

Section numbers are a stable interface — code comments cite them. Do not renumber; append.

---

## §1 What is being estimated

At a primary shading point `x0` with normal `n0`, seen from `wo`, the one-bounce indirect diffuse
term is

```
L_ind(x0 -> wo) = INTEGRAL over the hemisphere H(n0) of
                      f_r(x0, w, wo) * L_o(x1(x0,w) -> -w) * cos(theta0)  dw
```

where `x1(x0, w)` is the first surface the ray `(x0, w)` hits and `L_o(x1 -> -w)` is the radiance
leaving `x1` back toward `x0`.

**The constraint in the issue's title is load-bearing.** `L_o` at the sample vertex is evaluated as
if `x1` were **diffuse**, so it does not depend on the outgoing direction. Two things follow, and
they are the whole reason this estimator is allowed to reuse a neighbour's sample without re-tracing
anything:

1. a stored `L_o(x1)` is valid from *any* shading point that can see `x1`;
2. the vertex's **specular lobe is dropped**, which is a stated non-goal rather than an
   approximation nobody declared — this tier estimates one-bounce indirect *diffuse*. It is still
   counted, so a scene of polished floors producing little GI is attributable. §6.1.

`L_o(x1)` is the **reflected** radiance leaving the vertex, and nothing else:

```
L_o(x1) = (albedo(x1) * (1 - metallic(x1)) / pi)
          * ( E_direct(x1)              one NEE draw from the shared light set, shadow-ray tested
            + E_tail(x1) )              the probe cache, read AT x1 — see §5
```

On a **ray miss** the sample is the environment in direction `w`; `L_o` is the environment radiance
and the sample is stored as a direction rather than a point (§3, §4.3).

### §1.1 Emission at the sample vertex is NOT in it, and that is the whole reason

The obvious term to add is `L_e(x1)`. It must not be there, and the argument is one sentence:
**light that leaves a surface and arrives with no reflection in between is DIRECT lighting**, so it
is the direct tier's by definition. ReSTIR DI samples the emissive-triangle table from `x0` and would
be estimating exactly the same transport — adding `L_e(x1)` here double-counts every emissive surface
in the scene, EXACTLY, with the two estimates agreeing about the answer and the sum being twice it.

The failure mode is the one this whole document is organised against: an emissive-lit room comes out
twice as bright, which reads as "the new GI tier is a bit strong" and gets fixed with an intensity
slider.

It follows that ReSTIR GI is *indirect*: every path it estimates has at least one reflection. What
the clustered tier does with emissive geometry when ReSTIR DI is off — nothing; it glows and lights
nothing — is a pre-existing gap that #1140 filled for its own tier, and it is not this one's to fill
by smuggling a direct term into an indirect estimator.

---

## §2 The measure convention

> **The GI reservoir stores the sample VERTEX `x1` — a point, in AREA measure at `x1` — together
> with the vertex normal `n1` and the direction-independent outgoing radiance `L_o(x1)`. The TARGET
> FUNCTION is evaluated in SOLID-ANGLE measure at the shading point that owns the reservoir.**

```
pHat(x1) = || f_r(x0, w, wo) * L_o(x1) * cos(theta0) ||       w = normalize(x1 - x0)
```

as a luminance-weighted norm, the same scalar `OloReSTIRTargetPdf` uses for DI and for the same
reason: it is the measure the estimator's variance is actually in, and it keeps a saturated blue
bounce from outranking a brighter white one.

**Why a point and not a direction**, which is the one choice everything else follows from: a
direction is only interpretable relative to the shading point that drew it, so a neighbour's
direction is a *different sample*; a neighbour's sample **vertex** is the *same sample seen from
somewhere else*. This is the same argument `ReservoirDI.h` makes for storing a point on the emitter
rather than a direction to it, and it reaches the same answer for a different reason — there, because
the emitter point is a fixed feature of the scene; here, because the vertex is where the path is
*cut*.

The two measures meet inside every reuse, and the conversion between them is the shift Jacobian.

---

## §3 The shift mapping

The **reconnection shift** `S` maps a path generated at a source pixel to a path at a destination
pixel by replacing the first segment and keeping everything from the sample vertex outward:

```
S : (x0, x1, tail)  ->  (x0', x1, tail)
```

`x1`, `n1`, `L_o(x1)` and every vertex beyond are held **fixed**. Only `x0 -> x0'` changes.

`S` is deterministic and, on its domain, bijective — its inverse is the shift with the endpoints
swapped, which is why §7's reciprocal identity is a meaningful check.

**Its domain is not all paths.** `S` is defined only where the reconnection segment `x0' -> x1`
exists as a light-transport path: `x1` must be visible from `x0'`, `x1` must be on the upper
hemisphere of `x0'`, **`x0'` must be on the upper hemisphere of `x1`**, and the vertex must satisfy
§6's admissibility gates. Outside that domain the reuse is **rejected**, never scaled. A shift that
quietly returns something for an out-of-domain input is how light leaks through a wall with a
perfectly smooth falloff.

The hemisphere condition is stated at **both ends** on purpose, and neither end implies the other.
The one at `x0'` is the obvious one: a vertex behind the reusing surface contributes nothing to it.
The one at `x1` is the one a reviewer has to be told about, because two other pieces of the
implementation actively hide it. The bounce stores `n1` oriented toward the pixel that *traced* the
vertex, so the stored normal already points the right way for the source and says nothing about the
destination; and the Jacobian takes `|cos|` at the vertex, so an arriving-from-behind reuse produces
a perfectly finite, plausible number. What is stored at `x1` is `L_o(x1, ->x0)`, the **front face's**
outgoing radiance under the diffuse restriction of §6.1; transporting it to a pixel that can only
see the back face is a one-sided wall leaking light, and a smooth gradient rather than an artefact.

An **environment** sample has no vertex to be behind, so only the condition at `x0'` applies to it.

For an **environment** sample there is no vertex to reconnect to: the shift carries the *direction*
unchanged, which is the correct limit of reconnection at infinite distance (§4.3).

---

## §4 The Jacobian, derived

### §4.1 The derivation

A reservoir's contribution weight `W` behaves as `1 / p(y)` in the measure its source density was
expressed in. Both pixels are expressing the density of the **same fixed vertex** `x1`.

In **area measure at `x1`** the density is one number — it is a density on the surface at `x1`, not
on a hemisphere, so it does not know which shading point is looking. Converting to solid angle at
each shading point:

```
p_w^src(x1) = p_A(x1) * dSrcSq / cosPhiSrc
p_w^dst(x1) = p_A(x1) * dDstSq / cosPhiDst
```

where, for `x` the shading point,

```
d       = || x1 - x ||
cosPhi  = | n1 . ( (x - x1) / d ) |          <-- the cosine at the SAMPLE VERTEX
```

Dividing,

```
1/p_w^dst = (1/p_w^src) * (cosPhiDst / cosPhiSrc) * (dSrcSq / dDstSq)
          = W_src * J
```

so

> **J = (cosPhiDst / cosPhiSrc) * (dSrcSq / dDstSq)**

with **both cosines taken at the sample vertex**. Taking them at the shading points is the
plausible-looking mistake, and it is wrong for the same reason as in DI: the shading-point cosine
belongs to the *integrand* (`cos(theta0)` is inside `pHat`), not to the *measure*.

### §4.2 Where it goes — `J` multiplies `W`, it does not divide `pHat`

The merged candidate's RIS weight is

```
w_i = m_i * pHat_dst(y_i) * (W_i * J_i)        with pHat_dst LEFT ALONE
```

Dividing `pHat` by `J` instead leaves the estimate off by a factor of `J`, and **both versions
satisfy the identity in §7** — the identity constrains the *term*, not its *placement*. This is the
mistake that actually shipped and was caught by prose rather than by a test during #1140
(measure-convention doc §2b). It is restated here because the GI call sites are new code and the
same sentence has to be re-earned.

`m_i` carries the confidence weight, exactly as in DI: `M_i` under `1/M`, and the balance heuristic
(whose numerator already contains `M_i`) under the unbiased mode. Both go through the one shared
`ComputeContributionWeight`.

### §4.3 The environment arm, derived rather than copied

Let `x1 = x0 + t*w` and take `t -> infinity` with `n1` opposed to `w` (a distant, direction-only
sample). Then `dSrc/dDst -> 1` and `cosPhiSrc, cosPhiDst -> 1`, so

> **J = 1 exactly for an environment sample.**

This is the GI analogue of DI's delta-light arm, and it is a fact about the limit, not a shortcut.
Implementation consequence: an environment sample stores its **unit direction** in the position lane
(the same convention `LightSampleKind::Directional` uses in DI — which reading applies comes from the
sample kind and never from inspecting the vector) and its normal lane is the "no normal" sentinel.

---

## §5 The DDGI hand-off, stated so it can be tested

#979's non-goal is that ReSTIR GI and DDGI **must not double-count**. The rule:

> **The probe cache is read at exactly ONE vertex per path. Enabling ReSTIR GI moves which vertex
> that is; it does not add a second read.**

| | probe cache read at | diffuse ambient at `x0` comes from |
|---|---|---|
| ReSTIR GI **off** | `x0` — the ambient ladder in `ComputeDeferredLit` | the ladder: lightmap, else probes/DDGI, else sky irradiance |
| ReSTIR GI **on** | `x1` — inside the GI initial-sample draw, as `E_tail` (§1) | the resolved ReSTIR GI radiance |

**ReSTIR GI owns the WHOLE diffuse ambient when it is on**, not just the probe rung. That is forced
rather than chosen: a bounce ray that escapes collects the environment, so the sky's diffuse
contribution is already inside the resampled estimate, and leaving the ladder's sky-irradiance rung
on underneath would count it twice. The same applies to a baked lightmap, which is itself a complete
diffuse-GI solution.

**And the estimate is not multiplied by the ambient-occlusion term.** `ComputeDeferredLit` computes
`ambient * ao`; ReSTIR GI's occlusion is already exact, because it traced the rays. Multiplying a
ray-traced visibility estimate by a screen-space approximation of the same visibility darkens every
corner twice — and it darkens it by a factor nobody can attribute, because both halves look
plausible. AO keeps multiplying the SPECULAR half of the ambient, which ReSTIR GI does not touch.

So DDGI remains exactly what #979 calls it — *fallback / cache / lower tier* — and it keeps supplying
bounces 2 and beyond, which a one-bounce estimator cannot. The two terms live at different vertices
of the same path, which is why composing them is not double-counting and why turning the tail off is
a *quality* setting rather than a correctness one.

Three consequences, all asserted by `ReSTIRGIContractTest` against the pure function
`SelectIndirectDiffuseSources` in `ReSTIRGITechnique.h`:

1. exactly one of `DDGIAtPrimary` / `ReSTIRGIAtPrimary` is true in any frame — never both, never
   neither;
2. `DDGIAtSecondary` is only ever true while `ReSTIRGIAtPrimary` is, because there is no secondary
   vertex otherwise;
3. **SSGI stands down** when ReSTIR GI owns the term. SSGI composites a *third* estimate of the same
   integral on top of the lit colour (`PostProcess_SSGIComposite`), so leaving it on is a straight
   double count. The stand-down is counted and reported, not silent.

The **specular** half of the ambient ladder — prefiltered IBL and reflection probes — is untouched.
ReSTIR GI is a diffuse tier and says so; a tier that quietly ate the specular ambient would darken
every metal in the scene by an amount an exposure tweak hides.

---

## §6 The three ways GI's shift is not DI's

The formula in §4.1 is the same expression as `ReservoirDI.h::ShiftJacobian`. That is not a licence
to rename the DI chain — #1169 is blunt that it is not — and the reason is that **the formula is the
only part that transfers**. Each of the following is a piece of code with no DI counterpart, and each
is invisible in a still frame when it is missing.

### §6.1 The sample vertex has a BSDF; an emitter does not

DI's stored radiance is direction-independent *by construction*: the emitter's own cosine, texture
and cone are folded in at selection time and the result is what leaves that point toward anyone.

GI's is direction-independent only because **the definition in §1 makes it so** — the vertex is
shaded with its diffuse lobe alone. Evaluating the full closure at `x1` toward `x0` would make the
stored value depend on where `x0` is, and reusing it at a neighbour would transplant a view-dependent
highlight onto a pixel that is not at that view: a smear of extra light following the camera, which
reads as *plausible indirect specular*.

So the restriction is a **definition, not a gate**, and that distinction matters for what the code
looks like. There is no roughness threshold below which a sample is rejected — rejecting one would
leave a hole in the estimate, which is worse than the term it was avoiding. What there is instead is
a **count**: `GlossyVertexBounces` records the pixels whose bounce landed on a vertex rough enough
(below `kDefaultGlossyVertexRoughness`) that the dropped specular lobe is a visible fraction of what
left it. A chrome-floored scene legitimately produces far less GI than a full path tracer would, and
"the tier looks like it is off" has to be attributable to that rather than guessed at.

### §6.2 The reconnection segment is a NEW segment, and it can be occluded

DI's spatial reuse traces **no rays at all**: the reused emitter point is the same point in space,
and the destination pixel's own resolve ray tests it.

GI's reconnection introduces a segment `x0' -> x1` that **never existed in the source path**. A wall
between the destination pixel and the neighbour's sample vertex was never on the source path and
nothing in the source reservoir knows about it. Reuse without a visibility test leaks light through
that wall, and — because reuse is spatially coherent — leaks it as a *smooth gradient*, which is the
shape nobody reports.

So the resolve traces **one reconnection ray** on the surviving sample, at the pixel that will
actually shade with it. That is the same ray budget DI's resolve spends and for the same reason.
Per-neighbour reconnection rays during spatial reuse are a separate, off-by-default setting: they cost
`k` rays per pixel and buy a reduction in the residual leak, and the fact that the default arm has a
residual leak is reported rather than hidden.

### §6.3 The near field, where `dDstSq` is in the denominator

DI's emitter is metres away from both shading points, so `dSrcSq / dDstSq` is a gentle ratio and a
sub-pixel move of the shading point changes it by nothing.

GI's `x1` can be **centimetres** from `x0'` — a corner, a contact shadow, the inside of a fold. There
`dDstSq` in the denominator makes `J` explode, temporal reuse keeps the resulting firefly alive for as
long as the M cap allows, and the artefact is a persistent bright speck in exactly the concave
geometry GI exists to render.

So there is a **minimum reconnection distance** below which the reuse is rejected rather than scaled,
alongside the minimum-cosine guard DI already has. It has no DI analogue because DI has no near field.

**And it is why GI's TEMPORAL reuse computes a Jacobian where DI's is licensed to use 1.** DI's
temporal `J` is exactly 1 because the #976 validity test establishes that last frame's shading point
*is* this frame's — and DI's Jacobian is insensitive to the sub-pixel difference the reprojection
leaves behind. GI's is not, for the reason in the paragraph above. So the temporal draw reconstructs
the previous shading point (from the reprojected UV, the previous frame's view depth in the surface
history plane, and the previous inverse view/projection carried in the parameter block) and computes
the real term.

That is also why the GI parameter block carries a previous-frame inverse view-projection while the DI
one deliberately does **not**: DI's was declared, never read, and removed as 64 bytes of dead uniform
describing a fallback that did not exist. GI's is read by the temporal Jacobian on every frame that
reuses history, and a change that makes it dead should delete it the same way.

---

## §7 How the term is pinned

Per the measure-convention rule, an expected value on `J` proves nothing. What the tests assert:

1. **The measure identity**, over randomised, deliberately non-axis-aligned geometry: converting an
   area density at the source and then shifting must equal converting it at the destination
   directly —
   `AreaPdfToSolidAnglePdf(p, x1, dst) == AreaPdfToSolidAnglePdf(p, x1, src) / J`.
2. **The reciprocal identity**: swapping the endpoints gives `1/J`.
3. **The environment limit**: `J -> 1` as the sample recedes, and is exactly `1` for a sample stored
   as a direction.
4. **A negative control** that fails if the Jacobian stops doing work — the same merge run twice from
   one candidate stream, with and without the term, asserting both that the correct arm matches
   ground truth *and* that the broken arm is measurably worse, having first asserted its own geometry
   is non-degenerate (`|J - 1| > 0.25`).
5. **Ground truth that cannot share a bug with the sampler**: the one-bounce integral at `x0` by
   dense stratified **hemisphere quadrature** over an analytically-shaded Lambertian occluder, no
   random numbers on the ground-truth side.
6. **The hand-off**, as an exclusivity assertion on `SelectIndirectDiffuseSources` (§5).

`ReSTIRGIOracleTest` owns 1–5 and runs headless on every CI runner. `ReSTIRGIContractTest` owns 6 and
the GLSL/C++ constant agreement. `ReSTIRGIReservoirGpuParityTest` drives the packing through a real
RGBA32F target, because the failure that shipped in #1140 was a **store** that destroyed the value
while both sides' arithmetic was right.

---

## §8 What is shared with DI, and what is forked

#1169 asks for `Reservoir.glsl` to be shared "where the shape is genuinely shared" and forked where it
is not. The split, and the test for it: a piece is **shared** when it contains no reference to what
the sample *is*.

| Shared — `include/ReservoirCore.glsl` / `ReSTIR/ReservoirCore.h` | Forked |
|---|---|
| streaming RIS update (`xi * WeightSum < weight`, and why it is spelled that way) | the sample struct and the reservoir struct |
| `ContributionWeight`, the two bias modes, `FinalizeInitial`, `FinalizeCombined`, `ApplyMCap` | the target function |
| the balance-heuristic weight | candidate generation and its source pdf |
| `RoundToInteger`, the octahedral normal pack/unpack, the exact-integer limit | the plane layout and what rides in each lane |
| the **geometric core** of the reconnection Jacobian — a fixed vertex, two shading points | the degenerate arm: delta light (DI) vs environment (GI), §4.3 |
| the minimum-shift-cosine guard | admissibility, reconnection visibility, minimum distance (§6) |

The geometric core is shared because §4.1's derivation *is* §4.1 of the DI header's derivation with
"emitter point" replaced by "sample vertex" — the shift holds one vertex fixed and re-expresses a
solid-angle density at a moved shading point, and neither derivation looks at what the vertex is. The
domain wrappers are forked because the degenerate arm is exactly where they differ, and folding both
into one function would mean a `kind` enum that spans two layouts.

Extracting the core moves no DI behaviour: `Reservoir.glsl` and `ReservoirDI.h` keep every name they
export and forward to the core.

---

## §9 Engagement — and why DI's criterion has no GI analogue

`ReSTIRDIEngagePredicate` stands the DI tier down when the emitter set does not exceed the per-pixel
candidate budget, because below that RIS with `M >= N` **enumerates the same set the clustered loop
already walks**, and pays four passes to do it.

**There is no GI instance of that argument.** DDGI is not an enumeration of the same integral that
resampling would duplicate — it is a *coarser cache* of it, at probe resolution, with a different
error characteristic. There is no scene size at which the ReSTIR GI estimator degenerates into the
DDGI one, so there is no count to compare. Inventing one would be the rename this issue warns
against, dressed as a policy.

**And it is deliberately not a variance threshold**, for the reason DI states: a variance threshold
reads last frame's noise, so it hunts — the tier engages, the noise drops, it disengages, the noise
returns, and the image flickers between two estimators at a few Hz. Variance stays exposed as
something to *look at* (the Variance debug view), never as something to switch on.

What is left is one genuinely measured stand-down, and it is a property of the **scene**:

> **`NoIndirectSourceInScene`** — no live light of any type, no emissive triangle, and no environment
> source. Then there is nothing for one bounce to gather, every reservoir would carry zero, and
> two rays per pixel would buy a black image that the ambient ladder already produces for free.

Note that unlike DI this counts **directional** lights: the sun bouncing off a floor is exactly the
indirect light this tier exists for, whereas DI excludes directional lights because the clustered loop
keeps them for their cascades, their mask channel and their cloud shadow. Same word, different set —
which is why `ReSTIRGIEngageInputs` is its own type and not DI's.

Every other stand-down is a **capability** gate (no RT device, no TLAS, no G-Buffer, no shaders, no
GPU Scene, no targets, a history at a different layout version), each counted and each reported once
per change.

---

## §10 Staleness: two independent bounds

A GI sample carries radiance that was correct for the scene *at the frame it was traced*. If the
object at `x1` moves, or the light lighting `x1` changes, the receiving surface at `x0` has not
changed at all — so the #976 validity test, which only looks at the receiver, **accepts** the reuse.
That is not a bug in #976; it is a question #976 was never asked.

So staleness is bounded twice, and the two bounds do different jobs:

- **the M cap** bounds a stale sample's *weight* — how much of the pixel's estimate it can still
  claim (`ApplyTemporalMCap`, shared with DI);
- **the age cap** bounds its *existence* — a sample older than `MaxSampleAge` frames is dropped from
  the temporal merge outright.

The age is carried in the reservoir and is part of the **lineage** #1169 asks to stay inspectable: a
`SampleAge` debug view answers "is this region lagging or is it converged" from the picture, which no
amount of looking at the radiance can.

---

## §11 Motion

#979 asks for quality demonstrated **during** motion, so the three events are named here and each is
handled by a different mechanism — a still-frame comparison discharges none of them.

- **A camera cut** drops the whole history through the temporal-history registry: the frame is one
  bounce per pixel plus spatial reuse. Noisy, and correct. Explicit, not emergent.
- **Disocclusion** is per-pixel: the reprojected UV lands on a different surface, #976 rejects it,
  that pixel restarts while its neighbours keep their history, and spatial reuse is what refills it
  from the survivors.
- **A light or an object moving under a still camera** passes #976 — the receiver did not change —
  and is bounded by §10's two caps. This is the case an age cap exists for.

---

## §12 Checklist for a change to this estimator

- [ ] The measure convention (§2) still holds at every call site that touches a sample.
- [ ] The Jacobian is on the contribution weight, never on the target function (§4.2).
- [ ] Every new reuse path rejects out-of-domain shifts rather than scaling them (§3, §6).
- [ ] The probe cache is still read at exactly one vertex per path (§5), and the exclusivity test
      still covers the new configuration.
- [ ] A new bias or clamp is **counted** and has a debug view, not a comment.
- [ ] The negative control in `ReSTIRGIOracleTest` still fails when the term is removed.
