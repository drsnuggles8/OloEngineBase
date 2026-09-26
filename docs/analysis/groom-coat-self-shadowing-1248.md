# Dense-coat self-shadowing: the comparison, and what it selected (#1248)

Issue [#1248](https://github.com/drsnuggles8/OloEngineBase/issues/1248) asks for a **prototype of
density-volume and deep-shadow options against a dense static coat, selected using quality, cost and
memory evidence**. This document is that comparison and that record.

**The decision: an anisotropic density volume at 64 voxels on the longest axis, marched at three
voxels per step.** A deep opacity map was measured, is competitive on cost, and is **rejected** —
its reason is below and its numbers are kept. An isotropic density volume ships as the declared
lower-cost fallback.

## What is being measured

The quantity is a single scalar per (point, light direction):

> **tau(x, L) = the expected number of fibre crossings** along the ray from `x` towards the light,

from which the coat transmittance is `exp(-tau * (1 - exp(-kappa)))`. It is a purely geometric
quantity — fibre length density times diameter times the sine of the angle between ray and fibre —
and it is deliberately **independent of the pigment**, which #1247 already attenuates inside each
fibre.

> **That transmittance was `exp(-kappa * tau)` when this document was written, and #1360 changed
> it.** `tau` is an *expectation* over the fragment's footprint, so `exp(-kappa * tau)` is the
> transmittance of the mean crossing count where what the footprint receives is the mean of the
> transmittances; Jensen's inequality separates the two and the old form over-darkened a disordered
> coat. Every number below was measured with the old form and is left as measured — the comparison
> it supports is between *representations of tau*, and all of them are evaluated through the same
> transmittance, so the selection is unaffected. See
> [groom-coat-self-shadowing.md](../agent-rules/groom-coat-self-shadowing.md) rule 12.

### The three attenuations, and why they cannot double-count

The issue's scope note forbids double-counting the shadow and transmission terms. There are three,
and they are disjoint by construction:

| # | what | where | owner |
|---|---|---|---|
| 1 | inside one fibre — `exp(-sigma_a * chord)` | `GroomFibreCommon.glsl` | #1247 |
| 2 | between the fibres of one coat — `tau` | `GroomCoatShadow.{h,cpp}` | #1248 |
| 3 | everything else in the scene occluding the coat | the engine's shadow map | #1056/#702 |

(2) is greyscale and sees no colour; (1) never leaves one fibre. The trap is that a groom which is
*also* a shadow-map caster appears in (3) as well as (2), so a strand reading both would be shadowed
by its own coat twice. `GroomCoatShadowTechnique.h` owns that seam.

## The ground truth

Exact ray-cylinder intersection against the real cooked segments, averaged over a deterministic
jittered ray bundle. Fixed-seed integer hashes, IEEE-754, no `<random>` — so these are **not
machine-dependent measurements**, the same property #1246's coverage reference has and for the same
reason.

A curve segment is a **tapered cylinder with flat ends**, never a capsule: `GroomStrandMesh` emits a
square-ended band, and a rounded cap would add occlusion from a shape nothing draws
([groom-strand-visibility.md](../agent-rules/groom-strand-visibility.md) rule 3 records what that
cost the coverage model).

### The footprint is the trap, and it fails silently

A single ray's crossing count is an integer; a fragment covers many fibres, so the truth a shader
wants is the expectation over the fragment's footprint. **A coat is close to a regular lattice, and a
footprint narrower than its pitch aliases against it.** On a 40x40 lattice of 0.6 mm-radius strands
at 5.1 mm pitch, whose analytic answer is 9.36 crossings:

| footprint radius | reference tau | ratio to analytic |
|---|---|---|
| 0.5 mm | **0.00** | 0.00 |
| 2.0 mm | 11.55 | 1.23 |
| 4.0 mm | 8.93 | 0.95 |
| 20 mm | 9.54 | 1.02 |

Below one pitch the reference reports a confident **zero** — a coat that casts no shadow at all — and
raising the ray count does not help, because every ray in a too-small disc misses the same way. Judge
a footprint against the coat's *spacing*, never against its radius. Every measurement below uses a
footprint of four mean spacings.

The brute-force reference is `O(probes x rays x segments)`: 8e9 intersection tests on a
140 000-segment coat, which does not finish. `CoatSegmentGrid` is a uniform grid that changes the
cost and not the answer — a segment is a candidate if its bounding box overlaps a cell the ray
enters, and the same exact test then decides the hit.

## The candidates

| mode | what it stores | per light? |
|---|---|---|
| `None` | nothing; `tau = 0` | — |
| `IsotropicDensityVolume` | fibre areal density per voxel | no |
| `AnisotropicDensityVolume` | the same, plus the voxel's mean fibre direction and its coherence | no |
| `DeepOpacityMap` | light-space front depth + K accumulated-crossing layers warped to start at it | **yes** |

The isotropic and anisotropic volumes come from **one bake** — the direction channel is simply not
sampled by the isotropic arm — so the comparison cannot accidentally be measuring two different
builds.

## Results

Coats are the `GroomStrandFixture` scalp and pelt at 20 000 strands, authored `WidthScale 25` (the
value `GroomFibreCoat.olo` uses), `kappa = 1`. 300 probes taken on actual strands. Error is in
**transmittance**, not optical depth: transmittance is what the shader multiplies into radiance, so
it is what an error is visible in — a tau error of 0.5 is invisible at tau = 20 and is the whole
picture at tau = 0.1.

Every case's reference spans roughly 0.00 to 1.00 with a mean near 0.11–0.17, so the comparison is
**not** running in the saturated regime where every mode agrees at black.

### Best of breed

Each candidate at the resolution its own sweep found best — selecting against an under-tuned
competitor is how a bake-off flatters the answer it started with.

| case | `None` | Isotropic 64³ | **Anisotropic 64³** | Deep map 128²x8 |
|---|---|---|---|---|
| pelt, side light | 0.8317 | 0.0573 | **0.0297** | 0.0515 |
| pelt, top light | 0.8516 | 0.0411 | **0.0236** | 0.0495 |
| pelt, oblique light | 0.8418 | 0.0526 | **0.0263** | 0.0542 |
| scalp, side light | 0.8937 | 0.0207 | **0.0130** | 0.0275 |
| scalp, oblique light | 0.8783 | 0.0181 | **0.0134** | 0.0218 |

Mean absolute transmittance error. Samples per query: volumes ~26–32 march steps at one voxel per
step (see below — three voxels per step is better *and* cheaper), deep map 3 fetches.

**Memory, as the CPU model accounts it:** isotropic 435 KiB (pelt) / 252 KiB (scalp), anisotropic
2.1 MiB / 1.2 MiB, deep map 320 KiB **per light**. Those figures assume the tightest packing each
candidate could use. **What actually ships is one RGBA16F volume, 8 bytes a voxel** — 1.8 MiB for
the pelt at 64³. Until #1445 it was RGBA32F at 16 bytes, because `Texture3D`'s RGBA16F declared 8
bytes a texel while uploading its client data as `GL_FLOAT`, so `SetData` rejected the only buffer
it could be handed; #1445 fixed the upload and halved the volume.

**And the volume is not literally 64³.** `Resolution` sets the voxel count on the LONGEST bounds
axis only; `BuildDensityVolume` derives the other two so the voxels stay cubic. On the reference
pelt that is **59 x 64 x 59 = 222 784 voxels**, which is where the 3.6 MiB comes from. "64³" below
is shorthand for "`Resolution = 64`", not a claim about the grid's shape. The consequence for the selection is stated below: both
volume modes cost the same bytes at runtime, so the isotropic arm is a compute saving, not a memory
one.

### Finding 1: finer is worse, for both representations

This is the result that matters most, and it is counter-intuitive enough that it changed the
selection once it was measured.

Deep opacity map, pelt, side light, 8 layers:

| resolution | mean \|e\| | rmse | bytes |
|---|---|---|---|
| 64² | 0.0522 | 0.0951 | 80 KiB |
| **128²** | **0.0515** | 0.0992 | 320 KiB |
| 256² | 0.0944 | 0.1802 | 1.25 MiB |
| 512² | 0.1709 | 0.2994 | 5 MiB |
| 1024² | 0.2508 | 0.4024 | 20 MiB |

Density volume, same coat, anisotropic:

| resolution | voxel | mean \|e\| |
|---|---|---|
| 16³ | 17.4 mm | 0.0741 |
| 32³ | 8.7 mm | 0.0480 |
| **64³** | **4.4 mm** | **0.0297** |
| 128³ | 2.2 mm | 0.0369 |

**Both optima sit at roughly two strand spacings** (the pelt's mean spacing is 2.0 mm). Below that,
a cell holds one strand or none, so the "average" it stores is a sample rather than an average and
the representation aliases against the coat exactly as the reference's footprint does. Spending more
memory past the optimum does not merely waste it — it makes the shadow worse, by a factor of five in
the deep map's case.

The first run of this comparison used a 512² deep map, which is four times past the optimum, and
therefore scored it at 0.1704 — a third as good as it actually is. **The resolution sweep is what
made the comparison fair.**

### Finding 2: a coarser march is free, and slightly better

Anisotropic volume at 64³, mean absolute error against march step:

| step (voxels) | taps | pelt side | pelt oblique | scalp side | scalp oblique |
|---|---|---|---|---|---|
| 0.5 | ~58 | 0.0305 | 0.0273 | 0.0130 | 0.0136 |
| 1.0 | ~29 | 0.0295 | 0.0265 | 0.0130 | 0.0135 |
| 2.0 | ~15 | 0.0286 | 0.0245 | 0.0130 | 0.0131 |
| **3.0** | **~10** | **0.0254** | **0.0229** | 0.0136 | **0.0122** |
| 4.0 | ~8 | 0.0264 | 0.0265 | 0.0139 | 0.0150 |
| 6.0 | ~5 | 0.0580 | 0.0495 | 0.0138 | 0.0169 |
| 8.0 | ~4 | 0.0904 | 0.1050 | 0.0172 | 0.0202 |

The error is dominated by a systematic **negative bias** (the volume under-shadows), not by
quadrature, so refining the step buys nothing and the coarser step's smoothing happens to cancel
part of that bias. Three voxels per step is at or within noise of the minimum in all four cases and
costs a third of the taps. Past four it degrades sharply on the pelt, which is the denser coat.

## The decision, and the reasons

**Selected: `AnisotropicDensityVolume`, 64 voxels on the longest axis, marched at 3 voxels per step.**

1. **Most accurate in every case measured** — 0.0122 to 0.0254 mean transmittance error, **1.8x to
   2.4x** better than the best deep opacity map. Those errors are the SELECTED configuration's
   (three voxels per step); the best-of-breed table above lists the one-voxel march, whose ratios
   are a little narrower at 1.6x to 2.1x. The shipped number is the one quoted here.
2. **Light-independent.** One bake serves every light, so an animated light costs *zero* rebuilds.
   Acceptance criterion 4 asks that animated light movement not cause flicker or stale density; a
   representation that does not depend on the light cannot go stale when the light moves, which is a
   structural answer rather than a tuning one.
3. **~10 taps per light per fragment** after finding 2, and 1.2–2.1 MiB per coat.

**Rejected: `DeepOpacityMap`,** and the evidence is kept rather than discarded — a confidence-0.5
experiment that rejects an approach is a reportable result.

- It is genuinely cheaper: 3 fetches against ~10 taps, and 320 KiB against 2.1 MiB.
- But it is **per light**. It must be rebuilt whenever the light *or* the coat moves, and with more
  than one shadowing light it costs one map each, and its rebuild cost is paid every frame a light
  animates. That rebuild cost is the decisive one and it is the exact failure mode criterion 4
  names.

  Its memory advantage, to be accurate about it, does **not** invert at three lights: at 320 KiB a
  map against the shipped volume's 3.6 MiB, the crossover is about **twelve** lights. An earlier
  revision of this document said three, which was arithmetic against the CPU model's tighter
  packing rather than against what ships. The map is rejected on rebuild cost and accuracy, not on
  memory.
- And it is **1.8x to 2.4x less accurate** than the shipped volume, at its own optimum.

**Kept as a declared fallback: `IsotropicDensityVolume`.** The same bake with the direction channel
ignored: 1.5x to 2.2x the error, and no direction arithmetic in the march.

A correction worth stating, because the table above does not say it. In the CPU comparison the
isotropic mode needs only the density channel, so it is quoted at one fifth of the anisotropic
mode's memory. **The shipped GPU representation packs both into ONE RGBA volume**, so at runtime the
two modes cost the *same* bytes and the isotropic arm is a compute saving rather than a memory one.
The packing is deliberate and is not a concession: the march is a per-fragment hot loop of about ten
taps per light, so two fetches per step would double its bandwidth for a channel the isotropic arm
does not read — and the sampler namespace had exactly one index left to spend. The memory lever at
runtime is therefore the shadow LOD, which is cubic in the resolution, not the choice of mode.

## Declared approximations

1. **A voxel's fibres are summarised by a mean direction and a coherence.** A voxel holding two
   crossing strands has low coherence and blends towards the isotropic `pi/4`; it cannot represent
   two distinct directions.
2. **The representation contains the groom's own strands and nothing else.** Body-to-hair occlusion
   is **not implemented in this slice** — the strand shader reads no cascade or atlas lookup, so a
   coat in shade is lit as if it were in the open. It is tracked as #1323 together with the
   hair-to-body direction, and the double-count boundary that will matter once both exist is stated
   in `GroomCoatShadowTechnique.h`.
3. **`kappa` is authored, not derived.** How opaque one fibre is to direct light is a coat property;
   deriving it from the pigment would be exactly the double-count the scope note forbids.
4. **Wall-clock cost is not asserted.** The comparison counts taps and bytes, which are
   machine-independent; timings on named hardware belong in the PR body, not in a test.

## Where the evidence lives

- **The comparison, re-run by CI:** `OloEngine/tests/Rendering/PropertyTests/GroomCoatShadowPropertyTests.cpp`.
  Every number above is one of its assertions, so the evidence fails loudly rather than ageing
  quietly.
- **The seam, one reason at a time:** `OloEngine/tests/Groom/GroomCoatShadowSelectionTest.cpp`.
- **The model:** `OloEngine/src/OloEngine/Groom/GroomCoatShadow.{h,cpp}`.
