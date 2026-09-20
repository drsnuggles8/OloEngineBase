# Groom representation LOD: the measured comparison (#1252)

Issue #1252 carries `confidence: 0.5`, which `docs/process/issue-scoring.md` §1 reads as an explicit
*spike first* instruction. This document is that spike, and it changed the design twice.

Every number here is produced and asserted by
`OloEngine/tests/Rendering/PropertyTests/GroomLodComparisonTest.cpp`. Re-run it with:

```powershell
build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=GroomLodComparison.*
```

## What was compared, and against what

Three reference coats, each rebuilt with root UVs folded into the unit chart (see *Finding 0*):

| Coat | Strands | Points | Geometry |
|---|---|---|---|
| `human-scalp` | 20 000 | 8 | 25 cm hair, 70 µm, on a 9 cm skull |
| `short-coat` | 30 000 | 4 | 25 mm fur, 110 µm, over a 12 cm body |
| `long-coat` | 20 000 | 6 | 90 mm guard hair, 140 µm, over a 12 cm body |

Each is projected at four apparent sizes (256, 128, 64, 32 px of a 256-px-high frame) and every
representation is rasterised by `GroomCoverage::ReferenceCoverage` at 8×8 samples per pixel. The
statistics are **geometry area ratio** (this representation's covered area over the full coat's —
the *apparent density* claim) and **mean |error| per pixel against the full coat** (the *silhouette*
claim). Colour and highlight response are not measured because they are preserved by construction:
every tier is a curve set drawn by one pass through one shader with one BCSDF.

## Finding 0 — the first run measured the fixture, not the representation

`GroomStrandFixture`'s generators record an unwrapped `phi / 2π` as a root U, which reaches **11 459**
on a 20 000-strand scalp. `GroomCoatClumpCell` *clamps* a UV to ±16 before quantising, so every
strand past the clamp lands in one cell: the first card cook produced **64 cards for 20 000 strands**
and they carried **16 %** of the coat's covered area. It looked like a working level.

Two consequences, both shipped:

- `GroomLodBuilder::BuildCardLevel` **refuses** a groom whose root UVs leave ±16, by name. #1251's
  clumping has the same limitation silently.
- The comparison rebuilds each coat with wrapped root UVs
  (`GroomLodFixture::RebuildWithWrappedRootUVs`) rather than editing #1246's generators, whose
  shapes three committed analyses were measured on.

## Finding 1 — the width compensation is linear, and it works

Thinning to a fraction `k` and widening the survivors by `1/k` restores the coat's covered area.
`FoliageLod::CoverageCompensation` uses `1/sqrt(k)`, because a foliage instance is a sprite whose
area goes as the square of its linear size; a strand is a **band** whose area is length × width, so
the two rules differ by a square root and borrowing foliage's under-compensates a sixteenth-density
coat by 4×.

Geometry area ratio, `human-scalp` (the other two behave the same way):

| Thinning | 256 px | 128 px | 64 px | 32 px |
|---|---|---|---|---|
| ½, uncompensated | 0.641 | 0.644 | 0.648 | 0.650 |
| ½, compensated | 1.000 | 0.999 | 0.997 | 1.023 |
| ¼, uncompensated | 0.378 | 0.383 | 0.390 | 0.389 |
| ¼, compensated | 1.000 | 1.000 | 1.005 | 1.044 |
| ⅛, uncompensated | 0.207 | 0.210 | 0.216 | 0.218 |
| ⅛, compensated | 0.996 | 1.003 | 0.996 | 1.053 |

Note that halving the strands removes only ~36 % of the covered area, not 50 %: per-pixel coverage
saturates, so the strands that are removed were partly hidden behind the ones that stay. The 1/k
widening over-delivers by the same mechanism, and the two cancel to within a few per cent — which is
a result, not a coincidence, and is why the contract is asserted on a *band* rather than at a point.

## Finding 2 — a mean-centreline card is the textbook construction and it is worse

The first card builder averaged each cluster's members into one centreline, resampled to a fixed
point count, carrying their summed width. That is the classic hair card minus the texture. Measured
against keeping the cluster's most central member verbatim and scaling *its* widths to the cluster
total:

| Coat, coarse cell | Cards | Area (mean) | Area (kept) | mean&#124;e&#124; @128 px (mean) | mean&#124;e&#124; @128 px (kept) |
|---|---|---|---|---|---|
| `human-scalp`, 0.05 | 340 (58.8×) | 0.923 | **1.009** | 0.1146 | **0.0977** |
| `short-coat`, 0.05 | 1 200 (25.0×) | 0.548 | **0.963** | 0.2695 | **0.2567** |
| `long-coat`, 0.05 | 1 200 (16.7×) | 0.607 | **1.001** | 0.1553 | **0.1375** |

The kept member wins on both statistics on every coat at every distance, and on the short coat the
average loses **45 % of the covered area outright**. The mechanism is that averaging curves that
diverge produces a shorter, straighter curve: the mean loses exactly the spread that gives a tuft its
silhouette, while a kept strand lies on the coat's manifold by construction. It is also cheaper on a
short coat — it keeps its member's four points instead of resampling to six, so it emits 3 600
segments where the average emits 6 000.

