# Variable Rate Compute Shading: the saving is the easy half

Issue #683. Read this before touching `VRCSClassify.comp`, `include/VRCS.glsl`, `ShadingRateClassifier`,
or before adopting VRCS in a second compute pass.

VRCS lets a compute pass shade one pixel in four (or sixteen) where the frame is locally flat. A
shared classification pass rates every 8×8 screen tile into a **footprint** — 1, 2 or 4 — and a
consuming pass asks, per invocation, "am I the leader of my footprint?": leaders compute once and
broadcast across their block, followers retire.

Every defect in this system is quiet. Nothing crashes, no test goes red on its own, and the frame
keeps rendering — slightly wrong, in one scene, at one camera angle.

---

## 1. The issue's premise was wrong about three of its four passes

The issue opens with "the heavy lighting passes are fully compute (`DeferredLightingPass`, GTAO,
SSGI, `VolumetricFogPass`)". That is true of one and a half of them, and the plan's explicit
ordering — "integrate `DeferredLightingPass` first" — is built on it. Measured against the tree at
`7581bb05b`:

| pass | what it actually is | VRCS applicable? |
|---|---|---|
| `GTAORenderPass` | compute, 16×16 groups at scene-band resolution | **yes** — adopted |
| `VolumetricFogPass` | compute, but over a fixed 160×90×64 **froxel volume**, not screen tiles | not directly; see §7 |
| `DeferredLightingPass` | a fullscreen **fragment** draw (`DeferredLighting.glsl`), with an MSAA per-sample variant and a virtual-geometry overlay | **no** — nothing to coarsen |
| `SSGIRenderPass` | three fullscreen **fragment** draws (`PostProcess_SSGI*.glsl`) | **no** |

A fragment shader cannot express "compute once per quad and broadcast": the quad is the hardware's,
not the shader's. Adopting VRCS in either of those two means **porting the pass to compute first**,
which for `DeferredLightingPass` is a substantial project of its own (image-writable scene colour,
the Forward+ / reflection-probe / IBL rebinds, the MSAA variant, the debug overlay).

**The general lesson, which is why this is §1:** an issue that names call sites is asserting
something about them. Check the shape of every named site before committing to an ordering built on
it — here the first line of the plan was the one that could not be done.

---

## 2. `barrier()` is why the broadcast is per-invocation and not shared-memory

The obvious way to broadcast is shared memory: the leader writes, `barrier()`, the followers read.
It is also wrong here, and the reason generalises past VRCS.

Every consuming kernel already has early `return`s — GTAO alone has four (out of bounds, sky, the
normal sentinel, too small a pixel radius). A `barrier()` that only some invocations of a work group
reach is undefined behaviour, and the driver's choice of undefined ranges from "correct" through
"wrong" to a TDR device reset. See [gpu-scan-compaction.md](gpu-scan-compaction.md) §1: a green run
is not evidence that the barrier contract holds; only reading the shader is.

So the contract in `include/VRCS.glsl` is deliberately **communication-free**. Each invocation reads
one texel of the rate image, computes `OloVRCSIsLeader` from its own coordinates, and either retires
or writes its own block. No invocation waits for any other, which is what lets it compose with early
returns — and with any future kernel's early returns, without re-auditing them.

The classification shader itself *does* use `barrier()`, and there the rule is honoured the other
way: **no invocation returns above the reduction.** An out-of-viewport invocation contributes
reduction identities (`+FLT_MAX` for a min, `-FLT_MAX` for a max, zero counts) and stays alive
through all six barrier steps.

---

## 3. Coarsening must be GRANTED, never assumed

Every threshold in `VRCSClassify.comp` is an upper bound on a tile metric, and a tile must clear all
of them. Everything else — an unclassified tile, a tile that mixes sky and geometry, a tile with a
sentinel normal in it, a tile whose rate texel was never written, a rate value outside `{1,2,4}` —
resolves to **full rate**.

That asymmetry is the whole safety argument, and it comes from the cost asymmetry: a wrongly
full-rate tile costs microseconds, a wrongly coarse tile costs image quality in a way nobody notices
until a player does. `OloVRCSDecodeFootprint` therefore rejects unknown values rather than trusting
them, and `GTAORenderPass` keys the consume flag on what classification *actually produced this
frame* (`m_VRCSActiveLastExecute`), not on the settings — a `Classify()` that declined must not leave
the shader coarsening off whatever the rate image happened to contain.

**Three signals, because each is blind where another sees:**

- **Depth planarity** catches silhouettes — see §3a, because the first version of this got it wrong
  in a way that no test could see.
- **Normal agreement** (`1 - |mean(n)|`) catches creases and curvature. A box edge inside a tile is
  *continuous in depth* and completely different in shading — depth alone coarsens straight across
  it. `VRCSClassifierGpuTest.ANormalCreaseAtConstantDepthKeepsThatTileFullRate` is that case with the
  depth term held constant so it cannot mask the result.
