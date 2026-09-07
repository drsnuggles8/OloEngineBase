# Place a measurement box by a window MEAN, never by the brightest pixel

An A/B visual test that finds "where the feature is" with the argmax of the per-pixel
difference is not measuring where the feature is. It is measuring where the brightest
speck is. On a broad, faint difference field those are different places, and a handful
of stray pixels decides which one you get.

Use the **mean over a window of the size you are about to measure**, found with a
summed-area table so the exhaustive search stays O(W·H). It is the same quantity the
assertion is about, and outliers move it by their value divided by the window area.

Issue #1094 is the worked example. It is the complement of
[persistent-world-space-fields.md](persistent-world-space-fields.md) §4, which says to
measure the sampling bands off a real capture rather than from fractions of the frame:
that rule is right, and an argmax is one more way of guessing.

## The failure, with the numbers

`WaterWakeShapeVisualEvidenceTest.TheHullFootprintFlattensTheSeaBeneathIt` failed
deterministically whenever it was the **first water test in the process** — which is
what `ctest` always does, since every case runs in its own process.

```
Expected: (insideOn) < (insideOff * 0.85), actual: 1.29915 vs 0.51864
```

The A/B was not merely noisy, it was **backwards**: with the wake on the sea inside the
box was *more* textured than with it off, and the issue reasonably read that as a
first-frame state bug in the water fields.

It was not. The hull footprint's difference is hundreds of pixels wide and peaks at
**3–8 out of 255** on a smoothly shaded sea. Eight foam-speck pixels — 0.0009% of the
frame — moved the argmax 63 px, off the footprint and onto its edge:

| box centre | inside on / off | verdict |
|---|---|---|
| (644, 327) — argmax with the specks | 0.691 / 1.097 | passes |
| (581, 305) — argmax without them | 1.299 / 0.610 | fails |
| (608, 357) — highest-mean 90×90 window | 0.744 / 0.937 | passes, in **every** ordering |

Both of the first two numbers are true statements about the same frames. One of the
boxes is on the flattened footprint; the other is on its rim, where the wake's ridge
genuinely *adds* structure.

## The cheap check that settles it

**Re-measure one ordering's frames inside the other ordering's box.** If the numbers
move with the box, the bug is the box, and no amount of renderer archaeology will find
anything.

Three more tells pointed the same way before any code was read:

- the **global mean** of the difference was unchanged to five digits (0.147134 vs
  0.147145) — the feature's total effect is identical;
- the **golden RMSE passed** in both orderings;
- dumping both halves of the A/B showed the two orderings differ at **8 pixels out of
  921,600**, everything else bit-identical.

## Assert liveness on the window mean too

The same test guarded liveness with `ASSERT_GT(stats.m_Max, 4.0f)` on the global
maximum. Measured: **4.33** in one ordering — 8% above its own bar — and 8.0 in the
other. A single-pixel statistic sitting that close to its threshold is a flake already
written down. The window mean over the footprint was 2.90 in every ordering, stable to
three digits.

## The residue, which is real but was not the bug

The first water render in a process genuinely does differ from every later one, by
those 8 speck pixels; `--gtest_repeat=3` shows iteration 1 differing and 2 and 3
identical. It survives resetting all four world-anchored water services, so it is not
field state. It is ~0.001% of one frame and it is gone by the second scene, but it is
not nothing — see #1094 for the open note.

## Reach

Any A/B visual test whose difference is broad and low-amplitude. The high-contrast ones
(foam is near-white against blue) are safe by luck, not by construction: their argmax
sits inside the feature because the feature *is* the brightest thing in frame. Nothing
about the estimator changes; only the margin does.

Related: [persistent-world-space-fields.md](persistent-world-space-fields.md) §4 (where
to sample), [world-anchored-renderer-state-in-tests.md](world-anchored-renderer-state-in-tests.md)
(reset all four water services in the fixture — this file's test reset two, on the way
out only; fixed alongside #1094, though it is not what made the orderings differ: the
residual eight pixels survive resetting all four),
[live-verification-noise-floor.md](live-verification-noise-floor.md) (the A/B exists to
put the noise floor at zero; a fragile box puts it back).
