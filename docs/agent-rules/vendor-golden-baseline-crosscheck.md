# Cross-checking a per-vendor golden baseline set

**Read this before baking a new `assets/tests/{golden,visual}/<vendor>/` set, before
trusting one that already exists, and before concluding that a cross-vendor pixel delta is
"just driver variance".**

A golden makes the nightly green. It does not establish that the render is correct — and
once baked, the nightly **actively defends** whatever was captured, including a defect.
A per-vendor set makes that worse, because it is baked on hardware the reviewer does not
have, and from then on that vendor validates itself.

The precedent is real. On the branch that baked the AMD set, integer-format textures were
sampled with `GL_LINEAR`; GL requires `NEAREST` for integer formats, so on Mesa the texture
was incomplete and read as all-zero through `texelFetch`. **Every glyph in the engine was
invisible on AMD** — while the font loaded (191 glyphs), `SlugFontProcessor` packed it
(189 glyphs), `DrawString` emitted 852 quads across 4 draw calls, and nothing logged an
error. Had the baselines been baked before that fix, a blank UI would now be the reference,
and the delta that eventually exposed it would have read as *the regression*.

---

## 1. Why RMSE cannot settle it, in both directions

The instinct is to diff the two vendors' PNGs and call a small number "agreement". That is
half an answer at best, and it fails both ways:

- **A small RMSE does not mean correct.** If both vendors execute the same wrong shader,
  they agree perfectly. Agreement measures *portability*, never *correctness*. The glyph
  bug above would have produced two sets that disagreed loudly — but a bug in shared
  vendor-neutral code produces two sets that agree loudly and are both wrong.
- **A large RMSE does not mean defect.** The captures contain features whose pixels are
  decided by depth ties and by float comparisons at the visibility threshold. Those move
  under any last-bit difference, and they can dominate an aggregate metric while nothing
  about the render is wrong.

So the aggregate number is a *screening* tool. It tells you where to look. It never
delivers the verdict.

## 2. Measure the noise floor first — before attributing anything

Two runs of the **identical binary on the identical GPU** do not produce identical PNGs
here. `docs/process/task-loop.md` records `VirtualGeometry_Debug_ClusterId` moving by
158/255 and `Fluid_Waterline` by 80/255 between two runs of one binary.

**A delta at or below that floor is not a vendor difference — it is the same GPU
disagreeing with itself.** Capture the reference vendor twice into two scratch directories
and diff *those* first. Every later number is read against that floor, and a cross-vendor
delta smaller than it carries no information at all.

This is the same discipline as
[live-verification-noise-floor.md](live-verification-noise-floor.md), applied to goldens.

## 3. Separate vendor difference from temporal drift

A committed vendor set was baked at some commit. `master` has moved since. Comparing
`AMD@old` against `NVIDIA@HEAD` measures **vendor + every renderer change in between**, and
attributing the whole delta to the vendor is simply wrong.

Never rebake into the shared baselines to "make them comparable" — that destroys the
evidence. Decompose instead, with one fresh capture of the reference vendor at `HEAD`:

| comparison | isolates |
|---|---|
| `N1` vs `N2` (two fresh runs, same vendor, same commit) | **noise floor** |
| `N1` vs the committed shared baselines | **temporal drift** on the reference vendor |
| `N1` vs the committed vendor set | vendor **+** drift — the headline number |
| committed vendor set vs committed shared set | vendor **+** drift, recordings only |

If the drift row is at the noise floor, the shared baselines are current and the headline
row is very nearly pure vendor difference. If the drift row is large, the headline row is
mostly drift and says little about the vendor — go and bound the drift before judging.

**Also check that the fixture itself did not move.** If the test's scene, camera, or
thresholds changed since the bake, the two sets are not images of the same thing and no
amount of arithmetic repairs that:

```bash
git diff <bake-commit> HEAD -- <the test .cpp> | grep -E '^[+-]' | grep -vE '^[+-]{3}|^[+-]\s*//'
```

## 4. The property audit is the oracle that actually answers "is it correct?"

Since #734 each golden also asserts a property derived from its own readback — Reinhard
never saturates, FXAA's blends are complementary, PCF leaves a penumbra, each splatmap edge
carries its own layer's signature, night is much darker than noon. Those guards are
**vendor-independent statements about what the effect must be doing**, so they can be
evaluated against a *recorded PNG*, not only a live frame.

That is the second oracle, and it is the one that would have caught the glyph bug.
`OloEngine/tests/Rendering/PropertyTests/GoldenBaselineAuditTest.cpp` (L9) does exactly
this: it runs those guards over every committed baseline set — the shared one and every
vendor subdirectory, **discovered rather than listed** — with no GPU, so it gates every CI
run rather than only the nightly that owns that hardware.

**A guard failing on a vendor baseline is far stronger evidence than any RMSE delta**: it
means that recording froze a defect, and the fix is a rebake plus a bug, not a threshold.