- **Previous-frame luminance** catches what the geometry buffers cannot see at all: a shadow edge, a
  specular highlight, a light falloff crossing a perfectly flat wall. It is optional (no history on
  frame 0, none with TAA off) and its absence makes classification *stricter*, never looser.

---

## 3a. Measure departure from a PLANE, not depth range — and only the heatmap could tell

The first working build used the obvious depth metric: the tile's `(maxZ - minZ)` relative to its
nearest surface. Every test passed. The image-diff was 1.9/255, the tile-seam ratio was flat, the
crease survived, TAA was stable. **The feature was also doing almost nothing.**

The rate heatmap is what showed it. Reading `VRCS_Rate_Angled.png`: the sky coarsened, the cube's
camera-facing front coarsened — and the entire floor stayed full rate, as did the cube's grazing
*top* face. The pattern is unmistakable once seen: **everything facing the camera coarsened, and
everything receding from it did not.**

The cause is that a depth *range* test punishes slope. A ground plane seen at a grazing angle
genuinely spans a lot of depth across 8 pixels; that is perspective, not a discontinuity. So the
metric rejected exactly the surfaces that make up most of an outdoor frame, and it rejected them for
being *far away at an angle* rather than for being *broken*.

What actually invalidates a broadcast is a **discontinuity**. So the metric is now the maximum
residual of a least-squares **plane fit over inverse depth**, relative to the tile's mean inverse
depth. `1/z` is exactly affine in screen space across a planar surface — it is what
perspective-correct interpolation interpolates — so:

- a plane has residual ~0 **at any angle**, however steeply it recedes;
- a step has a large residual **however small the step is in world units**;
- a gently curved surface has a small residual and is allowed to coarsen, which is right (its
  shading varies smoothly), while strong curvature is caught by the normal term instead.

Because the tile is a fixed 8×8 grid, the fit's normal equations collapse to three sums — the other
moments (`Σdx`, `Σdy`, `Σdx·dy`, `Σdx²`) are compile-time constants. It costs one extra
barrier-separated reduction round and **no extra sampling**: each invocation still holds its own
inverse depth in a register when the residual is computed. Those constants are only valid for a
complete tile, which is why **a partial tile at the screen edge never coarsens**.

**Three things generalise past this bug:**

1. **A conservative heuristic's failure mode is invisible to correctness tests.** Every assertion in
   this feature's suite is of the form "did coarsening damage the image" — and a classifier that
   coarsens nothing passes all of them perfectly. The suite had no assertion that could fail, because
   "the feature is inert" is not incorrect, just useless. The heatmap is the only instrument that
   answers *where* it engaged, and it was worth building before it was worth trusting the numbers.
2. **Reach for the debug visualisation before reading the summary statistics.** The scalars said
   "visually equal output, no seams" — which was true, and which is exactly what a disabled feature
   produces. Two scalars agreeing with your hopes is not evidence.
3. **`ASteeplyRecedingPlaneStillCoarsens` is the regression test**, and it asserts a *positive*
   ("this must coarsen") deliberately, against the grain of every other test in the file. A suite
   made only of "must not coarsen" assertions has a trivial passing solution.

---

A sky/geometry mix is rejected on the mix itself, not on the depth range. The far plane linearises
to a finite distance, so a horizon tile's range is large only by luck — reject the case, do not rely
on the metric to notice it.

---

## 4. The partition property, and the two ways to break it

Footprints are aligned to the **pixel-space origin** (1, 2 and 4 all divide the 8-pixel tile), so
each leader sits at the minimum corner of its block. Two consequences the consumers rely on:

- if any pixel of a footprint is inside the viewport, its leader is too — so a consumer bounds-checks
  only the *broadcast*, never the dispatch;
- the leaders' blocks tile the plane exactly once.

Break it one way and some texels are never written, so they keep whatever the transient pool held —
on a fresh allocation that is unspecified memory, on a recycled one it is last frame's image. Break
it the other way and two invocations write the same texel. **Neither is visible on a smooth surface,
and neither fails any test that only checks values.** `VRCSContractTest` counts writes per pixel
across a 2×2 block of tiles and requires exactly one; it is a CPU test with no GPU in it, because the
property has nothing to do with any driver.

**A footprint that did not divide the tile** would put one leader's block astride two tiles with
different rates — the one case the leader/follower agreement argument does not cover, since a
follower would be reading a different tile's footprint than its leader wrote. Pinned separately.

---

## 4a. Masking lanes is not retiring waves, and only the second one is faster

