# OloEngine Renderer Test Documentation

> Reference documentation for the rendering test infrastructure.
> Last verified: **2119 tests across 349 suites, full run ≈ 34 s** on a developer box.

This document is the single source of truth for how the renderer is tested in
OloEngine — what we test, how we test it, where the code lives, and why each
layer exists. It is intended for:

- **Contributors** adding a new renderer feature (which tests do I add? where?)
- **Reviewers** deciding whether a PR is adequately covered
- **Maintainers** triaging a regression (which layer should have caught this?)
- **Newcomers** trying to understand the rendering codebase contracts

If a claim in this document disagrees with the code, the code wins and this
file should be updated in the same PR.

---

## Table of Contents

1. [Design principles](#1-design-principles)
2. [The testing pyramid](#2-the-testing-pyramid)
3. [Layer reference](#3-layer-reference)
    - 3.1 [Property / behavioural tests](#31-layer-1--property--behavioural-tests)
    - 3.2 [Shader unit tests](#32-layer-2--shader-unit-tests)
    - 3.3 [Data round-trip](#33-layer-3--data-round-trip)
    - 3.4 [GPU state validation](#34-layer-4--gpu-state-validation)
    - 3.5 [Render graph validation](#35-layer-5--render-graph--hazard-validation)
    - 3.6 [Performance regression](#36-layer-6--performance-regression)
    - 3.7 [Smoke / sanity readback](#37-layer-7--smoke--sanity-readback)
    - 3.8 [Golden image](#38-layer-8--golden-image)
    - 3.9 [Cross-vendor conformance](#39-layer-9--cross-vendor-conformance)
    - 3.10 [Automatic diagnostic escalation](#310-layer-10--automatic-diagnostic-escalation)
    - 3.11 [Sanitizers & fuzzing](#311-layer-11--sanitizers--fuzzing)
    - 3.12 [Current test catalogue (auto-generated)](#312-current-test-catalogue-auto-generated)
4. [How to add a new test](#4-how-to-add-a-new-test)
5. [How to debug a failure](#5-how-to-debug-a-failure)
6. [CI layout](#6-ci-layout)
7. [Known limitations and future work](#7-known-limitations-and-future-work)
8. [References](#8-references)

---

## 1. Design principles

These principles are the "why" behind every test in the renderer. They are
deliberately few and are applied consistently.

### 1.1 Verify *correctness*, not *similarity*

A pixel comparison tells you "something changed." A property test tells you
*what is wrong*. Example: a bloom pass that gained 3.2% energy produces the
assertion *"bloom energy ratio 1.032 exceeds tolerance (expected 1.0 ± 1%)"* —
actionable on its own, without RenderDoc or a reference image. We prefer
*invariant checks* (energy preserved, output monotone, black-in → black-out)
over goldens wherever the algorithm admits one.

### 1.2 Production code path, procedural inputs

Every test drives the *real* shader / render pass / framebuffer against inputs
generated in code. Never depend on authored scenes, textures, or meshes — a
change to the importer, material system, or camera would otherwise invalidate
hundreds of unrelated tests. Synthetic inputs isolate "the algorithm" from "the
content pipeline."

### 1.3 One test, one property

Each test pins exactly one contract: *roughness=0 is a mirror*, *metallic kills
diffuse*, *fog is zero at the camera*. When it fails you know precisely what
broke. Tests that assert five things at once are noise generators.

### 1.4 Tests live next to the code

Rendering tests live under `OloEngine/tests/Rendering/` and `OloEngine/tests/Rendering/PropertyTests/`,
not in a separate repo or editor workflow. Adding a test is a single C++ file
in the same build tree; no editor, no authored data, no golden-file management
by default.

### 1.5 Tests as documentation

Test names are contracts: `PbrFresnelTest.OutputMonotonicallyIncreasesWithAngle`,
`BloomChainEnergyTest.MultiPassDownUpPreservesTotalEnergy`,
`FogTest.DisabledEarlyOut`. Reading the test list should teach you the design
assumptions of each system.

### 1.6 Failures carry enough context to act

Every assertion message includes the relevant numerical context
(*ratio / max delta / per-channel stats / pixel coords*). On top of that, the
harness auto-captures GL state, framebuffer, and the most recent command
bucket (see [§3.10](#310-layer-10--automatic-diagnostic-escalation)). You
should never have to re-run a test with extra logging just to understand it.

---

## 2. The testing pyramid

The pyramid is oriented *away* from goldens and *toward* property tests,
following [Wronski's "Approach B"](https://bartwronski.com/2019/08/14/how-not-to-test-graphics-algorithms/).

```
             ┌──────────────────────────────┐
             │   Golden image (L8) — 4      │  ← few, integration-heavy,
             ├──────────────────────────────┤    cascaded RMSE → SSIM
             │   Cross-vendor (L9)          │
             │   Performance (L6)           │  ← slow, noisy,
             ├──────────────────────────────┤    handled nightly
             │   Render graph (L5)          │
             │   Smoke readback (L7)        │  ← fast structural checks
             ├──────────────────────────────┤
             │   Property / behavioural (L1) — primary surface
             │   Shader unit (L2)
             │   Data round-trip (L3)                              │
             │   GPU state validation (L4)                         │  ← most tests live here
             │   Sanitizers + fuzzing (L11)                        │
             └──────────────────────────────────────────────────────┘
```

- **L1–L4, L7, L11** run on every PR in under a minute.
- **L5** runs on every build — it's a construction-time check inside the
  RenderGraph.
- **L6** runs on every PR but only *asserts* on heavy regressions (<1.5× warn,
  ≥2.5× fail); the cross-machine trend line is evaluated nightly.
- **L8** runs on every PR with a forgiving RMSE → SSIM cascade so small
  vendor-driver drift never blocks the merge queue.
- **L9** runs nightly on Mesa llvmpipe.
- **L10** is not a test layer — it's the auto-capture helper that fires when
  any other layer fails.

---

## 3. Layer reference

Each section below has the same structure:

- **What it catches** — the bug classes this layer is designed to surface.
- **Where the code lives** — absolute-path links to the relevant files.
- **How it works** — the mechanics.
- **Current catalogue** — the actual tests shipping today.
- **Known limitations** — what this layer *can't* catch.

### 3.1 Layer 1 — Property / behavioural tests

**What it catches.** Algorithm correctness — every mathematical and physical
contract of a rendering algorithm. Bugs in BRDF formulas, tone-map curves,
shadow bias windows, post-process chains, splatmap blending, terrain normal
generation.

**Where the code lives.**

- [OloEngine/tests/Rendering/PropertyTests/PbrPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PbrPropertyTests.cpp)
- [OloEngine/tests/Rendering/PropertyTests/PostProcessPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PostProcessPropertyTests.cpp)
- [OloEngine/tests/Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp)
- Shared fixture: [OloEngine/tests/Rendering/PropertyTests/RenderPropertyTest.h](../OloEngine/tests/Rendering/PropertyTests/RenderPropertyTest.h)

**How it works.** A hidden-window GL 4.6 context is created on first use and
kept for the process. Tests call `OLO_ENSURE_GPU_OR_SKIP()` as their first
line, then generate a procedural input (uniform white, single bright pixel,
linear gradient, etc.), drive the production shader, read back the
framebuffer, and assert a numerical invariant. No scene loading, no editor,
no content pipeline.

**Current catalogue.**

| Domain | Test file | Representative contracts |
|---|---|---|
| **PBR / BRDF** | `PbrPropertyTests.cpp` | `PbrBrdfTest.FurnaceEnergyBoundViaMonteCarlo`, white-environment irradiance / prefilter, Fresnel normal / grazing / monotonicity / CPU reference, GGX NDF non-negativity / peak / roughness-1 / low-roughness, normal-map identity, metallic kills diffuse, prefilter mip energy monotonicity, prefilter mip 0 ≈ source |
| **Post-process** | `PostProcessPropertyTests.cpp` | Bloom energy conservation (multi-pass down/up), bloom black passthrough, tone-map monotonicity / black-to-black / NaN safety on extreme HDR, FXAA uniform no-op + hard-edge preservation, DOF focus identity + CoC linear-model sweep, vignette centre brighter than corners, chromatic aberration centre untouched, motion blur static-scene identity, fog disabled-early-out |
| **Shadows / terrain** | `ShadowTerrainPropertyTests.cpp` | `ShadowBiasTest.SelfShadowAndPeterPanningContract` (bias-window fully-lit / partially-lit / full-shadow transition via `ShaderUnit_ShadowSelfShadow.glsl`), cascaded-shadow bounds math, out-of-frustum short-circuits to lit, flat-heightmap produces up normal, splatmap channel isolation (4-layer `sampler2DArray` + 4-channel splatmap) |

**Failure message convention.** Every property assertion formats the measured
value, expected bound, and unit. Example actual output:

```
Bloom energy ratio 1.0324 exceeds 1.0 ± 1%
    (input sum = 10.0, output sum = 10.324)
```

**Limitations.** Doesn't test integration. A bloom pass that conserves energy
perfectly but reads from the wrong texture slot passes this layer; that's
what L8 (golden) and L4 (state validation) exist for.

### 3.2 Layer 2 — Shader unit tests

**What it catches.** Bugs inside a single GLSL function — math errors in
sRGB ↔ linear conversion, GGX distribution, octahedral normal packing, fog
falloff. Tested in isolation on tiny synthetic inputs via a dedicated probe
shader.

**Where the code lives.**

- [OloEngine/tests/Rendering/PropertyTests/ShaderUnitTests.cpp](../OloEngine/tests/Rendering/PropertyTests/ShaderUnitTests.cpp)
- Probe shaders: `OloEditor/assets/shaders/tests/ShaderUnit_*.glsl` (e.g.
  `ShaderUnit_SRGB.glsl`, `ShaderUnit_ToneMap.glsl`, `ShaderUnit_GGX.glsl`,
  `ShaderUnit_Octahedral.glsl`, `ShaderUnit_Fog.glsl`,
  `ShaderUnit_SplatmapChannel.glsl`, `ShaderUnit_ShadowSelfShadow.glsl`,
  `ShaderUnit_DofCoc.glsl`).

**How it works.** Approach A from the research notes: each probe shader calls
the real `.glsl` function under test with known inputs, writes the result to a
1×N texture, and the C++ side reads back + asserts. We use the *actual
compiled shader code on the actual GPU* rather than a CPU reimplementation —
this catches driver-level precision surprises that a dual-build CPU version
would miss.

**Current catalogue.** `ShaderUnitSrgbTest`, `ShaderUnitToneMapTest`,
`ShaderUnitGgxTest`, `ShaderUnitOctahedralTest`, `ShaderUnitFogTest`, plus
probe shaders consumed by L1 shadow / terrain / DOF / splatmap tests.

**Limitations.** Combinatorial explosion. We test the shared math primitives
in isolation; we do *not* enumerate every shader permutation. Permutation
coverage is a by-product of L1 tests actually exercising the real pass.

### 3.3 Layer 3 — Data round-trip

**What it catches.** Bugs in the binary / YAML serialisation pipeline for
textures, cubemaps, mesh binaries, and the IBL cache — format mismatches,
missing mips, dropped header fields, endianness issues, version regressions.

**Where the code lives.**

- [OloEngine/tests/Rendering/PropertyTests/DataRoundTripTests.cpp](../OloEngine/tests/Rendering/PropertyTests/DataRoundTripTests.cpp)

**How it works.** Procedurally generate a known bit pattern (gradient,
checkerboard, specific floating-point values), pass it through
`serialize → deserialize` (or `upload → readback`), assert equality within
the format's precision budget. Every test is deterministic; no authored data.

**Current catalogue.** RGBA8 identity round-trip, RGBA32F identity round-trip,
randomised stress round-trips for both formats, cubemap round-trip preserving
every mip face (regression test for an actual IBL cache bug that only loaded
mip 0), plus IBL-specific cache-key stability coverage.

**Limitations.** A value can round-trip perfectly and still bind to the wrong
texture slot at render time — that's for L4.

### 3.4 Layer 4 — GPU state validation

**What it catches.** State leakage between passes — shadow pass forgetting to
restore depth writes, `OpenGLFramebuffer::Bind()` unconditionally enabling
blend, prior commands leaking blend state into terrain callbacks. This is the
layer that pins OloEngine's historically-troubled state transitions.

**Where the code lives.**

- [OloEngine/src/OloEngine/Renderer/Debug/GLStateGuard.h](../OloEngine/src/OloEngine/Renderer/Debug/GLStateGuard.h)
- [OloEngine/tests/Rendering/PropertyTests/GLStateGuardTest.cpp](../OloEngine/tests/Rendering/PropertyTests/GLStateGuardTest.cpp)

**How it works.** `GLStateSnapshot::Capture()` queries the critical set of GL
state (depth / blend / stencil / cull / scissor / polygon-mode / viewport /
FBO / VAO / active program + 16 texture units + 16 UBO slots) via the
non-DSA `glGet*` path. `GLStateGuard` is the RAII wrapper a pass uses to
snapshot on entry and diff on exit; its `Policy` enum controls whether
violations log, assert, or are ignored, so guards can be rolled out
incrementally.

**Current catalogue.** `GLStateGuardTest.*` pins each tracked state: blend
(on/off + factors + equation), depth (test + mask + func), stencil (test +
ref + mask), cull mode + front-face, scissor, viewport, polygon mode, FBO
bindings, texture and UBO slot bindings.

**Limitations.** Only what you thought to track. Vendor-specific state (e.g.
NVIDIA-only flags) is invisible. Debug-only in production builds.

### 3.5 Layer 5 — Render graph / hazard validation

**What it catches.** Structural bugs in the frame's pass graph — cycles,
duplicate connections, out-of-order dependencies, and (the big one) resource
hazards: a texture written by pass A and read by pass B without the B-depends-
on-A edge.

**Where the code lives.**

- Resource-aware RDG: [OloEngine/src/OloEngine/Renderer/RenderGraph.h](../OloEngine/src/OloEngine/Renderer/RenderGraph.h) / [.cpp](../OloEngine/src/OloEngine/Renderer/RenderGraph.cpp)
  + [OloEngine/src/OloEngine/Renderer/ResourceHandle.h](../OloEngine/src/OloEngine/Renderer/ResourceHandle.h)
- Pass declarations via `RenderPass::DeclareRead` / `DeclareWrite` in
  `OloEngine/src/OloEngine/Renderer/RenderPass.h` and every subclass.
- Tests: [OloEngine/tests/Rendering/RenderGraphTest.cpp](../OloEngine/tests/Rendering/RenderGraphTest.cpp)
  and [OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp](../OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp).

**How it works.** Each pass declares the `ResourceHandle`s it reads and
writes. On connection, the graph records the edge list; `RenderGraph::
ValidateResourceHazards()` walks the transitive dependency closure and
detects RAW / WAW / WAR hazards. `Renderer3D` invokes the validator after
`SetFinalPass` so any production mis-wiring fails at construction time, not
at submit time.

**Current catalogue.** `RenderGraphTest.*` covers the baseline topology
cases (linear chain, diamond, independent passes, cycle detection); 5
`RenderGraphStructural.*` tests pin the production Shadow → Scene →
PostProcess → Final ordering, duplicate-connection idempotency, each-pass-
executes-once invariant, missing-pass connection safety; 12
`ResourceHazardValidationTests.*` cover RAW / WAW / WAR surfaces against
the real production API.

**Limitations.** Structural only. A graph that passes validation can still
produce wrong pixels — that's L1 and L8.

### 3.6 Layer 6 — Performance regression

**What it catches.** Microbenchmark-level slowdowns in individual post-process
passes (tone map, bloom threshold / downsample / upsample at 512²) **and**
pass-to-pass transition cost via a whole-frame post-process chain bench.

**Where the code lives.**

- [OloEngine/tests/Rendering/PropertyTests/PerfRegressionTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PerfRegressionTests.cpp)
- Baselines: [perf_baselines.txt](../OloEngine/tests/Rendering/PropertyTests/perf_baselines.txt)
- Historical TSVs: [perf_history/](../OloEngine/tests/Rendering/PropertyTests/perf_history/)
- Trend detector: [OloEngine/tests/scripts/perf_trend.py](../OloEngine/tests/scripts/perf_trend.py)

**How it works.** Each bench renders a fullscreen pass, times it via
`GL_TIME_ELAPSED` queries, takes the **minimum of 20 samples after 5 warmup
draws** (minimum is robust to thermal / scheduler noise). Policy is
`PASS < 1.5× baseline / WARN 1.5–2.5× / FAIL ≥ 2.5×` with a sanity ceiling of
100 ms. The bloom passes (typical cost 5–15 µs, very susceptible to
scheduler jitter) additionally route through `MeasureFullscreenPassStableNs`,
which re-measures **once** when the first sample trips WARN and keeps the
faster of the two. This retry only runs on bad samples; steady-state cost
is unchanged.

Every run appends a row to `perf_history/<machine>.tsv`. Machine tag resolves
`OLOENGINE_PERF_MACHINE → COMPUTERNAME → HOSTNAME → "unknown"`. Schema:
`iso_utc \t name \t measured_ns \t baseline_ns \t ratio`. `perf_trend.py`
computes min / median / p95 per (machine, benchmark) and runs a 3-sigma
drift detector that compares the last 30 samples against the prior 30 —
exits 1 when the recent median is more than three prior-window standard
deviations slower. The CI jobs in §3.9 invoke it as their post-run
verification step.

**Current catalogue.** Six benches — tone map, bloom threshold, bloom
downsample, bloom upsample (all at 512 × 512),
`whole_frame_postprocess_512x512` (post-process chain of the four passes
above), and `scene_draw_burst_512x512` (an integration-level state-change
budget: 64 fullscreen draws rotating through 4 shaders + 4 textures in a
single FBO, with pinned draw-call / shader-bind / texture-bind invariants
so silent batching regressions fail even when the timing margin is small).

**Limitations.** All benches remain fragment / fullscreen-pass oriented —
vertex-heavy or draw-call-explosion regressions in the *3D* pipeline
(ECS traversal, culling, material resolution) are currently only observed
indirectly via CPU-time cost on the full-scene smoke in L9. A fully
Renderer3D-driven frame budget would drag the whole scene-loading surface
into L6 and is intentionally out of scope; the burst bench above catches
the state-change-storm class of regression without that cost.

### 3.7 Layer 7 — Smoke / sanity readback

**What it catches.** Catastrophic failures — entire pass rendered black, NaN
propagation, infinite values spraying through the pipeline, alpha corruption,
HDR values exceeding fp16 range.

**Where the code lives.**

- Helper: [OloEngine/src/OloEngine/Renderer/Debug/RendererValidate.h](../OloEngine/src/OloEngine/Renderer/Debug/RendererValidate.h)
- Tests: [OloEngine/tests/Rendering/PropertyTests/RendererValidateTest.cpp](../OloEngine/tests/Rendering/PropertyTests/RendererValidateTest.cpp)

**How it works.** `RendererValidate::ReadFloatAttachmentStats(fb, index)`
reads back a float framebuffer attachment and returns an `AttachmentStats`
record (min / max / avg / NaN count / Inf count / per-channel RGBA maxima).
Tests pin the contract on synthetic inputs; the same helper is available in
production debug builds after any pass that can reasonably be spot-checked.

**Current catalogue.** `RendererValidateTest.*` cases exercising
`ReadFloatAttachmentStats`: `CleanFramebufferPassesValidation` (a solid
within-range clear has zero NaN/Inf and stats inside fp16 range),
`NanPixelsAreDetected` (an RGBA32F attachment seeded with NaN bumps the
`m_NanCount` field), `InfPixelsAreDetected` (same for +/-Inf),
`Fp16OverflowIsDetected` (a channel above `kFp16Max` fails validation), and
`EmptyAttachmentIsHandled` (a zero-sized attachment returns a
zero-initialised `AttachmentStats` without reading out of bounds).

**Limitations.** "Not black and not NaN" is a low bar. L7 is the safety net
under L1, not a replacement for it.

### 3.8 Layer 8 — Golden image

**What it catches.** Integration regressions — situations where every
individual pass is correct (L1 / L2 / L4 / L5 green) but the composed frame
still produces wrong pixels. State ordering across passes, unexpected
resource-slot collisions, depth / blend state interactions that only
manifest end-to-end.

**Where the code lives.**

- [OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp](../OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp)
- Baselines: `OloEditor/assets/tests/golden/*.png` (committed to the repo;
  regenerate with `OLOENGINE_GOLDEN_REBASE=1`).

**How it works — cascaded RMSE → SSIM.** On every run we read the rendered
frame, compute RGB RMSE over the reference, and decide:

| RMSE tier | Action |
|---|---|
| `RMSE < 0.004` | **Pass** fast-path — the frame is bit-for-bit close enough. |
| `RMSE > 0.02` | **Fail** hard — no amount of perceptual tolerance hides a regression this large. |
| `0.004 ≤ RMSE ≤ 0.02` | Escalate to **mean SSIM** (Wang / Bovik 2004) over 8 × 8 non-overlapping windows, C1 = (0.01 · 255)², C2 = (0.03 · 255)². Pass iff `SSIM ≥ 0.985`. |

This two-stage decision exists because **each metric alone is wrong**:

- **RMSE over-rates** isolated hot-pixel outliers (a 1-px specular highlight
  shift that's perceptually invisible).
- **SSIM over-tolerates** a uniform 2-LSB brightness shift that RMSE flags
  every pixel on.

The cascade gets the "surely pass" and "surely fail" cases in microseconds
and only runs the expensive SSIM on the ambiguous middle band. `~90%` of
runs resolve at stage 1 in practice.

The SSIM math itself is pinned by 4 standalone unit tests
(`GoldenImageSsimTest.*`: identity, tiny-shift tolerance, structural
destruction, symmetry) so the perceptual math can't silently rot.

**Current catalogue.**

| Golden | What it exercises |
|---|---|
| `GoldenImageTest.ReinhardHdrRampGolden` | HDR ramp → Reinhard tone map → RGBA8 (shader + `PostProcessUBO` binding + fullscreen draw + framebuffer readback + sRGB clamp). |
| `GoldenImageTest.FxaaHardEdgeGolden` | Hard diagonal edge through FXAA — pins edge displacement and shader permutation. |
| `GoldenImageTest.SceneShadowIntegrationGolden` | Shadow depth → shadow-probe shader → CPU-side sun / ambient lighting → tonemap. |
| `GoldenImageTest.SceneSplatmapIntegrationGolden` | 4-layer `sampler2DArray` + radial splatmap → splatmap-blend shader → tonemap. |

**On failure we emit.** `<name>.actual.png`, `<name>.diff.png` (red = worst-
channel delta × 8, green = mean-channel delta × 8), plus a text message
containing RMSE, SSIM (if escalated), worst-pixel coordinates + delta,
per-channel max (R/G/B), and the count of pixels with any channel delta > 4
LSBs. Combined with [§3.10](#310-layer-10--automatic-diagnostic-escalation)
this is usually enough to diagnose without re-running.

**Limitations.** Brittle across GPU vendors even with SSIM. Per-vendor
baseline sets remain a deferred follow-up (see [§7](#7-known-limitations-and-future-work)).

### 3.9 Layer 9 — Cross-vendor conformance

**What it catches.** Vendor-specific divergence — a bug that hides on NVIDIA
but manifests on Mesa / Intel / AMD (or vice versa). Precision differences,
fast-math reordering, driver extension quirks.

**Where the code lives.**

- [.github/workflows/cross-vendor.yml](../.github/workflows/cross-vendor.yml)

**How it works.** Nightly at 03:47 UTC + manual dispatch: the workflow
downloads `pal1000/mesa-dist-win 24.3.4`, drops its `opengl32.dll` next to
the test binary, sets `GALLIUM_DRIVER=llvmpipe`, `LIBGL_ALWAYS_SOFTWARE=1`,
`MESA_GL_VERSION_OVERRIDE=4.6`, tags the run with
`OLOENGINE_PERF_MACHINE=ci-llvmpipe-windows` +
`OLOENGINE_GOLDEN_VENDOR=llvmpipe`, runs the full suite, then invokes
[`OloEngine/tests/scripts/perf_trend.py`](../OloEngine/tests/scripts/perf_trend.py). On failure it uploads
the test output, the appended perf-history row, and any golden-image diffs
as CI artefacts.

**Limitations.** llvmpipe is a software rasteriser — it catches *spec*
divergence cleanly but says little about how a specific vendor driver
*actually* behaves on real silicon. Hardware-GPU coverage is a deferred
follow-up.

### 3.10 Layer 10 — Automatic diagnostic escalation

**What it catches.** Nothing on its own. L10 is the capture helper that
fires when *any other layer fails* so you have enough post-mortem evidence
to debug without re-running the test.

**Where the code lives.**

- [OloEngine/tests/Rendering/PropertyTests/TestFailureCapture.h](../OloEngine/tests/Rendering/PropertyTests/TestFailureCapture.h) /
  [.cpp](../OloEngine/tests/Rendering/PropertyTests/TestFailureCapture.cpp)
- Self-tests: [OloEngine/tests/Rendering/PropertyTests/TestFailureCaptureTest.cpp](../OloEngine/tests/Rendering/PropertyTests/TestFailureCaptureTest.cpp)
- Registered from: [OloEngine/tests/OloEngineTest.cpp](../OloEngine/tests/OloEngineTest.cpp)

**How it works.** A GoogleTest event listener is registered in the test
binary's `main()`. On `OnTestStart` it clears any stale capture directory
for the upcoming test. On the **first** failed assertion of a test it
invokes `CaptureAll()`, which writes — into
`OloEditor/assets/tests/captures/<suite>__<name>/` — four artefacts:

| Artefact | Contents |
|---|---|
| `metadata.txt` | Test id, ISO-UTC timestamp, GL vendor / renderer / version / GLSL, and the assertion summary. |
| `gl_state.txt` | Field-by-field dump of `GLStateSnapshot::Capture()` — the exact same snapshot type `GLStateGuard` uses for boundary validation, so the dump matches what §3.4 inspects. |
| `framebuffer.png` | Pixel-perfect RGBA readback of the currently bound draw FBO at its current viewport, Reinhard + gamma-encoded so HDR attachments are always visually useful. Rows are flipped so the PNG matches "what you'd see on screen." |
| `command_bucket.txt` | One-page summary of the most recent captured frame from [`FrameCaptureManager`](../OloEngine/src/OloEngine/Renderer/Debug/FrameCaptureManager.h) — pre/post-sort / post-batch command counts, sort / batch / execute timings. Only written when the manager has a recorded frame. |

The listener reuses OloEngine's *production* debug infrastructure — nothing
in the capture path is test-only except the listener glue. This keeps the
dumped state faithful to what the engine's debug overlay shows at runtime
and means adding a new tracked field to `GLStateSnapshot` automatically
propagates to failure dumps.

Subsequent failures in the same test are not re-captured; the first one
almost always identifies the cause. Tests pay **zero cost** when passing.

**Self-tests.** `TestFailureCaptureTest.*` (6 tests) exercise sanitisation of
filesystem-unsafe test names, metadata writing with / without a GL context,
the no-op path when `FrameCaptureManager` has no captures, GL-state dump
contents, PNG writing (including validating the 8-byte PNG signature on the
written file), and the full `CaptureAll()` artefact set.

**Limitations.** If a test crashes the process (SEH, segfault) the listener
never fires. For that class of failure you still have the GoogleTest crash
stack plus the artefacts from the *previous* test's run.

### 3.11 Layer 11 — Sanitizers & fuzzing

**What it catches.** Memory-safety bugs — buffer overruns, use-after-free,
data races, undefined behaviour in serialisation / deserialisation paths.
These never show up as a "wrong pixel"; they show up as a corrupt vertex
buffer, a torn mesh header, or a mysterious crash two hours into a
long-running test.

**Where the code lives.**

- Sanitizer CMake plumbing: [cmake/Sanitizers.cmake](../cmake/Sanitizers.cmake)
  (`OLO_ENABLE_ASAN`, `OLO_ENABLE_TSAN`, `OLO_ENABLE_UBSAN`).
- libFuzzer harnesses: [OloEngine/tests/Fuzzing/](../OloEngine/tests/Fuzzing/)
- Fuzz CMake gate: [OloEngine/tests/Fuzzing/CMakeLists.txt](../OloEngine/tests/Fuzzing/CMakeLists.txt) — toggled by `-DOLO_ENABLE_FUZZING=ON`; requires Clang / ClangCL (MSVC is early-returned).
- CI: [.github/workflows/fuzz.yml](../.github/workflows/fuzz.yml)

**How it works.** Each harness implements `LLVMFuzzerTestOneInput`, writes the
arbitrary fuzz-provided bytes to a PID-tagged temp file, and drives the real
production deserialiser against it. `-fsanitize=fuzzer,address,undefined` is
set when `OLO_ENABLE_FUZZING` and the compiler is Clang / ClangCL. Each
harness target has a per-target `<name>-smoke` sub-target that runs 30 s
against the checked-in corpus; the aggregate target is `OloEngine-FuzzSmoke`.

**Current harnesses.**

- `FuzzAnimationBinary` → `AnimationBinarySerializer::Read`.
- `FuzzMeshBinary` → `MeshBinarySerializer::Read`.
- `FuzzInputActionYaml` → `InputActionSerializer::Deserialize`.
- `FuzzImageDecoder` → `stb_image`'s `stbi_load_from_memory` / `stbi_load_16_from_memory` / `stbi_loadf_from_memory` (covers PNG / JPG / BMP / TGA / PSD / GIF / HDR decode paths).
- `FuzzSceneYaml` → `SceneSerializer::DeserializeFromYAML` (full component schema + yaml-cpp parser).
- `FuzzAssimpMesh` → `Assimp::Importer::ReadFileFromMemory` with rotating format hints (blank sniff + `obj` / `gltf` / `fbx` / `dae` / `blend`). Assimp has a long CVE history across MDL / MD2 / LWO / IFC / X / BVH parsers; this exercises the full import pipeline on each input.
- `FuzzSpirvCross` → `spirv_cross::CompilerGLSL` construction + `compile()`. Drives the SPIR-V binary parser + reflection + GL-decompile path that `OpenGLShader::CompileOrGetOpenGLBinaries` relies on at load time.

Each has a minimal seed corpus in `OloEngine/tests/Fuzzing/corpus/<target>/`.

**CI.** `fuzz.yml` builds on `windows-latest` with `-T ClangCL` +
`-DOLO_ENABLE_FUZZING=ON`, runs each harness for 30 s nightly at 04:13 UTC,
and uploads any crash / leak / timeout / oom artefacts on failure.

**Limitations.** 30 s per target nightly is a smoke budget — it catches
regressions, not deep bugs. An OSS-Fuzz integration for sustained campaigns
is a deferred follow-up.

---

## 3.12 Current test catalogue (auto-generated)

This is the authoritative view of every rendering-scope test file grouped
by layer. It is regenerated by
[`OloEngine/tests/scripts/generate_test_catalogue.py`](../OloEngine/tests/scripts/generate_test_catalogue.py)
from the mapping in
[`OloEngine/tests/scripts/test_catalogue.json`](../OloEngine/tests/scripts/test_catalogue.json).
Pre-commit runs the script in `--check` mode, which fails the hook if a
new `.cpp` file has been added under `OloEngine/tests/Rendering/`,
`OloEngine/tests/ShaderGraph/`, or `OloEngine/tests/Streaming/` without an
accompanying classification — this is the registration mechanism that
replaces manual doc edits.

<!-- BEGIN: auto-catalogue (generated by OloEngine/tests/scripts/generate_test_catalogue.py) -->

> **Do not edit by hand.** Generated from [test_catalogue.json](../OloEngine/tests/scripts/test_catalogue.json) by [generate_test_catalogue.py](../OloEngine/tests/scripts/generate_test_catalogue.py). Add new test files to the config and run the script (or pre-commit will run it with `--check`).

### L1 — [Property / behavioural tests](#31-layer-1--property--behavioural-tests)

| File | Tests | Cases |
|---|---:|---|
| [DeferredOverlayPassTests.cpp](../OloEngine/tests/Rendering/PropertyTests/DeferredOverlayPassTests.cpp) | 5 | **DeferredModelUBOLayout** &mdash; `StructMatchesStd140Expectations`, `PrevModelStoresMat4RoundTrip`<br/>**DeferredDrawMeshCommand** &mdash; `PrevTransformFieldIsPresent`, `PrevTransformRoundTripsThroughMemcpy`<br/>**ForwardOverlayRenderPassConstruction** &mdash; `DefaultConstructsAndExposesSetter` |
| [DeferredPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/DeferredPropertyTests.cpp) | 4 | **DeferredOctNormalTest** &mdash; `RoundTripKeepsHemisphereUnderOneDegree`, `EncodeOutputStaysInUnitRange`<br/>**DeferredSettingsTest** &mdash; `DefaultsMatchPlan`, `MSAASampleCountRoundTripsCommonValues` |
| [OITPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/OITPropertyTests.cpp) | 7 | **OITWeightTest** &mdash; `ClampBoundsAreRespected`, `MonotonicInAlpha`, `NearFragmentsOutweighFar`<br/>**OITResolveTest** &mdash; `SingleFragmentOpaqueMatchesForeground`, `SingleTransparentFragmentIsApproxOver`, `OrderIndependentForTwoFragments`, `EmptyAccumulationPreservesBackground` |
| [PbrPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PbrPropertyTests.cpp) | 17 | **PbrFresnelTest** &mdash; `NormalIncidenceEqualsF0`, `GrazingIncidenceApproachesOne`, `MonotonicallyDecreasingInCosTheta`, `MatchesCpuReference`<br/>**PbrNdfTest** &mdash; `NonNegativeAndFinite`, `PeaksAtHAlignedWithN`, `Roughness1HEqualsN_Equals_InvPi`, `LowRoughnessConcentratesHighlight`<br/>**PbrDiffuseTest** &mdash; `MetallicOneKillsDiffuse`, `DielectricDiffuseNonZero`<br/>**PbrBrdfTest** &mdash; `PositiveAndFiniteEverywhere`, `HelmholtzReciprocity`, `FurnaceIntegralWithinEnergyBounds`<br/>**PbrNormalMapTest** &mdash; `FlatNormalReturnsGeometricNormal`<br/>**PbrIrradianceTest** &mdash; `UniformWhiteYieldsNormalisedUnity`<br/>**PbrPrefilterTest** &mdash; `UniformWhiteYieldsUnityAtAllRoughness`, `MipChainEnergyIsMonotonicAndMip0ApproxSource` |
| [PostProcessPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PostProcessPropertyTests.cpp) | 17 | **ToneMapMonotonicityTest** &mdash; `ReinhardPreservesLuminanceOrdering`<br/>**ToneMapMonotonicityFixture** &mdash; `HdrRampIsNonDecreasing`<br/>**ToneMapBlackFixture** &mdash; `BlackInputStaysBlack`<br/>**ToneMapExtremeHdrFixture** &mdash; `ExtremeHdrProducesFiniteOutput`<br/>**VignettePropertyTest** &mdash; `CenterBrighterThanCorners`<br/>**ChromaticAberrationPropertyTest** &mdash; `CenterPixelUnaffected`<br/>**FxaaUniformInputTest** &mdash; `UniformInputIsNoOp`<br/>**FxaaEdgeDisplacementTest** &mdash; `EdgePreservesFlatRegions`<br/>**MotionBlurStaticTest** &mdash; `ZeroVelocityIsIdentity`<br/>**DofFocusTest** &mdash; `DepthAtFocusDistanceIsIdentity`, `CocLinearModelMatchesSweep`<br/>**BloomThresholdTest** &mdash; `BlackInputStaysBlack`<br/>**BloomDownsampleTest** &mdash; `UniformInputIsIdentity`<br/>**BloomUpsampleTest** &mdash; `UniformInputIsIdentity`<br/>**BloomCompositeTest** &mdash; `ZeroIntensityPassesSceneThrough`<br/>**BloomChainEnergyTest** &mdash; `MultiPassDownUpPreservesTotalEnergy`<br/>**FogDisabledTest** &mdash; `DisabledFlagProducesZeroInscatter`<br/>*Parametrised via 3 `INSTANTIATE_TEST_SUITE_P`* |
| [ShadowTerrainPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp) | 5 | **ShadowBoundsTest** &mdash; `BoundaryCasesShortCircuit`, `CascadeIndexSweepIsCorrect`<br/>**ShadowBiasTest** &mdash; `SelfShadowAndPeterPanningContract`<br/>**TerrainHeightmapTest** &mdash; `FlatHeightmapProducesUpNormal`<br/>**TerrainSplatmapTest** &mdash; `ChannelIsolationMapsToCorrectLayer` |

### L2 — [Shader unit tests](#32-layer-2--shader-unit-tests)

| File | Tests | Cases |
|---|---:|---|
| [ShaderCompilationTest.cpp](../OloEngine/tests/Rendering/PropertyTests/ShaderCompilationTest.cpp) | 1 | **ShaderCompilation** &mdash; `AllProductionShadersCompileUnderVulkanTarget` |
| [ShaderUnitTests.cpp](../OloEngine/tests/Rendering/PropertyTests/ShaderUnitTests.cpp) | 8 | **ShaderUnitSrgbTest** &mdash; `RoundTripWithinOneLsb`, `MidpointMatchesReference`<br/>**ShaderUnitToneMapTest** &mdash; `ReinhardMatchesReference`, `AcesMatchesReference`, `Uncharted2MatchesReference`<br/>**ShaderUnitGgxTest** &mdash; `HemisphereIntegralIsOne`<br/>**ShaderUnitOctNormalTest** &mdash; `RoundTripPreservesUnitNormals`<br/>**ShaderUnitFogTest** &mdash; `EndpointInvariants` |

### L3 — [Data round-trip](#33-layer-3--data-round-trip)

| File | Tests | Cases |
|---|---:|---|
| [DataRoundTripTests.cpp](../OloEngine/tests/Rendering/PropertyTests/DataRoundTripTests.cpp) | 5 | **DataRoundTripTest** &mdash; `Rgba32FGpuBitIdentity`, `Rgba8GpuByteIdentity`, `IblCacheCubemapRoundTripPreservesAllMips`, `RandomisedRgba32FStressRoundTrip`, `RandomisedRgba8StressRoundTrip` |

### L4 — [GPU state validation](#34-layer-4--gpu-state-validation)

| File | Tests | Cases |
|---|---:|---|
| [GLStateGuardTest.cpp](../OloEngine/tests/Rendering/PropertyTests/GLStateGuardTest.cpp) | 8 | **GLStateGuardTest** &mdash; `EmptyRegionHasNoLeaks`, `LeakedBlendIsDetected`, `LeakedDepthMaskIsDetected`, `LeakedDrawFboIsDetected`, `LeakedTextureBindingIsDetected`, `LeakedUboBindingIsDetected`, `MultipleLeaksAreAllReported`, `RestoredStateShowsNoLeaks` |
| [RenderStateTest.cpp](../OloEngine/tests/Rendering/RenderStateTest.cpp) | 33 | **RenderState** &mdash; `DefaultsAreCorrect`, `TriviallyCopyable`, `BlendedObjectsUseTransparentSortKey`, `InfiniteGridSortKeyIsTransparent`, `OpaqueObjectHasCorrectState`, `TransparentObjectHasCorrectState`, `TwoSidedMaterialDisablesCulling`, `WireframeModeUsesLinePolygonMode`, `PolygonOffsetForDecals`, `DepthOnlyPassDisablesColorMask`, `CubeWindingOrderIsCCW`, `CubeHas36Indices`, `CubeHas24Vertices`, `CubeNormalsAreUnitLength`, `CubeIndicesInRange`, `ShaderIncludeSkipsComments`, `ShaderIncludePathNoDuplication`, `WireframeCubeState`, `TransparentSphereState`, `PolygonOffsetOverlayState`, `BlendEnabledRequiresTransparentKey`, `OpaqueBeforeTransparentSortInvariant`, `StencilOutlinePassWriteState`, `StencilOutlinePassReadState`, `CylinderTopCapWindingIsCCW`, `CylinderBottomCapWindingIsCCW`, `CylinderSideWindingIsCCW`, `ConeBaseWindingIsCCW`, `ConeSideWindingIsCCW`, `TorusWindingIsCCW`, `MaterialBlendFlagDeterminesSortKeyType`, `TransparentDepthSortsBackToFront`, `BlendedObjectShouldNotWriteDepth` |

### L5 — [Render graph / hazard validation](#35-layer-5--render-graph--hazard-validation)

| File | Tests | Cases |
|---|---:|---|
| [RenderGraphTest.cpp](../OloEngine/tests/Rendering/RenderGraphTest.cpp) | 21 | **RenderGraph** &mdash; `AddPassMakesItRetrievable`, `GetPassReturnsNullForUnknown`, `GetAllPassesReturnsAll`, `LinearChainOrder`, `DiamondDependency`, `ExecutionDependencyOrdering`, `AllPassesPresentInOrder`, `IndependentPassesAllExecute`, `SetFinalPassIsFinalPass`, `GetConnectionsComplete`, `DuplicatePassNameOverwrites`, `MultipleExecuteIdempotent`, `SinglePassGraph`<br/>**RenderGraphStructural** &mdash; `ProductionPassOrderingAlwaysRespected`, `DuplicateConnectPassIsIdempotent`, `EachPassExecutesExactlyOncePerExecute`, `CycleIsDetectedAndDoesNotCrash`, `ConnectingToMissingPassDoesNotCorruptGraph`<br/>**RenderGraphResetTopology** &mdash; `ClearsPassesAndAllowsRebuild`, `PreservesPassReferenceOwnership`, `MultipleResetsAreSafe` |
| [ResourceHazardValidationTests.cpp](../OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp) | 20 | **RenderGraphResourceHazards** &mdash; `LinearChainWithHandoffIsHazardFree`, `ReadWithoutDependencyIsFlagged`, `ParallelWritesToSameResourceAreFlagged`, `WriteAfterReadWithoutDependencyIsFlagged`, `TransitiveDependencyCountsAsDependency`, `DiamondReadersOfSharedResourceIsHazardFree`, `ReadOnlyResourceHasNoHazards`, `SamePassReadAndWriteIsLegal`, `UndeclaredPassDoesNotContributeHazards`, `ProductionShapedGraphIsHazardFree`, `ProductionShapedGraphWithNoPathToShadowIsFlagged`, `IblProducerConsumerIsHazardFree`, `IblMissingDependencyIsFlagged`, `UICompositeInChainIsHazardFree`, `UICompositeSkippedByFinalIsFlagged`, `ResourceHandleEqualityIsNameBased`<br/>**RenderGraphConfigureTopology** &mdash; `ForwardPathIsHazardFree`, `ForwardPlusPathIsHazardFree`, `DeferredPathIsHazardFree`, `ResetTopologyAndRebuildAcrossPathsNoLeaks` |

### L6 — [Performance regression](#36-layer-6--performance-regression)

| File | Tests | Cases |
|---|---:|---|
| [CommandBucketBenchmarkTest.cpp](../OloEngine/tests/Rendering/CommandBucketBenchmarkTest.cpp) | 4 | **SortScalingTest** &mdash; `SortCompletesAndOrderIsValid`<br/>**CommandBucketBenchmark** &mdash; `ParallelSubmitAndMerge`, `AllocatorResetStability`, `LargeCommandMemoryPressure`<br/>*Parametrised via 1 `INSTANTIATE_TEST_SUITE_P`* |
| [PerfRegressionTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PerfRegressionTests.cpp) | 6 | **PerfRegressionTest** &mdash; `ToneMapPassTimingIsMeasurable`, `BloomThresholdPassTimingIsMeasurable`, `BloomDownsamplePassTimingIsMeasurable`, `BloomUpsamplePassTimingIsMeasurable`, `WholeFramePostprocessChainTimingIsMeasurable`, `SceneDrawBurstBudget` |

### L7 — [Smoke / sanity readback](#37-layer-7--smoke--sanity-readback)

| File | Tests | Cases |
|---|---:|---|
| [RendererValidateTest.cpp](../OloEngine/tests/Rendering/PropertyTests/RendererValidateTest.cpp) | 5 | **RendererValidateTest** &mdash; `CleanFramebufferPassesValidation`, `NanPixelsAreDetected`, `InfPixelsAreDetected`, `Fp16OverflowIsDetected`, `RejectsUnsupportedFormatsGracefully` |

### L8 — [Golden image](#38-layer-8--golden-image)

| File | Tests | Cases |
|---|---:|---|
| [GoldenImageTests.cpp](../OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp) | 8 | **GoldenImageSsimTest** &mdash; `IdenticalImagesYieldSsimOne`, `TinyUniformShiftKeepsSsimHigh`, `StructuralDestructionCollapsesSsim`, `SsimIsSymmetric`<br/>**GoldenImageTest** &mdash; `ReinhardHdrRampGolden`, `FxaaHardEdgeGolden`, `SceneShadowIntegrationGolden`, `SceneSplatmapIntegrationGolden` |

### plumbing — Pipeline plumbing (command bucket, dispatch, frame data)

| File | Tests | Cases |
|---|---:|---|
| [CommandAllocatorTest.cpp](../OloEngine/tests/Rendering/CommandAllocatorTest.cpp) | 18 | **CommandAllocator** &mdash; `AllocateReturnsNonNull`, `AllocateMultipleNonOverlapping`, `AlignmentIs16Byte`, `MultiBlockAllocation`, `ResetReusesMemory`, `CreateCommandPacketProducesValidPacket`, `AllocatePacketWithCommandPlacementNew`, `StressTestManyAllocations`, `AllocationCountTracksCorrectly`, `ConstantsAreSensible`, `ThreadCacheIsReusedAfterReset`, `MultiBlockResetDoesNotLeak`<br/>**ThreadLocalCache** &mdash; `SingleBlockForSmallAllocations`, `MultiBlockAllocationGrows`, `ResetReusesBlocksWithoutLeaking`, `ResetAndReallocateSameMemoryFootprint`, `OversizedAllocationGetsLargerBlock`, `AllocateReturnsAlignedPointers` |
| [CommandBucketTest.cpp](../OloEngine/tests/Rendering/CommandBucketTest.cpp) | 22 | **CommandBucketTest** &mdash; `SortPreservesAllCommands`, `SortOrderMatchesDrawKey`, `SortStabilityForEqualKeys`, `EmptyBucketSort`, `SingleCommandSort`, `ClearResetsState`, `ResetFreesMemory`, `SortReducesStateChanges`, `StatisticsTrackSubmissions`, `ParallelSubmissionMerge`, `MixedCommandTypes`, `TimingAccessors`, `DisabledSortingSkipsSort`<br/>**CommandBucketBatchTest** &mdash; `BatchConvertsMeshToInstanced`, `BatchRejectsDifferentRenderStateIndex`, `BatchAcceptsSameRenderStateIndex`, `BatchRejectsDifferentMaterialDataIndex`, `AnimatedMeshesAreNotBatched`, `HashTableGroupsNonAdjacentCommands`, `SingleCommandGroupsRemainDrawMesh`, `BatchRespectsMaxMeshInstances`, `BatchedTransformsAreContiguous` |
| [CommandDispatchTest.cpp](../OloEngine/tests/Rendering/CommandDispatchTest.cpp) | 8 | **CommandDispatch** &mdash; `DispatchTableIsComplete`, `InvalidTypeReturnsNull`, `CommandTypeToStringCoversAll`, `PODRenderStateFieldCount`<br/>**CommandDispatchIntegration** &mdash; `SetViewportDispatch`, `ClearDispatch`, `SetDepthTestDispatch`, `BucketExecuteDispatchesInOrder` |
| [CommandPacketTest.cpp](../OloEngine/tests/Rendering/CommandPacketTest.cpp) | 16 | **CommandPacket** &mdash; `InitializePopulatesType`, `InitializeSetsCommandType`, `DrawMeshPacketStoresShaderAndMaterialKeys`, `GetCommandDataReturnsCorrectType`, `CommandSizeMatchesType`, `MultiplePacketsAreIndependent`, `ComparisonFollowsDrawKeySorting`, `CanBatchWithSameTypeSameKeyFields`, `CannotBatchDifferentCommandTypes`, `CannotBatchDifferentRenderStateIndex`, `CanBatchSameRenderStateIndex`, `CloneDeepCopiesData`, `SortKeyPreservedFromMetadata`, `DefaultMetadataKeepsSortKeyZeroed`, `AllocatePacketWithCommandPath`, `MetadataDependencyAndStaticFlags` |
| [DrawKeyTest.cpp](../OloEngine/tests/Rendering/DrawKeyTest.cpp) | 18 | **DrawKey** &mdash; `BitwiseRoundTrip_Opaque`, `BitwiseRoundTrip_Transparent`, `BitwiseRoundTrip_Custom`, `OpaqueBeforeTransparent`, `OpaqueDepthFrontToBack`, `TransparentDepthBackToFront`, `TotalOrdering`, `SortStability`, `SettersAreIndependent`, `DefaultIsZero`, `ExplicitU64Constructor`, `EqualityOperator`, `HigherViewportSortsLast`, `ViewLayerOrderingWithinViewport`, `SameShaderGroupsTogether`, `ToStringViewLayerType`, `ToStringRenderMode`<br/>**DrawKeyFieldCombinations** &mdash; `PackingRoundTrip`<br/>*Parametrised via 1 `INSTANTIATE_TEST_SUITE_P`* |
| [FrameCaptureTest.cpp](../OloEngine/tests/Rendering/FrameCaptureTest.cpp) | 32 | **CapturedCommandData** &mdash; `ConstructionPreservesFields`, `TypedAccessRoundTrip`, `NullDataHandled`, `IsDrawCommand`, `IsStateCommand`, `IsBindCommand`, `GpuTimingAccessors`, `CopyIsIndependent`<br/>**FrameCaptureStats** &mdash; `DefaultsAreZero`<br/>**CapturedFrameData** &mdash; `DefaulConstruction`, `CanStoreCommandsAtMultipleStages`<br/>**FrameCaptureManager** &mdash; `InitialStateIsIdle`, `CaptureNextFrameTransition`, `StartStopRecording`, `MaxCapturedFramesConfig`, `ClearCaptures`, `SelectedFrameIndex`, `GetSelectedFrameReturnsNulloptWhenEmpty`<br/>**FrameCapturePipelineTest** &mdash; `CaptureNextFrameRecordsSingleFrame`, `CapturedCommandsPreserveTypes`, `RecordingCapturesMultipleFrames`, `MaxCapturedFramesTrimsOldest`, `PostSortOrderDiffersFromPreSort`, `DrawCallAndStateChangeStats`, `CaptureGenerationIncrements`<br/>**FrameExportTest** &mdash; `ExportToCSVCreatesValidFile`, `ExportToMarkdownCreatesValidFile`, `ExportToCSVWithNoSelectedFrameFails`, `ExportToMarkdownWithNoSelectedFrameFails`, `GenerateExportFilenameContainsFrameNumber`, `CSVContainsCorrectSortKeyData`, `MarkdownSortAnalysisPresent` |
| [FrameDataBufferTest.cpp](../OloEngine/tests/Rendering/FrameDataBufferTest.cpp) | 23 | **FrameDataBuffer** &mdash; `BoneMatrixWriteReadRoundTrip`, `TransformWriteReadRoundTrip`, `MultipleAllocationsNonOverlapping`, `ResetClearsState`, `AllocationFailsWhenFull`, `NoCapacityCreepOverResetCycles`, `StatisticsTrack`, `WorkerScratchIsolation`, `PrepareAndMergeCycle`, `RenderStateTableAllocateReturnsValidIndex`, `RenderStateTableDeduplicatesIdenticalStates`, `RenderStateTableDifferentStatesGetDifferentIndices`, `RenderStateTableRoundTrip`, `RenderStateTableResetsEachFrame`, `RenderStateTableMultipleUniqueStates`, `MaterialDataTableAllocateReturnsValidIndex`, `MaterialDataTableDeduplicatesIdenticalData`, `MaterialDataTableDifferentDataGetDifferentIndices`, `MaterialDataTableRoundTrip`, `MaterialDataTableResetsEachFrame`, `MaterialDataTableMultipleUniqueEntries`, `PBRMaterialAllTextureFieldsRoundTrip`, `PBRAndLegacyMaterialsDedupIndependently` |
| [FramePipelineTest.cpp](../OloEngine/tests/Rendering/FramePipelineTest.cpp) | 6 | **FramePipelineTest** &mdash; `OpaqueBeforeTransparent`, `AllSubmittedCommandsPresent`, `SortReducesShaderStateChanges`, `IsolatedBuckets`, `MultiFrameResetCycle`, `ViewLayerSortingPriority` |
| [PODCommandTest.cpp](../OloEngine/tests/Rendering/PODCommandTest.cpp) | 8 | **PODCommand** &mdash; `AllCommandsTrivialCopy`, `CommandSizeBound`, `DrawMeshFieldRoundTrip`, `ZeroInitNoNaN`, `PODRenderStateTrivialCopy`, `PODRenderStateDefaults`, `CommandTypeToStringCoverage`, `CommandHeaderDefault` |

### cullinglod — Culling, LOD, occlusion

| File | Tests | Cases |
|---|---:|---|
| [BoundingVolumeTest.cpp](../OloEngine/tests/Rendering/BoundingVolumeTest.cpp) | 17 | **BoundingBox** &mdash; `ConstructFromMinMax`, `CenterIsAverage`, `SizeIsCorrect`, `ExtentsAreHalfSize`, `ContainsCorners`, `UnionContainsBoth`, `TransformPreservesContainment`, `ConstructFromPoints`, `DegenerateZeroSizeBox`, `ConstructFromZeroPoints`<br/>**BoundingSphere** &mdash; `ConstructFromCenterRadius`, `ContainsCenter`, `ConstructFromBoundingBox`, `ConstructFromPoints`, `DegenerateZeroRadius`, `TransformPreservesContainment`, `ConstructFromZeroPoints` |
| [FrustumCullingTest.cpp](../OloEngine/tests/Rendering/FrustumCullingTest.cpp) | 20 | **Frustum** &mdash; `OriginVisibleInDefaultFrustum`, `PointBehindCameraNotVisible`, `PointBeyondFarPlaneNotVisible`, `PointBeforeNearPlaneNotVisible`, `FullyInsideSphereNeverCulled`, `FullyOutsideSphereAlwaysCulled`, `SphereTouchingPlaneIsVisible`, `BoundingSphereVisibility`, `BoxInsideFrustumVisible`, `BoxOutsideFrustumNotVisible`, `BoundingBoxVisibility`, `OrthographicFrustumCulling`, `UpdateChangesPlanes`, `PlaneNormalsAreNormalized`, `StressRandomSpheres_NoNaN`<br/>**Plane** &mdash; `ConstructFromNormalAndDistance`, `ConstructFromNormalAndPoint`, `SignedDistanceAbovePositive`, `SignedDistanceBelowNegative`, `SignedDistanceOnPlaneZero` |
| [LODTest.cpp](../OloEngine/tests/Rendering/LODTest.cpp) | 18 | **LODLevel** &mdash; `DefaultConstruction`, `ParameterizedConstruction`, `ParameterizedConstructionDefaultTriangles`<br/>**LODGroup** &mdash; `DefaultConstruction`, `EmptyGroupReturnsInvalid`, `SingleLevelAlwaysSelected`, `MultipleLevelsSelectCorrectly`, `BoundaryDistancesSelectCorrectLevel`, `BiasOneHasNoEffect`, `BiasGreaterThanOneKeepsHighDetailLonger`, `BiasLessThanOneFavorsLowerDetail`, `VeryHighBiasAlwaysSelectsHighestDetail`, `VeryLowBiasAlwaysSelectsLowestDetail`, `ZeroDistanceSelectsFirstLevel`, `NegativeDistanceTreatedAsVeryClose`, `VeryLargeDistanceSelectsLastLevel`, `AllLevelsSameDistanceSelectsFirst`, `ManyLevelsCorrectSelection` |
| [MeshOptimizationTest.cpp](../OloEngine/tests/Rendering/MeshOptimizationTest.cpp) | 28 | **MeshOptimization** &mdash; `OptimizeMeshPreservesVertexCount`, `OptimizeMeshPreservesTriangleContent`, `OptimizeMeshHandlesEmptyMesh`, `OptimizeMeshRemapsBoneInfluences`, `GenerateLODReducesTriangles`, `GenerateLODReturnsNullForEmptyMesh`, `GenerateLODPreservesVertexValidity`, `GenerateLODWithVeryLowRatioProducesMinimalMesh`, `GenerateShadowIndicesProducesSameCount`, `GenerateShadowIndicesValidIndices`, `GenerateShadowIndicesHandlesEmptyMesh`, `OptimizeMeshGeneratesShadowIndices`, `GenerateLODWithAttributesReducesTriangles`, `GenerateLODWithAttributesHandlesEmptyMesh`, `AnalyzeMeshReturnsValidStats`, `AnalyzeMeshHandlesEmptyMesh`, `OptimizedMeshHasBetterCacheStats`, `GenerateMeshletsProducesOutput`, `GenerateMeshletsRespectsLimits`, `GenerateMeshletsHandlesEmptyMesh`, `MeshletBoundsHavePositiveRadius`, `SpatialSortPreservesGeometry`, `SpatialSortHandlesEmptyMesh`, `EncodeDecodeVertexBufferRoundTrip`, `EncodeDecodeIndexBufferRoundTrip`, `EncodeVertexBufferCompresses`, `ShadowIndicesAreSpatialSorted`, `AttributeAwareLODProducesValidOutput` |
| [OcclusionIntegrationTest.cpp](../OloEngine/tests/Rendering/OcclusionIntegrationTest.cpp) | 17 | **OcclusionIntegration** &mdash; `DrawMeshCommandDefaultQueryIndex`, `DrawMeshCommandQueryIndexRoundTrip`, `DrawMeshCommandStillTriviallyCopyable`, `DrawMeshCommandSizeBound`, `ParticleSphereInsideFrustum`, `ParticleSphereBehindCamera`, `ParticleSpherePartiallyIntersecting`, `ParticleSphereBeyondFarPlane`, `FoliageBoundsInsideFrustum`, `FoliageBoundsBehindCamera`, `FoliageBoundsFarAway`, `PerInstanceCullingFiltersCorrectly`, `PerInstanceCullingAllVisible`, `PerInstanceCullingNoneVisible`, `BoundingBoxCenterExtentsForProxy`, `BoundingBoxAsymmetricProxy`, `BoundingSphereFromBoundingBox` |
| [OcclusionStateTest.cpp](../OloEngine/tests/Rendering/OcclusionStateTest.cpp) | 19 | **OcclusionState** &mdash; `DefaultConstruction`, `MutableFields`<br/>**OcclusionStateManagerTest** &mdash; `GetOrCreateNewState`, `GetOrCreateReturnsSameState`, `HasReturnsFalseForUnknown`, `HasReturnsTrueAfterCreate`, `RemoveDeletesState`, `RemoveNonExistentNoOp`, `MultipleObjects`, `AllocateSequential`, `AllocateRespectsMaxQueries`, `FreeAndReallocate`, `RemoveFreesQueryIndex`, `RemoveWithNoQueryNoFreeListCorruption`, `FrameCounterStartsAtZero`, `BeginFrameIncrementsCounter`, `ClearResetsFrameCounter`, `SimulateTemporalCoherence`, `StressAllocFree` |

### shaderpipe — Shader pack / binding layout / ShaderGraph

| File | Tests | Cases |
|---|---:|---|
| [ShaderBindingLayoutTest.cpp](../OloEngine/tests/Rendering/ShaderBindingLayoutTest.cpp) | 26 | **ShaderBindingLayout** &mdash; `CameraUBOAlignment`, `LightUBOAlignment`, `MaterialUBOAlignment`, `PBRMaterialUBOAlignment`, `ModelUBOAlignment`, `AnimationUBOAlignment`, `MultiLightUBOAlignment`, `ShadowUBOAlignment`, `TerrainUBOAlignment`, `BrushPreviewUBOAlignment`, `FoliageUBOAlignment`, `DecalUBOAlignment`, `IBLParametersUBOAlignment`, `TerrainUBOSizeStable`, `BrushPreviewUBOSizeStable`, `FoliageUBOSizeStable`, `DecalUBOSizeStable`, `UBOBindingSlotUniqueness`, `TextureSlotUniqueness`, `SSBOSlotUniqueness`, `AnimationConstantsConsistency`, `ShaderConstantGeneratorRoundTrip`, `KnownUBOBindingRecognized`, `UnknownUBOBindingRejected`, `AllTextureSlotsWithinGLMinimum`, `UBOGetSizeMatchesSizeof` |
| [ShaderPackTest.cpp](../OloEngine/tests/Rendering/ShaderPackTest.cpp) | 11 | **ShaderPackTest** &mdash; `LoadValid`, `ContainsQuery`, `GetShaderNames`, `LoadEntryValid`, `LoadEntryNotFound`, `LoadNonexistentFile`, `LoadInvalidMagic`, `LoadWrongVersion`, `LoadTruncatedFile`, `DefaultConstructor`, `EmptyPack` |
| [ShaderGraphCommandTest.cpp](../OloEngine/tests/ShaderGraph/ShaderGraphCommandTest.cpp) | 11 | **ShaderGraphCommandTest** &mdash; `AddNodeExecuteAddsNode`, `AddNodeUndoRemovesNode`, `AddNodeRedoRestoresNode`, `RemoveNodeUndoRestoresNodeAndLinks`, `AddLinkUndoRemovesLink`, `RemoveLinkUndoRestoresLink`, `MoveNodeUndoRestoresPosition`, `HistoryCanUndoCanRedo`, `NewCommandClearsRedoStack`, `HistoryClearResetsStacks`, `MultipleUndoRedoCycles` |
| [ShaderGraphCompilerTest.cpp](../OloEngine/tests/ShaderGraph/ShaderGraphCompilerTest.cpp) | 13 | **ShaderGraphCompilerTest** &mdash; `EmptyGraphFailsCompilation`, `MinimalGraphCompiles`, `OutputContainsVertexAndFragmentSections`, `OutputContainsMRTLayout`, `FloatParameterExposed`, `UBOBlockGenerated`, `MathNodeGeneratesCode`, `TextureSamplerBindingGenerated`, `MinimalComputeGraphCompiles`, `ComputeGraphWithBuffersCompiles`, `ComputeGraphCustomWorkgroupSize`, `ComputeGraphIsNotPBR`, `PBRGraphIsNotCompute` |
| [ShaderGraphSerializationTest.cpp](../OloEngine/tests/ShaderGraph/ShaderGraphSerializationTest.cpp) | 12 | **ShaderGraphSerializationTest** &mdash; `SerializeProducesNonEmptyYAML`, `RoundTripPreservesGraphName`, `RoundTripPreservesNodeCount`, `RoundTripPreservesLinkCount`, `RoundTripPreservesParameterName`, `RoundTripPreservesNodeTypes`, `DeserializeInvalidYAMLReturnsFalse`, `DeserializeMissingRootNodeReturnsFalse`, `DeserializedGraphIsDirty`, `DeserializedGraphCanCompile`, `RoundTripPreservesComputeWorkgroupSize`, `RoundTripPreservesBufferBinding` |
| [ShaderGraphTest.cpp](../OloEngine/tests/ShaderGraph/ShaderGraphTest.cpp) | 24 | **ShaderGraphTest** &mdash; `AddNodeAndFindIt`, `RemoveNodeCleansUpLinks`, `FindPinAcrossNodes`, `AddLinkConnectsCompatiblePins`, `AddLinkRejectsIncompatibleTypes`, `InputPinCanOnlyHaveOneLink`, `WouldCreateCycleDetectsCycle`, `WouldCreateCycleAllowsValidLink`, `ValidateEmptyGraphIsInvalid`, `ValidateGraphWithOutputNodeIsValid`, `ValidateGraphWithMultipleOutputsIsInvalid`, `TopologicalOrderPutsOutputLast`, `NodeFactoryCreatesAllRegisteredTypes`, `NodeFactoryReturnsNullForUnknown`, `ComputeOutputNodeCreation`, `ComputeBufferNodeCreation`, `ComputeInvocationIDNodes`, `ValidateComputeGraphIsValid`, `ValidateMixedOutputNodesIsInvalid`, `FindOutputNodeReturnsComputeOutput`<br/>**ShaderGraphTypeTest** &mdash; `FloatBroadcastsToVectors`, `Vec4TruncatesToVec3AndVec2`, `IncompatibleTypesCannotConvert`, `GenerateTypeConversionProducesValidGLSL` |

### integration — Feature-level integration tests (outside PropertyTests/)

| File | Tests | Cases |
|---|---:|---|
| [ComputeShaderAndSSBOTest.cpp](../OloEngine/tests/ComputeShaderAndSSBOTest.cpp) | 6 | **StorageBufferTest** &mdash; `StorageBufferIsRefCounted`, `StorageBufferHasPureVirtualAPI`<br/>**ComputeShaderTest** &mdash; `ComputeShaderIsAbstract`, `ComputeShaderInheritsRendererResource`, `ComputeShaderAssetType`<br/>**MemoryBarrierFlagsTest** &mdash; `FlagCombination` |
| [LightCullingTest.cpp](../OloEngine/tests/LightCullingTest.cpp) | 12 | **ForwardPlus** &mdash; `GPUPointLightSize`, `GPUSpotLightSize`, `ForwardPlusUBOSize`, `DefaultGridConfig`, `TileCountCalculation`, `TileCountCalculationNonMultiple`, `TileCountWith32pxTiles`, `SSBOBindingsDontConflict`, `UBOBindingIsUnique`, `ModeEnumValues`, `PointLightPacking`, `SpotLightPacking` |
| [MorphTargetTest.cpp](../OloEngine/tests/MorphTargetTest.cpp) | 27 | **MorphTargetTest** &mdash; `DefaultConstruction`, `NamedConstruction`, `ConvertToSparse`, `ConvertToDense`<br/>**MorphTargetSetTest** &mdash; `AddAndFindTarget`, `GetVertexCount`<br/>**MorphTargetEvaluatorTest** &mdash; `ZeroWeightsReturnBase`, `SingleTargetFullWeight`, `HalfWeight`, `MultipleTargetsAdditive`, `SparseEvaluation`<br/>**MorphTargetComponentTest** &mdash; `SetAndGetWeight`, `WeightClamping`, `ResetAllWeights`, `HasActiveWeights`, `GetOrderedWeights`<br/>**FacialExpressionLibraryTest** &mdash; `RegisterAndApply`, `ApplyWithBlend`, `BlendBetweenExpressions`<br/>**MorphTargetKeyframeTest** &mdash; `AnimationClipStoresMorphKeyframes`<br/>**MorphTargetSystemTest** &mdash; `SampleMorphKeyframesAtExactKeys`, `SampleMorphKeyframesInterpolated`, `SampleMorphKeyframesMultipleTargets`, `EmptyClipDoesNotCrash`, `EvaluateMorphTargetsReturnsFalseWithNoActiveWeights`, `EvaluateMorphTargetsAppliesWeights`<br/>**MorphTargetGPUvsCPUTest** &mdash; `CPUReferenceMatchesExpected` |
| [PostProcessTest.cpp](../OloEngine/tests/PostProcessTest.cpp) | 15 | **PostProcessSettings** &mdash; `DefaultsAreReasonable`, `BloomParameterRanges`, `DOFParameterRanges`, `MotionBlurParameterRanges`<br/>**PostProcessUBOData** &mdash; `SizeIs80Bytes`, `DefaultsMatchSettings`, `FieldOffsets_Std140Compatible`<br/>**MotionBlurUBOData** &mdash; `SizeIs128Bytes`, `DefaultsAreIdentityMatrices`<br/>**ShadowUBO** &mdash; `SizeConsistency`, `FieldLayout`<br/>**TonemapOperator** &mdash; `ValuesMatchGLSLDefines`<br/>**ShaderBindingLayout** &mdash; `PostProcessUBOSlot`, `PostProcessTextureSlots`, `MotionBlurUBOSlot` |
| [RenderingRegressionTest.cpp](../OloEngine/tests/Rendering/RenderingRegressionTest.cpp) | 5 | **RenderingRegression** &mdash; `ShadowUBOIntFieldsDefaultZeroed`, `WaterWavelengthMustBePositive`, `WaterWavelengthCopied`, `CascadeDebugDefaultDisabled`, `PBRMaterialUBOHasIBLField` |
| [SelectionOutlineTest.cpp](../OloEngine/tests/Rendering/SelectionOutlineTest.cpp) | 28 | **SelectionOutlineUBO** &mdash; `SizeIs304Bytes`, `SizeIs16ByteAligned`, `MaxSelectedEntitiesIs64`, `FieldOffsets_Std140Compatible`, `DefaultOutlineColorIsOrange`, `DefaultSelectedCountIsZero`, `DefaultOutlineWidthIsOne`, `SelectedIDsDefaultToNegativeOneSentinel`, `BindingSlotIs27`, `PackSingleEntityID`, `Pack64EntityIDs`<br/>**SelectionOutlineGraph** &mdash; `PassInsertedBetweenPostProcessAndUIComposite`, `TopologicalOrderRespectsChain`<br/>**JumpFloodUBO** &mdash; `SizeIs48Bytes`, `SizeIs16ByteAligned`, `BindingSlotIs29`, `FieldOffsets_Std140Compatible`, `DefaultOutlineColorIsOrange`, `DefaultThicknessValues`, `DefaultStepIsOne`<br/>**JFAStepSequence** &mdash; `PassCount1ProducesSingleStep`, `PassCount2ProducesCorrectSteps`, `PassCount3ProducesCorrectSteps`, `PassCount4ProducesCorrectSteps`, `PassCount0ClampsTo1`, `PassCount5ClampsTo4`, `NegativePassCountClampsTo1`, `AllStepsArePowersOfTwo` |
| [WaterRenderingTest.cpp](../OloEngine/tests/Rendering/WaterRenderingTest.cpp) | 16 | **WaterRendering** &mdash; `WaterUBOAlignment`, `WaterUBOSizeStable`, `WaterUBOGetSizeMatchesSizeof`, `WaterUBOBindingSlot`, `WaterUBOKnownBinding`, `WaterUBOFieldRoundTrip`, `DrawWaterCommandTrivialCopy`, `DrawWaterCommandSizeBound`, `DrawWaterCommandZeroInitNoNaN`, `WaterComponentDefaults`, `WaterComponentCopyOmitsRuntime`, `WaterComponentAssignmentOmitsRuntime`, `DrawWaterCommandTypeExists`, `WavelengthDefaultsNonZero`, `WavelengthPackedIntoWaveDir`, `WaterTextureBindingSlots` |
| [ShadowMapTest.cpp](../OloEngine/tests/ShadowMapTest.cpp) | 33 | **CascadeSplitTest** &mdash; `MonotonicallyIncreasing`, `CoversNearFarRange`, `LambdaZeroIsUniform`, `LambdaOneIsLogarithmic`, `ClampedMaxShadowDistance`, `AllSplitsPositive`, `DifferentLambdasProduceDifferentDistributions`<br/>**ShadowMapMatrixTest** &mdash; `ComputeCSMCascadesProducesValidMatrices`, `CascadePlaneDistancesMonotonicallyIncrease`, `CascadeFarPlaneCappedByMaxShadowDistance`, `CSMMatricesProjectKnownPointToValidNDC`, `SpotLightShadowProducesValidPerspectiveMatrix`, `SpotLightIndexOutOfRangeIsIgnored`, `PointLightProduces6FaceMatrices`, `PointLightShadowParamsStorePositionAndRange`, `PointLightIndexOutOfRangeIsIgnored`, `PointLightFaceMatricesProject90DegreeFOV`, `BeginFrameResetsPerFrameState`, `CascadeDebugToggle`<br/>**ShadowSettingsTest** &mdash; `DefaultValues`, `SetSettingsUpdatesValues`, `EnableDisableToggle`<br/>**ShadowUBOTest** &mdash; `StructSizeIsNonZero`, `StructSizeMultipleOf16`, `MaxConstants`, `DefaultInitializationZeroed`<br/>**Texture2DArrayTest** &mdash; `IsRefCounted`, `IsAbstract`<br/>**Texture2DArraySpecificationTest** &mdash; `DefaultValues`, `FormatEnumValues`<br/>**ShaderBindingLayoutTest** &mdash; `ShadowUBOBinding`, `ShadowTextureBindings`, `ShadowBindingsDoNotConflict` |
| [SlugFontRenderingTest.cpp](../OloEngine/tests/SlugFontRenderingTest.cpp) | 9 | **SlugDataTest** &mdash; `GlyphLookupReturnsNullForMissing`, `GlyphLookupFindsInserted`, `GetAdvanceUsesKerning`, `GetAdvanceWithoutKerning`<br/>**SlugFontProcessorTest** &mdash; `GlyphBoundsAreValid`, `ExtractCurvesProducesNonEmptyForVisibleGlyphs`, `SpaceGlyphHasNoCurves`<br/>**SlugGlyphRenderDataTest** &mdash; `DefaultValuesAreZero`<br/>**SlugCurveTest** &mdash; `ControlPointStorage` |
| [SphericalHarmonicsTest.cpp](../OloEngine/tests/SphericalHarmonicsTest.cpp) | 17 | **SphericalHarmonicsTest** &mdash; `ZeroClearsAllCoefficients`, `AccumulateAddsCoefficients`, `ScaleMultipliesAllCoefficients`, `GPULayoutRoundtripPreservesData`, `GPULayoutValidityFlag`, `GPULayoutUnusedWComponentsAreZero`, `BasisFunctionDCTermIsConstant`, `BasisFunctionLinearTermsMatchDirection`, `BasisFunctionOppositeDirections`, `ConstantLightProducesConstantIrradiance`, `IrradianceIsNonNegative`, `DirectionalLightHigherInLitDirection`, `SizeConstants`<br/>**LightProbeVolumeComponentTest** &mdash; `TotalProbeCount`, `TotalProbeCountSingle`, `GridIndexLinearization`, `WorldToGridCorners` |
| [SceneStreamingTest.cpp](../OloEngine/tests/Streaming/SceneStreamingTest.cpp) | 18 | **StreamingSettings** &mdash; `DefaultValues`, `HysteresisGuarantee`<br/>**StreamingVolumeComponent** &mdash; `DefaultValues`, `ManualMode`<br/>**StreamingRegion** &mdash; `DefaultState`, `StateTransitions`, `BoundsStorage`, `LRUFrameTracking`, `EntityUUIDs`<br/>**SceneStreamerConfig** &mdash; `DefaultValues`, `CustomValues`<br/>**StreamingRegionSerializer** &mdash; `ParseInvalidYAML`, `MetadataRoundTrip`<br/>**RegionMetadata** &mdash; `DefaultValues`<br/>**SceneStreamer** &mdash; `DefaultConstruction`, `InitWithoutScene`, `ConfigAccessors`<br/>**StreamingActivationMode** &mdash; `EnumValues` |

### meta — [Test-framework self-tests](#310-layer-10--automatic-diagnostic-escalation)

| File | Tests | Cases |
|---|---:|---|
| [TestFailureCaptureTest.cpp](../OloEngine/tests/Rendering/PropertyTests/TestFailureCaptureTest.cpp) | 6 | **TestFailureCaptureTest** &mdash; `DirectoryForSanitizesNames`, `MetadataIsWrittenWithoutGlContext`, `LatestFrameSummaryIsNoOpWithoutCaptures`, `GLStateSnapshotIsWrittenWhenGpuAvailable`, `FboPngIsWrittenWhenGpuAvailable`, `CaptureAllProducesExpectedArtefacts` |

**Totals.** 50 rendering-scope test files, 733 TEST / TEST_F / TEST_P declarations across all layers. Non-rendering subsystems (audio, networking, gameplay, save-game, &hellip;) are catalogued separately and are not listed here by design.

<!-- END: auto-catalogue -->

---

## 4. How to add a new test

Decision tree for a new feature:

1. **Does the feature have a mathematical / physical contract?**
   *(e.g. energy conservation, monotonicity, output bounds, identity cases)*
   → **Layer 1 (property)**. New test under [PropertyTests/](../OloEngine/tests/Rendering/PropertyTests/).

2. **Is the feature a single GLSL function / primitive?**
   → **Layer 2 (shader unit)**. Add a probe shader to
   `OloEditor/assets/shaders/tests/ShaderUnit_<Feature>.glsl` and a test case
   in `ShaderUnitTests.cpp`.

3. **Does the feature add a serialisation format or extend an existing one?**
   → **Layer 3 (round-trip)** in `DataRoundTripTests.cpp`, plus **L11
   (fuzzer)** if the format is consumed from disk.

4. **Does the feature change pass-boundary GPU state?**
   → **Layer 4 (state guard)**. Add a `GLStateGuard` in the pass's
   `Execute()` + pin the contract in `GLStateGuardTest.cpp`.

5. **Does the feature add a new RenderGraph pass?**
   → **Layer 5**. Declare reads / writes on the pass, then add a structural
   test in `RenderGraphTest.cpp` / `ResourceHazardValidationTests.cpp` if the
   pass introduces a new resource dependency pattern.

6. **Is the feature a new post-process shader that needs perf tracking?**
   → **Layer 6**. Add a bench in `PerfRegressionTests.cpp` and run once with
   `OLOENGINE_PERF_REBASE=1` to seed the baseline. Use
   `MeasureFullscreenPassStableNs` if the pass is sub-microsecond.

7. **Is the feature an integration path spanning ≥ 3 passes?**
   → **Layer 8 (golden)**. Add one golden. Do **not** add a golden for
   per-feature correctness — that's L1's job.

8. **Is the feature exposed via a loader / parser consuming untrusted input?**
   → Add an **L11 libFuzzer harness** under
   `OloEngine/tests/Fuzzing/FuzzMyLoader.cpp` and a minimal seed corpus.

If you genuinely can't articulate a property, monotonicity, invariant, or
range bound — that's a signal the algorithm isn't sufficiently understood yet.
Write the property test *first*; it will teach you the contract.

---

## 5. How to debug a failure

1. **Read the assertion message.** OloEngine's assertions carry the
   numerical context (ratio, deltas, coordinates). Often enough on its own.

2. **Look under `OloEditor/assets/tests/captures/<suite>__<test>/`.**
   [§3.10](#310-layer-10--automatic-diagnostic-escalation) auto-wrote
   `metadata.txt`, `gl_state.txt`, `framebuffer.png`, and (when available)
   `command_bucket.txt`. The framebuffer PNG answers "what did the GPU
   actually produce?"; the state dump answers "what pipeline state was
   active when it failed?".

3. **For golden failures, open the diff PNG.** Red pixels = worst-channel
   delta hotspots, green pixels = broad drift. That tells you whether it's
   a localised shader bug or a global colour / gamma regression.

4. **Check lower layers first.** If L1 property tests also fail, fix the
   property bug — the golden failure is a symptom, not a cause.

5. **If the change is intentional**, rebase baselines in the *same commit*:

   - Goldens: `OLOENGINE_GOLDEN_REBASE=1 ...\OloEngine-Tests.exe
     --gtest_filter="GoldenImageTest.*"`.
   - Perf: `OLOENGINE_PERF_REBASE=1 ...\OloEngine-Tests.exe
     --gtest_filter="PerfRegressionTest.*"`.

   Never blind-rebase. Look at the diff PNG and understand why pixels
   changed before committing.

6. **Reproduce locally.** The fixture is process-global and deterministic —
   filtering to one test reproduces the failure exactly:

   ```powershell
   pushd OloEditor
   ..\build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter="BloomChainEnergyTest.*"
   popd
   ```

---

## 6. CI layout

| Workflow | Trigger | What it runs |
|---|---|---|
| **Standard PR build** | every push | L1–L8, L10, L11 (existing) via `run-tests-debug` task. Must pass to merge. |
| **cross-vendor.yml** ([workflow](../.github/workflows/cross-vendor.yml)) | nightly 03:47 UTC + manual dispatch | Full suite against Mesa llvmpipe; uploads perf history + golden diffs on failure; runs `perf_trend.py` as post-step. |
| **fuzz.yml** ([workflow](../.github/workflows/fuzz.yml)) | nightly 04:13 UTC + manual dispatch | Clang-cl build with `-DOLO_ENABLE_FUZZING=ON`; each libFuzzer harness runs for 30 s; uploads crash / leak / timeout artefacts on failure. |

Local developer workflow:

```powershell
# Full suite
cmake --build build --target OloEngine-Tests --config Debug --parallel
pushd OloEditor; ..\build\OloEngine\tests\Debug\OloEngine-Tests.exe; popd

# Property tests only (fast, no scene loading)
pushd OloEditor
..\build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter="*Test.*:-*Benchmark*"
popd
```

Perf baselines are developer-machine local — `OLOENGINE_PERF_REBASE=1`
refreshes them, and the machine-tagged TSV under
[`perf_history/`](../OloEngine/tests/Rendering/PropertyTests/perf_history/)
is gitignored so no local seeding leaks into other developers' runs.

---

## 7. Known limitations and future work

These are deliberately documented as *non-blocking* follow-ups; none of them
gate current coverage. Every item here requires external infrastructure that
cannot live inside this repo.

- **Hardware-GPU cross-vendor matrix.** Today L9 covers llvmpipe-on-CI. A
  self-hosted runner set for NVIDIA / AMD / Intel silicon would catch
  vendor-specific driver regressions that software rasterisers hide.
  Requires a self-hosted runner provisioned per vendor — pure ops work,
  no code change on our side beyond wiring the second job.

- **Per-vendor golden baseline sets.** The cascade is tuned against a single
  NVIDIA baseline. Once the first hardware-vendor divergence is reviewed,
  flip on `OLOENGINE_GOLDEN_VENDOR` and commit a per-vendor baseline
  directory. Unblocked the moment the cross-vendor matrix above is live.

- **OSS-Fuzz continuous fuzzing.** L11 currently runs a nightly 30 s
  smoke per harness, which catches regressions on known seed corpora but
  will not discover deep bugs. OSS-Fuzz integration would run the same
  harnesses continuously on Google's fuzzing farm with coverage-guided
  seed-corpus mutation. The harnesses themselves are already OSS-Fuzz
  compatible — enabling it is a project-onboarding step, not a code change.

All in-repo code-level gaps the design document originally called out have
shipped. The design principles in [§1](#1-design-principles) are fully
honoured by the current coverage.

---

## 8. References

Primary sources the strategy draws from:

- [**How (not) to test graphics algorithms** — Bart Wronski (2019)](https://bartwronski.com/2019/08/14/how-not-to-test-graphics-algorithms/) — the foundational essay for [§3.1](#31-layer-1--property--behavioural-tests), [§3.8](#38-layer-8--golden-image), and the general "property > golden" pyramid orientation.
- [**The Furnace Test** — Brian Karis (SIGGRAPH 2013)](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf) — origin of PBR energy-conservation tests used in `PbrBrdfTest.FurnaceEnergyBoundViaMonteCarlo`.
- **Wang, Bovik, Sheikh, Simoncelli (2004)**, *Image Quality Assessment: From Error Visibility to Structural Similarity* — defines the SSIM formulation used in the L8 cascade (C1 / C2 constants).
- [**Filament Test Infrastructure**](https://github.com/google/filament/tree/main/test) — reference for the "shared GLSL / C++ math" pattern in L2 and the headless-GL approach.
- [**Mesa piglit**](https://gitlab.freedesktop.org/mesa/piglit) — GL conformance reference; the cascaded RMSE / perceptual-metric pattern in L8 is a simplified version of piglit's tolerance model.
- [**Vulkan CTS (dEQP)**](https://github.com/KhronosGroup/VK-GL-CTS) — the per-function shader precision tests in L2 take their probe-shader approach from dEQP's methodology.
- [**libFuzzer**](https://llvm.org/docs/LibFuzzer.html) — the `LLVMFuzzerTestOneInput` interface underpinning L11.
- [**Google Sanitizers**](https://github.com/google/sanitizers/wiki) — reference for the ASan / TSan / UBSan plumbing in `cmake/Sanitizers.cmake`.
- [**Mesa 3D for Windows**](https://github.com/pal1000/mesa-dist-win) — the llvmpipe / OpenGL driver drop used by the cross-vendor CI.

Secondary / practitioner references:

- **Rendering Testing at Scale** (Unity, GDC 2019) — scaling golden-image test suites in a commercial engine.
- **Automated Visual Testing for Games** (GDC 2023) — practical golden-image workflows and CI.
- **id Tech internal benchmark methodology** — fixed-camera whole-frame budgets, inspiration for the L6 chain bench and scene-draw-burst state-change budget.
