# Criteria with a judgement word are settled by the frame, not by a proxy

**When an acceptance criterion contains a judgement word — *convincing*, *natural*, *readable*,
*stable to the eye* — a measurement cannot settle it. Write a two-part verdict: the number, and a
plain-words description of what the frame actually looks like. If they disagree, the frame wins.**

The failure this prevents is not laziness. It is the opposite: a thorough, well-evidenced,
entirely sincere acceptance report that measures the wrong thing.

## What happened

Epic #1224's second criterion was *"close grass, shrubs and trees have **convincing** geometry,
light transmission and independent motion"*. It was marked met on three real measurements taken
from committed captures:

- authored meshes cover 39.3% of the frame against the flat cards' 27.2%;
- transmission lifts masked canopy luma from 54.68 to 80.99;
- grass and pines move 11.61% and 1.39% of their own pixels under the same gust.

All three numbers are correct. The author opened every image. Then a human looked at the same
capture and said it looks like 1998 — floating, half-finished, see-through trees — and was right.

**Every one of those measurements is a presence test**: *is the effect switched on?* Presence tests
are what gets optimised toward, because they are cheap, falsifiable and feel like rigour. None of
them can answer *is this any good*.

## The four rules

### 1. Enlarge before judging

The defects were invisible at 960×540 and obvious in a 3× nearest-neighbour crop of a 480 px
region. "I opened the images" is not the same as looking at them. Crop the region the criterion is
about and scale it up:

```python
box = im.crop((x, y, w, h))
box.resize((box.width * 3, box.height * 3), Image.NEAREST).save(out)
```

### 2. Measure the asset, not only the renderer

When a frame looks wrong, the renderer is usually drawing exactly what it was given. On #1224 the
entire appearance came from two asset facts that took one command each to establish:

- `pine.obj` is **48 faces**; `palm.obj` is **57**;
- `grass.png` — the only vegetation albedo in the repository, used for every species including the
  pines — is **9.3% opaque** and passes **17.9%** of its texels at the pine layer's authored
  `AlphaCutoff` of 0.3.

A canopy triangle therefore discarded ~82% of its pixels and drew grass blades in the rest. No
amount of work on LOD, wind, transmission or culling would have changed a pixel of it.

### 3. A richness feature must not REDUCE fine detail

This is the one assertion in this guide that is both objective and ungameable, because it cannot be
satisfied by enabling the feature.

**Fine-detail density** = the fraction of pixels whose luminance gradient magnitude exceeds a fixed
threshold, on an identical crop at an identical output resolution. Real foliage is dense fine detail
at every scale; a low-poly mesh with a stretched texture is large flat regions. On #1224's own
captures:

| subject | mean \|grad\| | pixels > 8/255 |
|---|---|---|
| `grass.png`, a photograph of real grass | 5.37 | 22.2% |
| flat cards canopy, #1233 **off** | 3.60 | 8.3% |
| authored mesh canopy, #1233 **on** | 1.60 | **2.6%** |

Enabling #1233 — whose stated purpose is *"render authored plant meshes up close instead of fixed
quads"* — reduced measured detail to **0.31×**. The feature that exists to add geometric richness
removed it. That is a quality signal a presence test structurally cannot produce, and it would have
failed at acceptance time.

**The gate is in the code now.** `VisualEvidenceGuards.h` owns `FineDetailDensity` (fixed threshold
8/255, forward-difference gradient of Rec.601 luma; a central difference reads zero on a 1 px alternation), `ExpectFineDetailNotReduced` (the A/B, the strong
check) and `ExpectFineDetailFloorAgainstReference` (against a photograph, the weaker check).
`FineDetailConditions.h` snapshots resolution, path, MSAA and the full post-process settings at each
arm's capture, and `ExpectConditionsPinned` fails the A/B if the two snapshots differ. A new
"richness" A/B calls the first and the pinned check; it does not pick its own threshold or crop. The
CPU proof, including the negative controls (a blurred stand-in fails, an identical frame passes,
dither raises the absolute number but not the A/B ordering), is `FineDetailDensityTest.cpp`.

The table above is from the #1224 captures as they were when #1389 was accepted; those PNGs have
since been replaced. On the committed `FloraCloseGeometry_*` frames today the authored meshes carry
MORE detail than the cards (density ratio 1.69 on `FloraCloseGeometry_*`, 2.43 on
`FoliageAuthoredMesh_near*`) and 1.18x `grass.png`, so the gate passes: it is a regression guard
now, not a finding. Swap the arms and it fails, which is the negative control on real frames. One
limit: a count of pixels over a threshold rises when a HARD edge is lightly blurred (the edge widens
before it fades), so it orders fine texture reliably and hard-edged content not at all; compare like
with like (`CaveatHardEdgesWidenUnderLightBlur...` pins this).

Caveats, because a gate nobody can trust is worse than none:

- **Compare only at identical output resolution.** A non-native or upscaled capture scores
  differently for reasons unrelated to quality.
- **Noise scores as detail.** A stochastic-coverage dither raises this number. The A/B form
  survives that (both arms carry the same dither); an absolute floor does not, so measure it on a
  temporally stable frame under mock time.
- **Post-processing moves it.** Bloom, DOF and upscaling all change gradient statistics; pin the
  post configuration.
- It measures *detail*, not *beauty*: it catches "this is a flat triangle", never "this is the
  wrong green".

### 4. Say what you cannot see

If the frame cannot be inspected — no GL context, a live-only cell, an iconified editor — say so
plainly instead of substituting a number that does not answer the question. `CLAUDE.md` already
requires this for rendering changes; a judgement-word criterion is the case where it matters most,
because the numbers are available and look like an answer.

## Related

- [live-verification-noise-floor.md](live-verification-noise-floor.md) — before concluding "it drew
  nothing" or "the change had no effect".
- [foliage-lod-transition-coverage.md](foliage-lod-transition-coverage.md) — the neighbouring trap
  where a conserved quantity makes a plausible metric read backwards.
- Issue #1401 carries the proposed gate and the measurements above.
- [testing-architecture.md](testing-architecture.md): `FineDetailDensityTest` is an L1 CPU test; the A/B calls sit in the L8 evidence tests.