The first *correct* build — planarity metric in, heatmap showing coarse tiles everywhere but the
silhouettes, image-diff and seam tests green — made GTAO **43% slower**: 0.947 ms → 1.354 ms, with
classification costing only 0.048 ms and the untouched denoise dispatch confirming the measurement
had not drifted.

The mapping was the obvious one: every invocation keeps its own pixel and returns unless it is its
footprint's leader. That is correct, and it saves nothing. Leaders sit at even x and even y, so every
32-lane wave over a 16×16 group still contains eight of them and still pays the full latency of the
slice loop. What the "saving" actually bought was a texture fetch, a branch, and four times the
stores.

**A wave costs what its slowest active lane costs. Masking 24 of 32 lanes changes nothing.** To save
time the spare lanes must be *contiguous*, so whole waves fall off the end of the work list.

`GTAO.comp`'s `ResolveVRCSWorkItem` does that: the group's 256 invocations are handed the
concatenated leader lists of the four 8×8 tiles the group covers. With all four at 2×2 there are 64
work items, so lanes 64–255 — six of the eight waves — retire on a single comparison and never touch
a texture.

Two consequences worth carrying:

- **The compaction forced the edge weight to be broadcast too** (§5). Keeping any per-pixel output
  meant every follower still had work, so no wave could retire, so the compaction would have bought
  nothing. A "safety" decision about one output silently capped the whole feature's ceiling at zero.
- **Keep the disabled path on the identity mapping.** `ResolveVRCSWorkItem` branches on the enable
  flag — a group-uniform branch on a uniform, so it is free — and returns `gl_GlobalInvocationID`
  when VRCS is off. The compacted mapping is a *permutation* of the same pixels, so it would be
  correct there too, but it changes which lane touches which texel and therefore the access pattern.
  A performance feature must not perturb the path it is disabled on.

---

## 5. Per-pixel data must stay per-pixel — until it stops the waves retiring

GTAO writes two outputs: the AO term and a per-pixel **edge weight** the denoise pass uses as
bilateral guidance. The first implementation broadcast the AO term and kept the edge weight
per-pixel, reasoning that broadcasting it would let the bilateral blur run straight across the
discontinuities the term exists to protect.

**That reasoning was right about the risk and wrong about where it applies** — and it was what capped
the whole feature at zero saving (§4a), because keeping any per-pixel output meant no invocation
could retire.

Inside a coarse tile the classifier has already certified planarity *and* normal agreement. There is
no discontinuity there for the edge term to protect, and its per-pixel variation is noise. The
guidance that matters is at the tile boundaries, and those tiles are full rate. So the edge weight is
broadcast too.

**Two generalisations for the next adopter, and they pull in opposite directions:**

1. Before broadcasting an output, ask what reads it. A value consumed per-pixel by a *later* pass is
   not automatically safe to coarsen just because the pixel that produced it was.
2. But check the answer against what the classifier already guarantees, because a per-pixel output
   you keep "to be safe" is not a local cost — it stops every wave from retiring and can take the
   entire saving with it. "Safe" and "inert" are easy to confuse when the measurement comes last.

---

## 6. Two mirrored numbers, and the slot the feature deliberately did not take

- `OLO_VRCS_TILE_SIZE` and the three rate encodings exist in both `include/VRCS.glsl` and
  `ShadingRateClassifier`; the classification shader's `local_size` must equal the tile size, because
  `Classify()` dispatches the tile grid **itself**, not a ceil-divide of it. `VRCSContractTest` reads
  the shader text and compares, rather than restating the constants — a restatement is a third
  mirror, not a check.
- The classifier's params ride **`UBO_USER_0` (binding 7)**, the shared pass-local slot, exactly like
  `DDGIPassData`. The engine has **one** uniform-buffer binding left below the GL 4.6 minimum of 84,
  and #707 and #715 both recorded the decision not to spend it; a per-dispatch parameter block for a
  classifier that uploads immediately before its own dispatch has no claim on it either.