The audit deliberately does **not** mirror the splatmap's first guard, which compares the
blended RGBA16F intermediate against a CPU-authored `Σ wᵢ·layerᵢ`. A recorded 8-bit PNG does
not carry that intermediate. What survives the tone map is the per-edge layer signature, and
that still catches the swizzle / layer-off-by-one the guard exists for.

## 5. Localize a residual geometrically before classifying it

When a delta survives the noise floor, do not classify it from the number. Write a
per-pixel max-delta heatmap and **look at it**. The spatial pattern names the cause far
faster than any statistic:

- **Thin lines converging at the horizon, with clean silhouettes where scene props occlude
  them** — the `InfiniteGrid`. Its coplanar depth bias decides these pixels by a depth tie,
  so *any* last-bit difference flips which primitive wins.

  **This pattern tells you WHICH feature moved, never WHY.** In the #735 cross-check it was
  the single most misleading signal: comparing the AMD set against the committed shared
  baselines produced a textbook grid-line heatmap that read as obvious cross-vendor
  depth-tie variance — and it was nothing of the kind. It was **drift in the stale shared
  baselines**, and the actual vendor difference on the same frame was *five pixels*. Both
  vendors had moved away from a two-month-old recording, together. Only the decomposition in
  §3 separates those two readings, and the heatmap cannot.
- **Isolated bright specks scattered through the sky band** — stars at the visibility
  threshold. Benign *if* the positions coincide; see below.
- **A broad, smoothly varying region** — lighting, fog or tone mapping. Not benign; this is
  the shape a real divergence takes.

For a star field specifically, "did the positions move?" is directly measurable and is a much
sharper question than brightness RMSE: threshold both sky bands into star masks and compare
them as *sets*. Sweep the threshold so the answer is not an artifact of one cutoff, and
include a control — shifting one mask by a single texel shows what genuine relocation costs,
which is the scale a real hash divergence would exceed.

## 6. Mechanism reference — the things that are easy to get wrong

- **The vendor knob is a CLI flag, not an environment variable.** `--olo-golden-vendor=<name>`
  and `--olo-golden-rebase`, parsed in `OloEngine/tests/TestOptions.cpp`. The env vars
  `OLOENGINE_GOLDEN_VENDOR` / `OLOENGINE_GOLDEN_REBASE` were removed; anything still quoting
  them is stale.
- **Only the `=value` form parses.** `--olo-golden-vendor amd` (space) is a hard error
  (`"option needs a =value"`), *not* a silent fallback. That is deliberate — a silent
  fallback under `--olo-golden-rebase` would overwrite the shared baselines.
- **Two independent golden mechanisms scope by vendor**, and they are separate code:
  `GoldenImageTests.cpp::GoldenBaselineDir` → `assets/tests/golden[/<vendor>]`, and
  `AtmosphereVisualEvidenceTest.cpp::GoldenBaselineDir` → `assets/tests/visual[/<vendor>]`.
  A change to one is not a change to the other. The value is whitelisted once at parse time
  (`RequirePlainPathComponent`) precisely so a third consumer cannot miss the rule —
  `--olo-perf-machine` is a third such path component (`perf_history/<machine>.tsv`).
- **The working directory is `<repo>/OloEditor`**, which is why every baseline path is
  spelled `assets/...`. Running the binary from the repo root makes every golden "missing".
- **`<name>.actual.png` / `<name>.diff.png` are generated L10 escalation artefacts**, written
  next to the baseline when a compare fails. They are `.gitignore`d. Eight of them were once
  committed into `assets/tests/golden/`, byte-identical to the baselines beside them.
- **A vendor directory that does not exist is not an error** — `--olo-golden-vendor=llvmpipe`
  is passed by the nightly with no `golden/llvmpipe/` present, by design ("maintainers promote
  a set by committing PNGs into that directory"). The consequence is worth knowing: **that
  nightly currently compares no goldens at all.**

## 7. Never rebake into the shared set to close a cross-check

The whole method depends on the committed sets staying untouched while you judge them.
Capture into a scratch vendor directory (`--olo-golden-vendor=nvidia-crosscheck-run1`) and
delete it afterwards. And never `git add -A` after a suite run: a full run dirties ~10
tracked PNGs under `assets/tests/visual/` that have nothing to do with your change.

---

## 8. Result of the #735 cross-check

Run 2026-08-17 on the RTX 4090 dev box, against the AMD/Mesa set committed in `5d776ba2`
(2026-08-01). **The AMD set was baked from an unchanged fixture** — the only diff to
`AtmosphereVisualEvidenceTest.cpp` since is the env-var→CLI-flag migration, so the two sets
are images of the same scene, and `AtmosphereSky.glsl`'s GL path is untouched (the changes
since are a Vulkan-only `#ifdef` branch and comments).

**Verdict: all 17 AMD baselines stand — no rebake, no defect, and the AMD/NVIDIA agreement is
tight enough that per-vendor scoping is no longer buying anything.** The one thing that did
turn up needing a rebake is the *shared* Atmosphere set, which has drifted from HEAD; see
"What to do about it" below.

### The four renderer goldens

