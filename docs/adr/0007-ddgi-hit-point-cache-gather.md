# DDGI (#632) gathers rays from a relit static hit-point cache, not per-frame cube rasterization

Issue **#632** asks for realtime dynamic diffuse GI (DDGI) on the existing
`LightProbeVolumeComponent` probe grid: octahedral irradiance + Chebyshev
visibility atlases, per-frame response to moving lights, infinite bounce,
relocation/classification, an amortized budget, and a sampler rewrite that
kills the confirmed wall light-leak in `LightProbeSampling.glsl`. The issue's
proposed gather backend was "per-frame probe ray dispatch … gathered via an
on-GPU rasterized mini-cube capture (no hardware RT)".

Before designing, a deep research pass on UE5 Lumen was mandated (per the
task handover) to decide what to adopt. This ADR records that decision. The
full research corpus (5 Lumen reports, an implementation-grade classical-DDGI
spec, a survey of shipped non-RT probe relighting, and 5 engine-mapping
reports) lives in the session scratchpad; the primary sources are the
SIGGRAPH 2021/2022 Lumen decks, the JCGT 2019/2021 DDGI papers, the NVIDIA
RTXGI-DDGI SDK, and GDC decks for Far Cry 3 (2012), AC4 (2014), The Division
(2016), and Ghost of Tsushima (SIGGRAPH 2021).

## Decision

The **consumer-facing side is classical DDGI exactly as the issue proposed**:
fixed probe grid on `LightProbeVolumeComponent`, octahedral irradiance atlas
(RGBA16F, ping-pong, temporal EMA), Chebyshev mean/mean² visibility atlas
(RG16F, ping-pong), RTXGI-style relocation + classification, and the full
leak-fixing sampler (trilinear × wrap-shading × Chebyshev³ × self-shadow
bias).

The **gather backend diverges from the issue's literal text**: instead of
re-rasterizing a mini-cube per probe per frame and shading it at capture
time, each probe owns a **static hit-point cache** — a small octahedral-space
mini-G-buffer (albedo, octahedral-encoded normal, hit distance, backface
flag; default 16×16 = 256 fixed directions per probe) captured by a budgeted,
amortized rasterization pass (a few probes per frame, ShadowRenderPass-style
mini-render, 6 cube faces resampled to octahedral by a per-tile fullscreen
draw). Every frame, a fullscreen **graphics** pass over the radiance
framebuffer **relights all cached hit points** — a fragment pass rather than
compute precisely so it can `#include` PBRCommon and reuse the CSM +
local-light shadow atlas evaluators and falloff formulas the main lit paths
use (`DDGIProbeUpdatePass::RelightProbes`; a `RelightBudget` maps probe
ranges to scissored atlas-row `DrawIndexed` windows) — **plus the previous
frame's probe irradiance at the hit point** (infinite bounce). A second
fullscreen draw then cosine-convolves the result into the irradiance atlas
under EMA hysteresis.

In one sentence: **rasterization is the amortized *capture* stage; the
per-frame stage is a *relight* of cached hits.**

## Why the divergence (the load-bearing arguments)

1. **Moving-light latency.** If hits are shaded at capture time, a probe's
   lighting only changes when it is re-captured. Any feasible raster capture
   budget is a few probes per frame (Ghost of Tsushima shipped exactly 1
   probe/frame; this engine's only working scene→cubemap path,
   `ReflectionProbeBaker::CaptureSceneCubemap`, costs 7 full render-graph
   frames per probe) — so a 16×8×16 grid would take tens of seconds to notice
   a moved light, failing the issue's headline acceptance criterion.
   Relighting the cache decouples the two rates: **lighting responds within
   one frame for every probe**; only *geometry* changes wait on the amortized
   capture.
