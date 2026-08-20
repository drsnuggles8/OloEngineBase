# DDGI probe cascades, toroidal storage and request-driven sparsity

Issue #707 turned the single authored DDGI volume of #632 into a camera-centred clipmap with
request-driven updates and GPU relocation. Four upgrades, and every one of them fails **silently and
plausibly** — none of them make a frame obviously wrong on the frame they go wrong, and none of them
make a test red. This document is the list of ways that happens, and what each one looks like.

Read it before touching `Renderer/DDGI/*`, `include/DDGICommon.glsl`,
`include/DDGIProbeBuffers.glsl` or any `compute/DDGI_*.comp`.

---

## 1. Toroidal storage is the load-bearing invariant, and breaking it looks like smeared GI

A cascade is a probe **lattice** anchored at a fixed world origin, of which a `Dims`-sized window is
stored. Storage is **toroidal**: storage coordinate `s` holds the lattice point congruent to `s`
modulo `Dims` that lies inside the window.

That is not an optimisation. It is what makes cascades possible at all here:

* our probe capture is a **rasterized cube-face mini-G-buffer** (ADR 0007), not a ray trace, so
  recapturing one probe costs six draws over the nearby casters;
* a camera-following grid whose probes all moved every time the window shifted would invalidate
  every probe on every one-cell camera step, and could never converge at any capture budget.

With toroidal storage, a one-cell shift reassigns exactly **one slab** and leaves every other probe's
cached data valid at its existing address. `DDGIMath.ToroidalStorageInvalidatesOnlyOneSlabPerCellOfCameraMotion`
pins the count; if you change the addressing and that test still passes, check it is not passing
because *nothing* changed.

**The failure mode.** If the slab that DID change is not invalidated, those probes keep shading with
irradiance, visibility and a relocation offset measured **somewhere else**. The frame renders. The
probes look lit. The GI is from where they used to be, which reads as light smearing behind the
camera as you fly — easy to attribute to temporal hysteresis, which is exactly the wrong conclusion.

Invalidation is derived **independently on both sides**, from the same two lattice origins:

* CPU: `DDGIProbeUpdatePass::BuildCascades` (so the capture scheduler stops believing a moved probe
  is captured);
* GPU: `compute/DDGI_ProbeMaintain.comp` (so the gather stops reading it, and the aux record resets).

They are not allowed to inform each other — the GPU telling the CPU would be a readback, which is
the thing §4 exists to remove. Deriving the same answer twice from the same inputs is the correct
shape here; keep it that way.

## 2. `%` truncates toward zero in both C++ and GLSL

`DDGI::WrapIndex` / `ddgiWrapIndex` are Euclidean modulo, and that is the single most load-bearing
line in the whole scheme. A cascade's lattice coordinates go **negative** the moment the camera moves
toward −x, and a plain `%` maps a negative lattice coordinate to a negative storage index.

A negative index into the probe atlas does not crash: it samples some other tile, or clamps, and the
probe field quietly develops a mirror image of itself on one side. `DDGIMath.NegativeLatticeCoordinatesWrapIntoTheStorageWindow`
builds its fixture at a deliberately negative camera position and asserts the round trip; if you find
that test "obviously passing", note that the fixture asserts `LatticeMin.x < 0` first, precisely so
it cannot degenerate into testing the positive case.

## 3. Sparsity's failure mode is "no GI, and no error"

A probe is relit only if something **requested** it: a shaded screen pixel (`DDGI_RequestScreen.comp`),
another live probe's cached hit point (`DDGI_RequestProbe.comp`, **one indirection deep**), or the
camera-neighbourhood seed. Three things follow, and all three have bitten:

* **The floor is not optional.** The camera seed in `DDGI_ProbeMaintain.comp` requests every probe
  within `DDGICameraSeedRadius` unconditionally. Without it, one bad frame of the screen request
  chain — no depth buffer yet, a sky-only view, a resource that resolved to null — means no probe is
  live, nothing relights, and GI fades out over the request lifetime with nothing logging anything.
  A feature whose failure mode is silence needs a floor under it.
