# Foliage LOD transitions: what "no popping" is a measurement of

Three rules for any distance-driven LOD that swaps one representation for another over a
population. Written from issue #1237 (foliage mesh → card → impostor → culled), but none of them is
specific to plants.

## 1. A "ring" is a dispersion, not a count — measure it as one

**Do not assert that decorrelating a threshold reduces how many instances change representation per
frame. It cannot, and a test that claims it does is measuring something else.**

Every instance crosses its threshold exactly once as the camera recedes past it, so the number of
crossings per metre of camera travel is *conserved*. Spreading the thresholds over a band changes
**where the crossing instances are when they cross**, not how many there are.

Measured on the #1237 fixture, over two eyes one metre apart in a 3274-plant layer:

| thresholds | plants flipping | mean distance | **std dev** |
|---|---|---|---|
| one shared distance | 11 | 47.44 m | **0.49 m** |
| spread over 30 m | 39 | 46.69 m | **7.26 m** |

The count went *up*, from 11 to 39. The first cut of the evidence test asserted
`flipsOn * 2 < flipsOff` on the expectation that decorrelation would reduce it, and GoogleTest
printed the comparison as **94 vs 11** — the 94 being twice the flip count that run produced, not a
count of anything. It is the correct answer to the wrong question either way.

A ring sweeping across a meadow is precisely the statement that the flipping instances are all at
the same distance, so they read as one coherent arc instead of as scattered individuals. So the
number to assert is the **standard deviation of the flipping set's distances**: near zero when every
instance shares one threshold, of the order of `spread / sqrt(12)` when they do not.

**A pixel RMSE over a walking camera will not tell you this.** Over a 30 m step the frame-to-frame
RMSE is dominated by parallax — plants changing size and occluding each other — and the LOD
contribution is in the noise. On the same fixture the worst consecutive-step RMSE moved from 95.52
to 94.80 with the feature on: a real improvement, and an unusable assertion. Capture the sweep for a
human to read; assert on the geometry.

## 2. A fade band belongs **above** the keep threshold, never below it

A distance-driven density reduction keeps the instances whose per-instance hash is under a keep
fraction `k(d)`, and has to fade each one out rather than switching it off. Put the ramp on
`[k, k + w)` — opaque below `k`, fading above it.

Putting it on `[k - w, k)` is the obvious first move and it is wrong at the top of the range: when
`k == 1`, which is what every distance *inside* the start distance produces, the highest-hashed `w`
of the population is already partially transparent. The layer is thinned before its density band
begins, and the thinning is invisible in any single frame because it looks like a slightly sparser
meadow.

The assertion that catches it is one line: **inside the start distance, the summed coverage must
equal the instance count exactly.** It failed at 19991.3 of 20000 — a 0.04% error that no screenshot
would have shown, and that would have compounded into every compensation number downstream.

## 3. Compensate the *effective* keep fraction, not the raw one

To preserve apparent coverage while thinning, grow the survivors so the summed area is unchanged.
Coverage is a sum of areas, so the linear growth is `1 / sqrt(keep)` — except that the fading
instances contribute at their fade value, so the fraction actually covered is the **integral of the
fade ramp**, not `k`:

```
u = min(1 - k, w)
effectiveKeep = k + u - u² / (2w)          // k alone over-grows by the whole fade band
compensation  = 1 / sqrt(effectiveKeep)    // clamped; see below
```

The relation to pin is `effectiveKeep * compensation² == 1` over a sweep of `k` and `w`, plus a
population measurement over real hashes — the closed form alone passes with the wrong `effectiveKeep`.

**The cap is not a detail.** At a density floor of `1/64` the uncapped factor is 8x, and an instance
drawn eight times its authored size is a different plant. Past the cap the layer genuinely thins;
say so in the editor rather than clamping quietly. On the #1237 fixture the measured screen coverage
at the far pose was 2.78% full, 1.19% thinned-uncompensated, 2.89% thinned-and-compensated — the
uncompensated arm is the control that makes the third number mean something.

## 4. A transition-smoothing change must not move a scene that did not ask for it

Every authored field defaults to the identity, as the rest of `FoliageLayer` does. That includes the
*dither function*: #1237 replaced a clustering `fract(sin(dot(...)))` hash with interleaved gradient
noise, which is strictly better distributed — and moved the committed `FoliageWind` golden by an
SSIM of 0.011 for a layer that had opted into nothing. Both implementations now live behind the
authored switch. A better default is still a default, and a golden that moves for a scene with no
new keys in it is a regression report nobody asked for.

## Where the code is

- `OloEngine/src/OloEngine/Terrain/Foliage/FoliageLodTransition.h` — the math, and its GLSL twin
  `OloEditor/assets/shaders/include/FoliageLodTransition.glsl`. Every consumer compiles the same
  functions: four vertex stages, three fragment stages and `FoliageInstanceCull.comp`, which drops a
  row exactly where the vertex stages would have drawn it at alpha zero.
- `OloEngine/tests/Rendering/FoliageLodTransitionContractTest.cpp` — the contracts above.
- `OloEngine/tests/Rendering/PropertyTests/FoliageLodCoverageEvidenceTest.cpp` — the dispersion, the
  three coverage arms and the cost, on a live GL context.

## One more trap, cheap to hit

**Source-text contract tests pin whitespace.** `FoliageAuthoredMeshContractTest` and
`FoliageImpostorPlacementTest` search the shader text for `u_Model * vec4(instancePos` and friends.
GLSL is not clang-formatted here, so nothing restores a decorative column alignment — writing
`u_Model     * vec4(...)` to line up a block breaks two tests with a message about the terrain
transform being missing. Keep single spaces around operators in the foliage stages.