- The rate texture rides a **pass-local heap-offset index** (6, next to GTAO's 3/4/5), not a new
  `TEX_*` constant. Adding an engine texture slot moves `MAX_ENGINE_TEXTURE_SLOTS`, which moves
  `HEAP_IMAGE_SLOT_BASE`, which must be mirrored by hand in `BindlessHeap.glsl` — a drift that has
  already shipped once. The feature needs a per-dispatch binding, not a reservation, so it takes one.
- The rate **heatmap** writes into the AO term ahead of every early-out, so a sky or sentinel tile
  does not paint white just because its early return publishes AO = 1.0 — and `GTAORenderPass`
  suppresses the denoise while it is on, because four bilateral blur passes over a block pattern is a
  blur of the thing the overlay exists to show. That suppression is an **Execute-side** decision
  only: `Setup` still declares the ping/pong scratch, so toggling the overlay does not change graph
  topology and the fingerprint cache has nothing to freeze.
- Both slots are bound **unconditionally**, with a stand-in when the real resource is absent (the
  classifier's previous-colour slot falls back to scene depth; GTAO's rate slot falls back to the
  Hilbert LUT). The heap-offset table is shared and persistent across dispatches, so an index a
  dispatch never writes keeps whatever the previous flush left there. The GTAO stand-in is the
  Hilbert LUT specifically because it is the only other **unsigned** sampler that shader binds — a
  heap handle carries no type, so a float stand-in read through `usampler2D` returns garbage
  ([glsl-shaders.md](glsl-shaders.md) §5d).

---

## 7. Adopting VRCS in a second pass

1. `#include "../include/VRCS.glsl"`, read the footprint, branch on `OloVRCSIsLeader`.
2. Route **every** store of the coarsenable output through a footprint loop with a bounds test.
   Missing one leaves stale texels in exactly the tiles that coarsened.
3. Leave per-pixel outputs per-pixel (§5).
4. Bind the rate texture with `ShadingRateClassifier::GetRateLifetime()` — `Persistent`, because the
   image is the classifier's own texture and not a graph-pooled transient.
5. Gate on `Classify()`'s return value, not on the setting.
6. **Move the frame stamp.** `Classify()` is idempotent within a frame index so two consumers share
   one classification. Today the stamp is `GTAORenderPass`'s own Execute counter, which is a faithful
   frame number only while GTAO is the only consumer. A second adopter must replace it with a
   renderer-wide monotonic frame index — there is none today
   (`InflightFrameManager::GetCurrentFrameIndex` is a ring position) — or the two passes will
   disagree about which frame it is and each will re-dispatch classification, which is exactly the
   duplication the stamp exists to prevent.
7. Hash the pass's VRCS gate into the blackboard fingerprint if its `Setup()` branches on it
   ([virtual-shadow-map-page-cache.md](virtual-shadow-map-page-cache.md) §5).

**`VolumetricFogPass` is the interesting next case and is not a copy of this one.** Its dispatch is a
160×90×64 froxel volume, already ~1/12 resolution in XY, so the screen-tile grid does not map onto it
one-for-one: adopting VRCS there means classifying in *froxel* XY (or projecting screen tiles into
it) and coarsening 2×2 froxel quads, and the saving must be weighed against a volume that is already
tiny. Measure before building it.

---

## 8. Measuring it honestly

The classifier reads three full-resolution buffers to write one texel per 64 pixels. It is entirely
possible for it to cost more than it saves, and a report quoting only the consuming pass's GPU-ms
would show a win while the frame got slower. `VRCSPerfProbe` (DISABLED_, an instrument not a gate)
therefore reports the **whole GTAO bracket**, plus the untouched denoise dispatch as a control that
should not move.

Measured on an RTX 4090 at 1600×900, cube row on a flat floor, 2×2 only, eight interleaved rounds,
two runs:

| | full rate | variable rate | saved |
|---|---|---|---|
| **whole bracket** (classify + gtao + denoise) | 0.352 / 0.359 ms | 0.286 / 0.294 ms | **0.067 / 0.066 ms — 18.9% / 18.2%** |
| gtao dispatch alone | 0.262 / 0.268 ms | 0.126 / 0.125 ms | 52% |
| denoise (control) | 0.091 / 0.091 ms | 0.090 / 0.091 ms | — |
| classification | — | 0.071 / 0.077 ms | — |

**Classification eats about half the gross saving.** The shading dispatch halves, and roughly half of
what that buys goes straight back into deciding where to coarsen. That ratio is what makes the
"adopt VRCS in a second pass" case strong — the classification is shared, so the second consumer's
saving is very nearly free — and it is also why a single-consumer VRCS is a much weaker proposition
than the headline 52% suggests.

**Three properties make this measurement trustworthy, and all three were absent from earlier runs
that produced nonsense:** the denoise control does not move; the components reconcile
(0.136 saved − 0.071 classify ≈ the 0.067 whole-bracket figure); and the result reproduces across
runs. Any of those failing means the number is an artefact, not a result.

The saving is a property of the **scene**, not of the feature: a frame of foliage coarsens almost
nothing, a frame of flat walls almost everything. Quote the coarse-tile fraction alongside any number
or the number is not reproducible.

And the visual claim needs its own instrument. A **mean** image difference cannot see a tile lattice
— a one-pixel step repeated every eight columns is a rounding error in a mean over a million pixels
and an obvious grid on screen. `VRCSVisualEvidenceTest` measures the mean step across 8-pixel tile
boundaries against the mean step *within* a tile, on smooth ground, as a **ratio against the
full-rate frame**: an artefact inflates only the numerator, while a global contrast change moves both
and cancels.