* **The probe→probe hop is what makes the bounce term work.** Cached hit points are surfaces, and
  those surfaces are usually *not* on screen — a wall the camera sees bounces light off a ceiling it
  does not. Drop the hop and GI goes subtly too dark in exactly the places indirect light comes from.
  Nothing fails; the picture just gets worse in a way that reads as "our GI is a bit weak".
* **The indirection is exactly one, and something has to enforce that.**
  `DDGIProbeAuxRecord::ScreenRequestFrame` is that something: the hop walks only probes a *screen*
  requester marked, and the requests it raises are marked non-screen. Without the distinction the
  request set grows transitively over frames and converges on the dense grid again — which does not
  fail, it just quietly stops being sparse. Measure `GetProbeStats()` rather than believing it.

## 4. The relight and blend gates must be the same predicate

`ddgiProbeUpdatesNow` is called by **both** `DDGI_Relight.glsl` and `DDGI_BlendIrradiance.glsl`, and
that is deliberate. A probe that blends without being relit EMAs toward a radiance cache nobody is
refreshing — it drifts toward a stale lighting state over many frames, converging on something that
was right once. Every test stays green while it happens.

Related: the irradiance atlas is ping-ponged, so "this probe is not updating" means **copy `prev`
through**, never `discard`. Discarding leaves the write target holding its two-frames-old content,
which flickers at exactly half the update rate and reads as noise.

## 5. There is no readback in `Execute`, and that is a contract

Issue #707's acceptance criterion 3. `RelocateProbe` / `ClassifyProbe` moved into
`compute/DDGI_Relocate.comp`; the old code read each captured probe's hit tile back with
`glGetTextureSubImage` **immediately after the draw that produced it**, which is a full pipeline
drain in the middle of the frame, once per captured probe, every frame.

Consequences that are easy to undo by accident:

* The CPU no longer knows a probe's relocation offset or its classification. `DDGIProbeUpdatePass::ProbeRecord`
  is split into CPU-owned fields (`Captured`, `LastCaptureFrame`, `RelocationIteration`) and GPU-owned
  mirrors (`OffsetN`, `State`, `Bounce*`) that read as their defaults until `ReadbackProbeDiagnostics()`
  is called. Zeros look exactly like "un-relocated, uncaptured", so a reader that forgets the call
  gets a plausible answer rather than an obvious one.
* **The capture position is derived on the GPU.** `DDGI_Capture.glsl`'s vertex stage fetches the
  probe-data texture and subtracts the relocated position itself; the CPU supplies an eye-at-**origin**
  view-projection and the probe's global index. Capturing from the lattice point while the relight
  stage reconstructs hit points from the relocated one offsets every cached hit by up to 0.45 of a
  cell — GI that is slightly wrong everywhere, with nothing failing.
* The capture *scheduler* still runs on the CPU, because capture is rasterization and the CPU issues
  the draws. It uses a frustum + distance **proxy** for liveness (`IsProbeCpuLive`) rather than the
  GPU request set. Being conservative is the right failure direction: capturing a probe nothing shades
  wastes a slice of a small budget, while missing one leaves a live probe permanently uncaptured and
  therefore permanently absent from the gather.

  **THE PROXY MUST BE A SUPERSET OF WHAT THE GPU CAN REQUEST, and getting that wrong is a permanent
  steady state, not a slow one.** The first version of `IsProbeCpuLive` tested the frustum plus the
  camera-seed sphere — which covers two of the three request sources and misses the third. The
  one-indirection hop marks probes around a live probe's cached HIT POINTS, and a hit point can sit
  up to `kMaxRayDistanceSpacingScale` spacings behind its probe, outside the frustum entirely. Those
  probes stayed live on the GPU and invisible to the CPU scheduler **forever**: 30 of 509 live probes
  stuck uncaptured across a 910-frame steady state. They are also the worst 30 to lose — off-screen
  surfaces are precisely what the hop exists to bring in.

  The fix is a frustum test *dilated* by the hop's reach (one max ray distance to the hit point plus
  one cell to the probes gathered around it), which closes the gap exactly and bounds the
  over-capture. The general rule: any CPU-side approximation of a GPU-side set has to be checked for
  **containment**, not similarity, and the check needs an assertion — nothing about the frame looked
  wrong while those 30 probes were missing, and `GetCapturedFraction()` cannot see it because they
  were never expected to be captured. `UncapturedLive` reaching a non-zero fixed point is the signal;
  `CascadeFieldIsTemporallyStableFromAFixedPose` asserts it before it measures anything else.
