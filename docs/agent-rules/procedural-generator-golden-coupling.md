# A determinism/portability fix to a procedural generator invalidates its goldens — rebake in the SAME PR (issue #754)

`AtmosphereVisualEvidenceTest`'s `NightClear` (RMSE 13.08) and `NightOvercast` (RMSE 9.40) goldens
sat **permanently red for 10 days** against a threshold of 8. Because they were the *only* consistently
failing test in the suite, "N passed, 1 failed" became the expected result — the exact state in which a
genuine new regression is invisible, and three separate merged PRs each had to write a paragraph
explaining the failure away.

**Neither golden was wrong the way a stale golden is usually wrong.** The night frame had changed for two
*deliberate, already-merged, visually-verified* reasons, and both authors knew their change moved pixels
but deferred the rebake (it needed the baseline GPU) and then it was forgotten:

- **`f4fef24b` — star-field hash made bit-exact across vendors.** The old `fract(sin(dot(p,k))*43758.5453)`
  hash placed stars in different positions on NVIDIA vs Mesa; the fix replaced it with an integer PCG
  bit-mixer over the lattice cell. A *different hash means every star lands somewhere else* — a change
  localised to the **sky band**, strongest in `NightClear`, fully occluded (and so a *passing* cell) in
  `NightStorm`.
- **`dfd100ef` — InfiniteGrid coplanar depth bias.** A `1e-5` depth nudge so the Y=0 grid wins its
  z-fight with the Y=0 ground plane deterministically on every vendor (it was "shattering into dashes
  toward the horizon" on radeonsi). This changed the **ground band** — and *only visibly at night*,
  because the grid is the dominant bright feature on the near-black night ground; by day the lit,
  textured ground swamps it (every Dawn/Day/Dusk golden drifted only ~1–2.6 RMSE, pure re-encode noise).

The decomposition was proven by amplified per-pixel diff heatmaps: `NightClear` = relocated stars (sky) +
grid lines (ground); `NightOvercast` = grid only (sky pure black — stars occluded); `NightStorm` = grid
only, heavier occlusion. Sky colour, moon, clouds, horizon glow and all geometry were byte-for-byte the
same. That is candidate **"stale golden"**, not candidate "rendering regression" — the fix is a
deliberate rebake, not a bug hunt.

## The generalisable rule

**When you make a portability/determinism/quality fix to a *procedural generator* (a hash, a noise field,
a star layout, a dither pattern, a depth-tie-break), it relocates or re-lights the generator's output and
silently invalidates every golden image that captured it. The correctness fix and the golden rebake must
land in the SAME PR.** A generator change that "only" makes output *more correct* still changes *which
pixels are which* — goldens pin pixels, not correctness.

Corollaries that bit here:

- **The coupling must be documented at the CHANGE SITE, not (only) in the test.** The person editing the
  star hash is in `AtmosphereSky.glsl`, not the test file — a note buried in the test never reaches them.
  Both change sites (and the CPU mirror) now carry a `GOLDEN COUPLING:` comment naming the affected
  goldens and the `OLOENGINE_GOLDEN_REBASE=1` mechanism.
- **A red that recurs every run gets *normalised*, and a normalised red blinds the whole suite.** The cost
  is not the one failing test — it is that every other test's signal now reads through "well, there's
  always 1 red." Clearing it is worth a PR on its own.
- **Rebake only the cells that legitimately moved.** A rebase run rewrites *every* golden (PNG re-encode +
  sub-threshold jitter touches all of them). Revert the ones whose new-vs-HEAD RMSE is at the noise floor
  (here ~1–2.6) and keep only the genuinely-changed cells (here the 3 night cells, incl. `NightStorm` at
  7.19 — under threshold but well above the ~1.5 day-noise floor, a latent red-in-waiting on the same
  cause). Committing the noise-floor churn pins transient jitter and hides what actually changed.

## Why NOT add a *second* test that duplicates the golden's relocation signal

The scope of this argument is **narrow**, and getting it wrong is easy: it is *not* a case against
CPU/contract coverage of this code, and none of the existing coverage should be weakened. What already
guards this path, and stays:

- **`AtmosphereSkyMathTest`** (L4) pins the CPU sky mirror — full-day == Preetham, night dark/finite/
  moonlit, the twilight blend monotone, adversarial params sanitised, and the rebake-gate parameter hash
  moving for every field.
- **The visual test's own band contracts** assert the driver-independent physics on every capture (noon
  sky bright + blue, night ≪ day, storm dims the noon ground, dawn horizon warmer than noon).
- **Multi-angle screenshot inspection and the editor shader-compile-log check** for `AtmosphereSky` and
  `InfiniteGrid` (CLAUDE.md's rendering-verification rule) remain the standing discipline any change here
  runs through.

What does **not** earn its place is an *additional* test whose only job is to go red when the stars
relocate — i.e. one that re-detects exactly what the golden already detects. The issue asked to consider
that specific test, and the answer is no:

- **"These goldens must be rebaked" ≡ "the frame changed visibly"**, which is *exactly what the golden
  already measures.* A second test that fires on the same event is a redundant, equally-normalisable red;
  it does not address the process gap (a red left unfixed), and could make it worse.
- **The only *robustly* pinnable thing is the integer hash; everything downstream is float.** Star
  *positions* are bit-exact, but their brightness carries cross-vendor/compiler ULP variance
  (`AtmosphereSky.h` documents the evaluator as "structurally identical, not bit-exact"). A tight
  star-*value* pin would itself become a flaky cross-compiler red — reintroducing the precise problem this
  issue fixes.
- **A *robust* contract (presence/sparsity with wide margins) would NOT fire on relocation** — and
  relocation is the legitimate change you *want* to force a rebake for. You cannot cheaply have a
  non-fragile contract that fires exactly when goldens need rebaking; the golden is the right instrument.

The durable guard is the **same-PR rebake discipline made visible at the change site**, layered on top of
the existing math/band contracts and screenshot/shader-log checks — not another normalisable red.