2. **It is the proven non-hardware-RT path.** Every shipped non-RT probe GI
   found in the research relights a static capture rather than re-gathering:
   The Division relit 600–800 probes' cached surfels per frame in 0.95 ms on
   Xbox One; Far Cry 3's deferred radiance transfer volumes, AC4's
   time-of-day-normalized irradiance, and Ghost of Tsushima's relightable
   G-buffer-cubemap probes are the same shape. None re-rasterizes per frame;
   none does moving-geometry bounce.
3. **It is also Lumen's own core insight, at probe granularity.** Lumen's
   surface cache exists precisely to decouple "capture material data slowly,
   under budget" (512×512 texels/frame) from "relight it in texture space
   every frame" (1024×1024 texels/frame direct + amortized radiosity). We
   adopt the principle without the machinery that doesn't fit this engine
   (see below).
4. **Determinism and testability.** Fixed capture directions (the octahedral
   texel directions) mean no stochastic ray noise: CPU-mirror contract tests
   and golden PNGs are stable, and hysteresis only has to smooth light
   changes and bounce propagation, not filter Monte-Carlo noise. 256 fixed
   directions per probe matches RTXGI's default 256 rays/probe/frame in
   angular sample count.

## What we adopt from Lumen (and what we explicitly don't)

Adopted:

- **Capture/relight decoupling** (surface-cache principle) — the central
  divergence above.
- **Budget + refresh scheduling**: capture budget (probes/frame) prioritizes
  never-captured probes first, then re-captures the oldest (Lumen's
  `CardCaptureRefreshFraction=0.125` idea) so moved *static* geometry heals
  within seconds at bounded cost; relight is optionally budgeted round-robin
  for very large grids ("fixed update cost, variable lighting latency").