`fxaa_hard_edge` is **byte-identical** across vendors. The other three differ by
**max |delta| = 1 LSB**, RMSE 0.0006–0.0016 in the test's own normalised units — against a
`kRmsePassBelow` of 0.004 and a `kRmseFailAbove` of 0.02. That is last-bit rounding with a
2.5× margin on the *pass* gate.

### The thirteen Atmosphere captures — and the trap in reading them

The decomposition of §3, on a fresh NVIDIA capture at `d780bd6e4`:

| comparison | isolates | result (RMSE, 0..255) |
|---|---|---|
| run 1 vs run 2 | **noise floor** | **≤ 0.30**; 13 of 17 byte-identical |
| fresh NVIDIA vs committed shared | **temporal drift** | goldens **byte-identical**; Atmosphere day cells **0.87–2.60** |
| fresh NVIDIA vs committed AMD | **vendor** (+ negligible drift) | **0.10–0.72** |

**Read the last two rows against each other, because the naive comparison inverts them.**
Diffing the committed AMD set against the committed shared set gives 0.23–2.59 and a
textbook grid-line heatmap — which reads as cross-vendor depth-tie variance and is not.
Fresh NVIDIA sits *closer to AMD* (≤0.72) than to the shared baselines (≤2.60). Both vendors
have drifted away from a shared set last rebaked 2026-07-16, **together**; the AMD set, baked
2026-08-01, is simply the more recent recording of the two.

On the worst cell (`DuskClear`), the same frame compared both ways:

| | pixels with any channel delta > 4 | distinct 8×8 blocks | max delta |
|---|---|---|---|
| vs stale shared baseline (**drift**) | 11 718 / 921 600 | 560 | 70 |
| vs AMD baseline (**vendor**) | **5** / 921 600 | 4 | 39 |

Five pixels. The noise floor is ~0 and the vendor term is ~11× inside the test's 8.0
threshold. The physical content agrees even more tightly: across all 13 cells × 3 bands the
worst band-luminance disagreement between vendors is **0.43 / 255**.

### The star field — the claim only a cross-check could settle

`f4fef24b` replaced `fract(sin(dot(p,k)) * 43758.5453)` with an integer PCG hash because the
old one placed stars in *genuinely different locations* on NVIDIA and Mesa. Whether the
replacement is bit-exact cannot be measured from one vendor. It is:

| threshold above the night-sky floor | star pixels (AMD / NVIDIA) | positional agreement |
|---|---|---|
| +5 | 60075 / 60042 | 99.92% |
| +15 | 16749 / 16733 | 99.74% |
| +25 | 3836 / 3819 | 99.45% |
| +90 | 804 / 804 | 99.75% |

Robust across every threshold, so it is not an artifact of one cutoff. For scale, shifting
one mask by a **single texel** drops agreement to 88% — a genuine hash divergence would be
far below that. The residual ~20 pixels are stars sitting exactly on the brightness cutoff.
`NightOvercast` / `NightStorm` contain zero star pixels: the cloud deck occludes the sky, as
intended.

`AtmosphereSky.h`'s claim — positions bit-exact, brightness carrying cross-vendor ULP
variance — is confirmed.

### The property audit

All **23** guards pass on the AMD recording, and all 23 on the shared set, with the derived
quantities agreeing to three significant figures (shadow attenuation 0.6162 vs 0.6190; night
sky luma 58.00 vs 57.99; dawn warmth 1.0534 vs 1.0534). Nothing in the AMD set is a recording
of a broken frame.

`GoldenBaselineAuditTest` was itself checked against deliberately-broken baselines before
being trusted — a saturated ramp, an unblended FXAA edge, a hard-thresholded shadow, a
swizzled splatmap — and each fired with the diagnostic naming that defect class, while the
real sets passed in the same run.

### What to do about it

1. **The four renderer goldens: vendor scoping is unnecessary.** Fresh NVIDIA@HEAD is
   byte-identical to the shared set and AMD differs by 1 LSB, against a `kRmsePassBelow` gate
   twelve times larger than the observed delta. `assets/tests/golden/amd/` can go.
2. **The thirteen Atmosphere captures: vendor scoping is also unnecessary** — vendor ≤0.72 on
   a noise floor of ≤0.30 against a threshold of 8.0. **But rebake the shared set at HEAD
   first**, because it currently carries 0.87–2.60 of accumulated drift that would otherwise
   be charged against AMD's budget for no reason.
3. Dropping the scoping is **not** just deleting the directories — `gpu-conformance-amd.yml`
   passes `--olo-golden-vendor=amd`, and with no `amd/` directory every golden-backed test on
   that runner fails with "baseline missing". The flag has to come out in the same change.

That change is **deliberately not made in this PR**: it can only be validated on the AMD
runner, which this cross-check could not reach. The evidence for making it is above.

### The one thing this cross-check could not do

Everything here compares *fresh NVIDIA at HEAD* against an *AMD recording from 2026-08-01*.
It cannot see a divergence introduced on the AMD side after that date. Re-running the audit
on a freshly-baked AMD set is what closes that, and `GoldenBaselineAuditTest` now makes it a
CI-gated one-liner rather than a session like this one.