* `ReadbackProbeDiagnostics()` must not mutate scheduling state. It deliberately does **not** copy the
  GPU's captured flag back into `ProbeRecord::Captured`: a diagnostic that edits the scheduler makes
  "look at the probe table" change what the next frame does.

## 6. The spring relocation is a feedback loop, so its step size is a stability parameter

`DDGI::RelocateProbeSpring` replaces RTXGI's three-case closest-face rule with a force balance
(crowding + average free direction + a pull back toward the lattice point), keeping the
strictly-inside-geometry escape because that is the one case where the closest face is unambiguous.

Two things that are not obvious:

* Its output feeds the next capture's position, so a step size near 1 turns a mild crowding signal
  into an oscillation between two cells — which reads as probes that never settle, not as a bug.
  `kSpringStepScale = 0.35` settles in three to four captures.
* The ellipsoid clamp **projects** rather than rejecting. Rejection is right for RTXGI's large
  discrete steps ("that move was wrong") and wrong for a spring: a permanently crowded probe would be
  pinned at its previous offset with an unsatisfied force forever.
* The **sky counts as a free direction with full weight**. Without it the mean free direction is built
  only from the far walls, and a probe by a window drifts outward instead of staying put.

## 7. Cascade defaults are a memory decision, not a quality one

The issue quotes PGI's 6 cascades × 32³ — ~196k probes. **Always quote the hit-cache resolution with
a footprint figure**, because the per-probe cost depends on it and the same field otherwise reads as
68 MB or 128 MB depending on which comment you land on:

| | per probe | 4 × 16³ (shipped default) | 6 × 32³ (PGI's figure) |
|---|---|---|---|
| t = 8 (what the CASCADE path submits) | ~4.3 KB | **~68 MB** | ~0.8 GB |
| t = 16 (what an authored volume uses) | ~8.0 KB | ~128 MB | ~1.5 GB |

Irradiance and visibility are fixed-size and ping-ponged; radiance and the hit cache scale with t².
The full breakdown is in `RenderingPath.h`. PGI can afford their default because they re-trace rays
per update and store no hit cache at all.

The renderer-settings panel prints the probe count and estimated footprint next to the sliders for
exactly this reason. If you raise them, have the arithmetic in hand; the failure is an allocation
that either fails or swaps, not a warning.

## 8. What to verify, and in what order

1. **`DDGIMathTest`** (L1) pins the CPU side of all of the above headlessly.
2. **`DDGIReferenceParityTest`** is the strongest oracle available: probe irradiance against
   `PathTracer::EstimateIrradiance` in physical units. It runs the AUTHORED path, which #707
   deliberately leaves bit-identical — a parity move there means the compatibility claim broke, not
   that the cascades are wrong.
3. **Multi-angle screenshots from a scene that crosses a cascade boundary.**
   `SandboxProject/Assets/Scenes/DDGICascadesTest.olo` is built for it: a 120 m corridor with a
   coloured bounce panel inside each cascade window, and no authored volume. A blend band is invisible
   from a single viewpoint.
4. **Convergence over many frames, not one.** Sparsity and the variable update rate are about *when*
   probes update, so the failure mode is temporal: watch for flicker and for creep as the camera moves,
   not for a wrong single frame.
5. **Measured probe counts** from `GetProbeStats()` — live vs total, relit vs live. The claim
   "sparsity cut the active set" is a number, not an argument.