`GroomCardAggregation::MeanCentreline` survives as the measured-and-rejected alternative, the way
`GroomCompositionMode::AlphaToCoverage` does for #1246.

## Finding 3 — the card tier earns its cook above the compensation cap, and only there

The runtime strand budget can do the same arithmetic for free: thin by `k`, widen by `1/k`. So a
cooked level has to beat a free stride **at the same curve count** to be worth a cook, a format
section and resident memory. At a modest reduction it does not:

| `human-scalp` @ 64 px | Curves | Area | mean&#124;e&#124; |
|---|---|---|---|
| card, cell 0.02 (9.3×) | 2 150 | 1.008 | 0.0468 |
| matched strand stride | 2 000 | 0.890 | 0.0503 |

Within noise. But the runtime widening is **capped** — `MaxWidthCompensation`, 8× by default,
because a strand widened sixty times is a flat band rather than a fibre. Past the cap a stride
cannot restore the density at all, and the card's width is *baked*, so the cap does not apply to it:

| Coat @ 64 px, coarse cell | Reduction | Card area | Stride area | Card mean&#124;e&#124; | Stride mean&#124;e&#124; |
|---|---|---|---|---|---|
| `human-scalp` | 58.8× | **1.007** | 0.234 | **0.0740** | 0.2096 |
| `short-coat` | 25.0× | **0.964** | 0.409 | **0.2059** | 0.2545 |
| `long-coat` | 16.7× | **1.006** | 0.618 | **0.0889** | 0.1879 |

That gap is the whole justification for the tier. A cell that only reduces by 4–9× is not worth
cooking; the builder's "no reduction" refusal is a floor, not a target, and the default cell (0.05)
is chosen to land well past the cap.

**One boundary case, stated because it is the shape of the trade.** At **256 px** — above the
default hand-over, so a size the tier is never selected at — the short coat's card keeps the density
(0.957 against the stride's 0.408) but *loses* on per-pixel error (0.325 against 0.281). At that
size a 25× reduction is visibly coarse whichever way it is built, and the error is dominated by
*where* the coverage is rather than by how much of it there is. The deciding assertions are
therefore gated on `pixelSize < GroomLodPolicy{}.CardPixelSize`: asserting the claim at a size the
tier is never used at would be asserting something the feature does not do.

## Finding 4 — the shell ("mesh") tier is refused, on measurement

A shell claims a coverage of 1 everywhere inside the silhouette, so it is only defensible where the
real coat has **saturated**. That is measurable without building a shell: the fraction of the coat's
footprint whose true coverage already exceeds 0.9.

| Coat | 128 px | 64 px | 32 px | 16 px | 8 px | 4 px |
|---|---|---|---|---|---|---|
| `human-scalp` | 0.064 | 0.061 | 0.043 | 0.043 | 0.000 | 0.000 |
| `short-coat` | 0.002 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| `long-coat` | 0.043 | 0.040 | 0.022 | 0.000 | 0.000 | 0.000 |

Mean coverage inside the footprint never exceeds **0.50** anywhere in the sweep, and the highest
solid fraction measured anywhere is **0.064**. These coats do not saturate; they get *less* solid
with distance, because a shrinking coat loses overlap faster than it gains it. A shell would replace
a see-through coat with a solid lump.

So the shell tier is **not shipped**. `GroomRepresentation::Mesh` and
`GroomLodFallbackReason::MeshTierNotSelected` exist so the refusal is a gathered input with a named
reason rather than a missing branch, and
`GroomLodComparison.TheShellTierIsMeasuredBeforeItIsRefused` **fails** if a future coat ever
saturates — which is the correct signal to revisit rather than a reason to hard-code the answer.

The scope boundary asked for shell techniques to be *evaluated* for suitable short coats and for
them not to replace the close-up strand target. They were evaluated; no coat in this engine is
suitable; the strand target is untouched.

## What the measurements do not cover

- **The GPU.** Every number here comes from `GroomCoverage`'s CPU model. It is faithful — it models
  the square-ended band the vertex shader widens, and the one-pixel floor with its compensating
  alpha — but it is still a model. `GroomLodVisualEvidenceTest` ties it to real pixels on all three
  rendering paths, and the live editor session in the PR body covers Vulkan.
- **Motion.** A hysteresis has no meaning in a still frame. `GroomLodContractTest` proves the two
  bounds in arithmetic and `GroomLodVisualEvidenceTest` drives a camera through the real pipeline.
- **A textured card.** A production hair card carries an alpha/tangent atlas baked from the strands
  it replaces, which preserves the tuft's spread through the texture rather than through width. That
  needs an atlas bake and a second shading path, and it fixes the coat's look at bake time — which
  is the thing criterion 1 most wants preserved. Out of scope for this slice and worth its own
  issue if the card tier's silhouette error ever becomes the limiting factor.
