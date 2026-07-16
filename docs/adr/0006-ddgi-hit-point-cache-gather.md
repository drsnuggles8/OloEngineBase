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
mini-render, 6 cube faces resampled to octahedral by compute). Every frame, a
compute pass **relights all cached hit points** with current shadowed direct
lighting (reusing the CSM + local-light shadow atlas and the same PBRCommon
formulas the main paths use) **plus the previous frame's probe irradiance at
the hit point** (infinite bounce), then cosine-convolves the result into the
irradiance atlas under EMA hysteresis.

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
  radiance, albedo clamped (≤ 0.9) in the bounce term, relit radiance
  saturated, and a minimum hit distance before the previous-frame probe
  sample may be taken (Lumen's 10 cm `MinTraceDistanceToSampleSurface`
  anti-self-lighting rule).

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
