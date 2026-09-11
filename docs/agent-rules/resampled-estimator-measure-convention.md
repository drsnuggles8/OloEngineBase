# A resampled estimator needs its measure written down, and its Jacobian pinned by an identity

**The rule.** When a renderer reuses a light or path sample across pixels or frames, write the
measure convention into the header that owns the sample, in one place, before writing any of the
resampling code. Then pin the change-of-measure term with an **identity** — two ways of computing
the same estimate that must agree — and add a **negative control** that fails if removing the term
does *not* change the answer. An expected-value assertion on the term itself proves almost nothing,
because the term is a smooth geometric factor and almost any wrong version of it still produces a
plausible image.

This came out of #1140 (ReSTIR DI). It transfers to anything that resamples: ReSTIR GI, ReSTIR PT /
GRIS, a light-BVH cache, a reprojected reflection sample.

**#1169 (ReSTIR GI) is the first place it was applied rather than derived**, and
[design/restir-gi-reconnection-shift.md](../design/restir-gi-reconnection-shift.md) is what that
looks like as a document written *before* the code. Two things it adds that this rule did not
anticipate, both worth reading before the next estimator:

- the **degenerate arm is derived, not chosen**. DI's "a delta light shifts with J = 1" and GI's "an
  environment sample shifts with J = 1" are the same sentence for different reasons, and GI's is
  obtained as the limit of the vertex receding — which is then asserted as a limit, so the special
  case is a consequence rather than a decision somebody could have made differently;
- the shift's **domain** needs guarding separately from its Jacobian. A term can be finite and
  correct at a configuration the shift does not actually cover (a vertex behind the destination's
  surface, or centimetres from it), and the identity in §2 says nothing about those — they have to
  be rejected, not scaled, and rejecting them is a different piece of code from computing J.

## Why the usual checks do not catch it

A resampling estimator has three parts that each look right in isolation:

1. the sample and the measure it was drawn in,
2. the target function the resampling optimises against,
3. the normaliser that turns the survivor into an unbiased contribution weight.

Get any one of them wrong and the output is **still noise-shaped, still smooth, still plausible**. A
missing Jacobian scales the estimate by a factor that varies with geometry: brighter where the
neighbour saw the emitter more edge-on, darker where it saw it more face-on. On a wall that reads as
a soft gradient. Nobody files that. A normaliser applied twice divides the whole image by the
candidate count, which reads as "the new tier looks a bit dark" and gets fixed with an exposure
tweak.

So the failures here are not caught by:

- **a golden image** — it says "did it change", not "is it right", and the first golden is baked from
  the wrong estimator;
- **a unit test on the Jacobian's value** — a hand-picked expected number is derived from the same
  understanding that wrote the code, so a cosine taken at the wrong end is asserted to be correct;
- **agreement between the CPU and GPU implementations** — that proves they agree, and they were
  written from the same misunderstanding.

## What to do instead

### 1. One measure convention, in the header that owns the sample

Two measures meet inside every reuse and they have to be named. #1140's choice, in
`Renderer/ReSTIR/ReservoirDI.h`:

- the sample is a **point on the emitter** (area measure) — never a direction, because a direction is
  only interpretable relative to the shading point that drew it, so a neighbour's direction is a
  *different sample* while a neighbour's emitter point is the *same sample seen from elsewhere*;
- the target function is evaluated in **solid-angle measure** at the shading point, because that is
  the integrand the renderer actually integrates.

The conversion between them is then the one place the Jacobian appears, and every call site can be
checked against a stated rule instead of against intuition.

### 2. Pin the Jacobian by an identity, over randomised geometry

The identity #1140 uses: converting a density at the source point and then applying the shift
Jacobian must equal converting it at the destination directly.

```
AreaPdfToSolidAnglePdf(p, sample, dest) == AreaPdfToSolidAnglePdf(p, sample, source) / J
```

Asserted for 512 randomised, deliberately non-axis-aligned configurations. This is what catches the
two mistakes the term invites — cosines taken at the shading points instead of at the emitter, and
the two squared distances swapped — because both break the identity while both produce a
finite, plausible number.

A second cheap identity: swapping the endpoints must give the reciprocal.

### 2b. Knowing the identity is not knowing where the term goes — write the derivation down