- **Feedback-loop guards** (shared with RTXGI): backface hits contribute zero
  radiance, albedo clamped (≤ 0.9) in the bounce term, and relit radiance
  saturated. The albedo clamp is the load-bearing one — see *"The bounce
  gather reaches one probe spacing past the bounds"* below for why it, and not
  the volume test, is what makes the loop a contraction.

  Lumen's 10 cm `MinTraceDistanceToSampleSurface` anti-self-lighting rule was
  listed here as adopted and **never was** — `DDGI_Relight.glsl` has no
  minimum-hit-distance guard before the previous-frame probe sample. That went
  unnoticed for as long as it did because the bounce term it guards was itself
  dead in the common authoring case (issue #751): a guard on a term that is
  always zero cannot be observed to be missing. Corrected here rather than
  implemented, because with the bounce term live the reference path tracer says
  it is not needed at this probe density — see the measurements below.

Explicitly rejected as out of scale for this engine (each is engineer-months
and requires infrastructure that does not exist here — no mesh/global SDF, no
GPU BVH, no geometry-shader stage, no virtualized page streaming):

- **Mesh cards / virtualized surface-cache atlas** (import-time surfel
  clustering, GPU-feedback page residency, runtime BC compression).
- **Software SDF ray tracing** (per-mesh narrow-band SDF baker + brick
  streaming + global-DF clipmap compositor + leak-heuristic tuning) — and
  Godot's SDFGI/HDDAGI history shows even the scoped-down version is a major
  project with its own failure class.
- **Screen probes + world radiance cache clipmaps** (view-driven adaptive
  placement, product importance sampling). Notably, Lumen itself ships a
  fixed-grid "Irradiance Field Gather" fallback mode — Epic's own evidence
  that a DDGI-shaped grid is the right low-end architecture.
- **SDFGI-lite** stays out of scope per the issue's own text (future epic).

## Other recorded divergences from the issue text / RTXGI reference

- **Dynamic (moving) geometry is receive-only**: movers sample probe GI but
  do not occlude or bounce it. This matches every shipped non-RT system
  (FC3/AC4/Division/GoT/Godot SDFGI all made the same call); near-field
  dynamic response continues to come from SSGI/SSAO composing on top. Healing
  for *moved static* geometry comes from the refresh budget plus an explicit
  invalidation hook.
- **Visibility (Chebyshev) data updates at capture time**, not per frame: hit
  distances come from the cached depth, so the visibility atlas only changes
  when a probe is (re)captured — still ping-ponged and EMA-blended so
  relocation/recapture doesn't pop, but there is no per-frame distance
  re-blend. Static-scene visibility is exactly what kills the wall leak, and
  it is *more* temporally stable than RTXGI's per-frame stochastic distance
  blend.
- **Linear RGBA16F storage without RTXGI's gamma-5 perceptual encode.** The
  gamma encode exists to make EMA perceptual and to survive 10-bit formats;
  our input is noise-free (fixed directions) and RGBA16F is the engine's
  first-class HDR format (`R11F_G11F_B10F` isn't in the texture enums). The
  threshold-based hysteresis boost (irradiance delta > threshold → cut
  hysteresis) is kept for fast response to big lighting changes.
- **`m_RaysPerProbe` maps to the hit-cache angular resolution** (8×8=64,
  16×16=256, 32×32=1024; default 256) — the honest equivalent of ray count in
  a cached design.

## The bounce gather reaches one probe spacing past the bounds (issue #751)

Amends the gather contract above. The original design sampled the bounce term
through the *same* `ddgiSampleIrradiance` the lit passes call, and that sampler
opens by returning zero for any position outside the volume. Every cached hit
point is **on a surface**, so a volume fitted to a room's interior air — the
natural authoring, and what both `SandboxProject/Assets/Scenes/DDGITest.olo` and
`DDGIVisualEvidenceTest` do — excludes every wall, floor and ceiling in the
room, which is to say every surface the bounce light was supposed to come from.
The infinite-bounce term was therefore **exactly zero on every probe, every
frame**, with no visual signature beyond "the GI is a bit dim". The offline
reference path tracer (#709) measured probe irradiance at 0.41–0.61× ground
truth while agreeing with the *direct-only* reference to within 1%.

**Decision.** The bounce path gathers through a margined variant of the same
function — `ddgiGatherIrradiance(…, volumeMargin, intensity)` — with
`volumeMargin` = one probe spacing per axis and `intensity` = 1. The lit passes
keep calling `ddgiSampleIrradiance`, which is now a wrapper passing margin 0 and
the authored intensity, so their behaviour is unchanged by construction rather
than by inspection (`DDGI::VolumeWeight` with a zero margin reproduces the old
hard inside-test exactly, boundary inclusive; pinned by
`DDGIMath.VolumeWeightZeroMarginIsTheHardInsideTest`).

**Why one spacing, and why a margin at all.** The gather's trilinear lookup
already clamps to the boundary probe layer for a position outside the bounds —
`p0` is clamped to the grid and `frac` to [0, 1] — so "extend the volume" and
"clamp the lookup" are the same arithmetic; the only question is how far that
constant extrapolation is allowed to reach. A probe field cannot resolve
variation finer than its own spacing, so extrapolating by one spacing is exactly
as accurate as the interpolation *inside* the volume, and no further. Weight
falls off as a smoothstep across the band rather than stepping at its edge,
because a step in the bounce term draws a hard edge in the GI wherever geometry
crosses it.

**Contractiveness — the constraint this had to be argued against.** The volume
test was doing double duty: it was also what held the feedback loop's gain at
zero. Writing the iteration out:

    L_hit  = min(albedo, c)/PI * (directE + bounceE)          [relight]
    E_next = PI * sum(w L) / sum(w)                           [blend]
    bounceE(x) = m(x) * sum(W_i E_i) / sum(W_i)               [gather]

both the blend's ratio estimator and the gather's weight normalization are
**convex averages**, and the margin only enters as a factor `m(x) ∈ [0, 1]`. So
in the sup norm

    ||E_next|| ≤ c * (||direct|| + ||E||),   c = u_DDGIEnergyConservation = 0.9

— a contraction with Lipschitz constant exactly `c`, **independent of the
margin**. The albedo clamp, not the volume test, is what makes the loop stable;
the volume test was only making it *dead*. The EMA on top is a convex
combination with the previous state and cannot increase that constant.

Two consequences follow from the same inequality and are implemented:

- The bounce path passes **intensity 1**, not `u_DDGIIntensity`. An artist gain
  inside the loop multiplies the Lipschitz bound to `c · intensity`, so an
  authored intensity above ~1.11 pushes it to 1 and the argument above stops
  **proving** contraction. That is weaker than saying it diverges — the true
  operator norm is bounded by `c · intensity` and is generally well under it
  (sky misses feed nothing back, the volume weight is ≤ 1, Chebyshev de-weights
  occluded probes), so such a volume might still settle. But a stability
  guarantee that an artist-facing knob can revoke is not one worth keeping.
  Kept out, the intensity scales the converged field linearly, which is what an
  intensity knob should do. (Latent before this change only because the loop
  was dead.)
- The bound says nothing about *where* the fixed point is, so a contraction to
  the wrong value is still a contraction — and a loop that stopped running
  would read as perfectly stable. `DDGIReferenceParityTest` asserts the settled
  field against the path tracer, and
  `DDGIReferenceParityTest.InfiniteBounceFeedbackConverges` runs 360 frames
  (~30 EMA time constants), asserting that a sampled probe settles, that the
  spread across the whole measured window is under 2%, that the peak texel
  **anywhere** in the atlas does not grow (a local instability would not show
  at a single probe), and that the settled value still carries multi-bounce
  energy against the direct-only reference.

  Measured: from frame 80 to frame 360 the sampled probe reads **0.378878** and
  the whole-atlas peak **1.3877** at every one of the 15 samples — not "within
  tolerance", bit-identical. The loop reaches an exactly representable fixed
  point in the FP16 atlas and stays on it.

**Measured, against the reference path tracer** (`DDGIReferenceParityTest`,
enclosed ~8x4x8 room, one point light, albedos 0.55-0.6). DDGI / full
multi-bounce ground truth, per probe:

| volume | before | after |
|---|---|---|
| fitted to the air (the natural authoring) | 0.41 – 0.61 | **0.79 – 0.89** |
| enclosing the wall slabs | 1.12 | **1.12** (unchanged, to three digits) |

The wall-enclosing case is handed the same one-spacing margin as everything
else — `BounceMarginScale` is uploaded unconditionally — and never uses it: its
hit points are inside the hard bounds, where `VolumeWeight` returns 1 before the
margin is consulted. So the fix is a no-op there, which is what makes it the
control. The air-fitted case still
lands 11–21% low, and that residual is the margin's smoothstep taper: its
surfaces sit 0.29–0.42 of a spacing outside the bounds and therefore gather at
0.6–0.8 weight, which compounds through the bounce series. Erring low rather
than high is the right side for a feedback loop. The residual is comparable to
the machinery's own 12% error at full coverage — smaller at three of the four
probes, larger at the fourth — so the two authorings now differ by roughly the
width of the approximation instead of by a factor of two.

**Options rejected.**

- *Authoring rule only* (document "enclose the surfaces you want bounce light
  from", warn in the editor). Zero risk and zero fix: it leaves a correct-looking
  volume silently producing half the light it should, and every scene already
  authored stays wrong. Shipped as a **companion**, not as the fix:
  `DDGIProbeUpdatePass::GetBounceCoverage()` is the **mean of `DDGI::VolumeWeight`
  over every captured hit point of every active probe** — i.e. the average of the
  attenuations the bounce gather actually applies, each hit point counting once.
  It is a proxy for how much of the bounce term survives, not that fraction
  itself: the irradiance a hit point contributes is also weighted by its radiance
  and its cosine term, and neither enters here. Good enough to answer "are these
  probes' surfaces inside the volume?", which is the authoring question. The
  Light Probe Volume inspector warns below 50%. It reads **0.762** for the
  `DDGIReferenceParityTest` air-fitted room, **1.000** for the wall-enclosing one,
  and **0** for the pre-fix behaviour (margin 0), which is the point: it turns a
  silent no-op into something an author can see. It returns **-1** when there is
  nothing to measure — no active probe, or no cached surface hit yet — because a
  diagnostic that reports "fine" when it means "unknown" is the failure mode it
  exists to fix.
- *Clamp the lookup with no bound.* Same arithmetic as the margin, minus the
  limit. It stays contractive, but it lets a probe shade an arbitrarily distant
  surface from a boundary probe — an extrapolation the field's own sample
  density cannot justify, and one that pumps indoor light onto outdoor geometry
  behind the volume. The margin is this option with the reach made explicit.

## Component / mode contract

`LightProbeVolumeComponent.m_Mode` (`Baked` | `Realtime` | `Hybrid`,
default `Baked` — zero behavior change for existing scenes):

- **Baked** — today's path: offline SH bake, static SSBO, trilinear sampler.
- **Realtime** — DDGI atlases only; the baked asset is ignored.
- **Hybrid** — baked SH provides the volume's irradiance while DDGI capture
  coverage converges (scalar blend by captured-probe fraction), then DDGI
  takes over; baked SH also remains the documented fallback when DDGI is
  disabled by renderer settings / quality tier.

One active volume at a time (the existing engine-wide contract) is kept.

## Consequences / risks

- The relight pass is the new per-frame cost center: `probes × hitTexels ×
  (CSM + culled local lights + prev-frame probe sample)`. At 2048 probes ×
  256 texels ≈ 0.5 M texels ≈ half an SSGI pass with simpler shading;
  classification skips dead probes and the relight budget bounds worst cases.
- Capture correctness must NOT be modeled on `LightProbeBaker`'s
  bind-local-FBO-then-readback pattern — the engine research flagged it as
  probably capturing black (the bug `ReflectionProbeBaker` fixed in 82f181e4
  but `LightProbeBaker` never got). The DDGI capture pass rasterizes with its
  own dedicated mini-G-buffer pass (ShadowRenderPass blueprint), not through
  `Scene::RenderScene3D`.
- The forward path's probe sampling is currently dead code
  (`CommandDispatch.cpp` hard-codes `EnableLightProbes=0` for forward);
  meeting the "both Deferred and Forward+" criterion requires wiring it,
  which slightly changes forward frames that previously never sampled probes
  (gated by the same settings toggle).
- Adding engine sampler slots for the atlases moves `TEX_SHADER_GRAPH_0`
  (must stay after all engine slots) — shader-graph shaders regenerate from
  the constant; serialized shader-graph assets that baked the old binding
  would need a regenerate.

## Considered options

- **Per-frame rasterized mini-cube capture, shaded at capture (issue's literal
  item 2).** Rejected: at any affordable budget, moving-light response decays
  to the round-robin period (seconds+) on real grids — fails the epic's core
  criterion; also 6 scene rasterizations per probe per frame is CPU
  draw-bound long before probe counts get interesting.
- **Static hit-point cache + per-frame relight (chosen).** Per-frame light
  response for all probes at bounded cost; the shipped-industry and
  Lumen-principled path; dynamic geometry receive-only is the accepted trade.
- **Compute-shader BVH ray tracing (Wicked Engine style).** Real DDGI, real
  dynamic geometry — but requires building and refitting a GPU BVH plus
  incoherent software traversal (CryEngine's Neon Noir needed a whole Vega 56
  for reflections alone); a miniature RTX stack is its own epic. The gather
  stages here stay ray-source-agnostic so a future tracer can drop into the
  same atlases.
- **SDF tracing (Lumen SWRT / Godot SDFGI).** No SDF infrastructure exists in
  this engine; the baker + streaming + global compositor + leak heuristics
  are engineer-months, and the issue already scopes SDFGI out.