This is the mistake that actually happened while #1140 was being written, *after* the identity above
was already asserted and passing. The identity says

```
p_dest(y) == p_source(y) / J
```

and from that it is entirely natural — and wrong — to divide the target function by `J`. The
Jacobian belongs on the **contribution weight**:

```
W behaves as 1/p(y) in the measure its source density was expressed in, so
    1/p_dest = (1/p_source) * J = W_source * J
and the merged candidate's RIS weight is
    w_i = m_i * pHat_dest(y) * (W_i * J)      with pHat_dest LEFT ALONE.
```

Dividing `pHat` by `J` instead leaves the final estimate off by a factor of `J`. Both versions
satisfy the identity in §2 — the identity constrains the *term*, not its *placement* — and both
produce a plausible image.

Worse, the CPU oracle test had been written from the same understanding, so it divided too and
**passed**. The reason this was caught at all is that the derivation was written out longhand while
documenting the helper; nothing in the test suite as it stood would have flagged it. So: give the
conversion a **named function whose comment carries the derivation** (`ShiftedContributionWeight`),
call it from every site, and derive it in prose before trusting either implementation. A term that
two independently-written implementations agree about is not verified — it is only consistent.

### 3. A negative control, or the arm you tested proves nothing

`ReSTIRDIOracleTest.SpatialReuseWithTheJacobianStaysUnbiasedAndWithoutItDoesNot` runs the merge
twice from the same candidate stream — once with the Jacobian and once without — and asserts **both**
that the correct arm matches ground truth and that the broken arm is measurably worse.

Without the second assertion, a Jacobian that had silently become a no-op (an early `return 1.0f`, a
call site that dropped the division) would pass. The test also asserts, before measuring anything,
that `|J - 1| > 0.05` at its chosen geometry — otherwise the two arms are the same test and the
"negative control" is vacuous.

### 4. Ground truth that cannot share a bug with the sampler

Both oracles in #1140 avoid random numbers on the ground-truth side:

- a **punctual** light set has an *analytic* answer — the sum of the per-light contributions, with no
  sampling error at all, so a systematic bias of one percent is visible;
- an **area** emitter is integrated by *dense stratified quadrature* (512×512 deterministic samples),
  which converges to the same integral the path tracer converges to by sampling, computed the one way
  that cannot share a bug with a sampler.

Comparing a resampler against another sampler is a much weaker check: they can be wrong together.

### 5. Share the sampling density with whatever you are validated against

#1140's scope required the light-sampling PDF to be the path tracer's, "shared rather than
re-derived". This is not tidiness. If the oracle and the tier being measured against it each carry
their own copy of the density, a divergence in the *measure* makes the comparison meaningless in
exactly the case it was supposed to catch. The extraction went into
`OloEditor/assets/shaders/include/LightSampling.glsl`, and the oracle was proved unmoved by
disassembling its SPIR-V before and after and diffing every arithmetic, conversion, bitwise and
control-flow opcode count.

## The two normalisers, and why both ship

The `1/M` normalisation is unbiased only when every combined reservoir *could* have produced the
surviving sample. Spatial neighbours routinely could not — a neighbour facing away from the light has
zero target function on that sample — so `1/M` under-weights the survivor and darkens exactly the
region where reuse helps most.

That makes the bias mode a **user-visible choice**, not an implementation detail, so both modes ship,
both are selectable, and which one ran is **reported** rather than inferred from the settings (the
pass clamps an out-of-range value, so the settings are not authoritative). One shared
`ComputeContributionWeight(weightSum, normaliser, targetPdf)` serves both arms, which is what makes
"normalised twice" a detectable bug instead of a plausible dimming.

## Checklist

- [ ] The measure convention is stated in the header that owns the sample, before the resampling code.
- [ ] The change-of-measure term is pinned by an identity over randomised geometry, not by an expected value.
- [ ] Where the term is APPLIED is derived in prose next to a named helper, because the identity constrains the term and not its placement.
- [ ] A negative control fails if the term is removed, and the test asserts its own geometry is non-degenerate.
- [ ] Ground truth is analytic or quadrature — never another sampler.
- [ ] The sampling density is *shared* with the oracle, and the oracle is proved unmoved.
- [ ] Every normaliser goes through one function, and the mode that ran is reported, not inferred.
