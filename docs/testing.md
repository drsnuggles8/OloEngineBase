# OloEngine Testing

> Single source of truth for how OloEngine is tested. Covers the philosophy,
> the two test axes (renderer pyramid + Functional / cross-subsystem),
> per-layer reference, and the auto-generated test catalogues.

This document is intended for:

- **Contributors** writing a new test — what should it look like, where does
  it go, what counts as good?
- **Reviewers** judging whether a PR's coverage is adequate.
- **Maintainers** triaging a regression — which layer should have caught it?
- **Newcomers** reading the test list to learn what each subsystem promises.

If a claim in this document disagrees with the code, the code wins and this
file should be updated in the same PR. Operational rules — the registration
contract, pre-commit checks, the add-a-test workflow — live in
[docs/agent-rules/testing-architecture.md](agent-rules/testing-architecture.md).

---

## Table of Contents

1. [Philosophy](#1-philosophy)
2. [The two test axes](#2-the-two-test-axes)
3. [The value heuristic](#3-the-value-heuristic)
4. [Anti-patterns](#4-anti-patterns)
5. [Where does my new test go?](#5-where-does-my-new-test-go)
6. [Renderer testing pyramid](#6-renderer-testing-pyramid)
    - 6.1 [Design principles](#61-design-principles)
    - 6.2 [The pyramid](#62-the-pyramid)
    - 6.3 [Layer reference (L1–L11)](#63-layer-reference)
7. [Functional / cross-subsystem axis](#7-functional--cross-subsystem-axis)
    - 7.1 [What it catches](#71-what-it-catches)
    - 7.2 [The fixture and its contracts](#72-the-fixture-and-its-contracts)
    - 7.3 [Authoring a Functional test](#73-authoring-a-functional-test)
    - 7.4 [Known limitations](#74-known-limitations)
8. [CI layout](#8-ci-layout)
9. [Test catalogues (generated)](#9-test-catalogues-generated)
10. [References](#10-references)

---

## 1. Philosophy

Tests exist to **prevent regressions of contracts we actually depend on**.
Three corollaries fall out of that:

1. **A test must name a contract.** "Constructor doesn't crash" is not a
   contract; the linker already enforces it. "`Inventory::AddItem` merges
   stack-compatible items into one slot" is a contract.
2. **A test must fail when the contract breaks.** If you can edit the
   production code and the test still passes, the test is decoration, not
   defence. The test that inlines the algorithm under test and then
   asserts on the inlined copy is the canonical mistake — it cannot fail
   when the real implementation changes.
3. **A test must be cheap to maintain.** Expensive maintenance looks like
   "fires on every reorder," "sleeps to force a clock tick," or "I have
   to update twenty type permutations to add a new numeric type."

A candidate test passing all three is worth writing. Failing any of them
is a signal to either rework the test or skip it.

---

## 2. The two test axes

OloEngine tests live on two independent axes that share only the GoogleTest
harness and the registration contract:

| Axis | Catches | Where | Doc section |
|---|---|---|---|
| **Renderer pyramid (L1–L11)** | Rendering-pipeline contracts — math, shader semantics, GL state, render graph, goldens, perf, fuzz | `OloEngine/tests/Rendering/`, `ShaderGraph/`, `Streaming/`, classified by layer ID in `test_catalogue.json` | [§6](#6-renderer-testing-pyramid) |
| **Functional / cross-subsystem** | Bugs at the seams between two or more subsystems, driven by a real `Scene::OnUpdateRuntime` | `OloEngine/tests/Functional/<Subsystem>/`, tagged `"Functional"` in `test_catalogue.json` | [§7](#7-functional--cross-subsystem-axis) |

```text
  Renderer pyramid (11 layers)                         Functional axis
  =====================================================    ==========================
  L1 / L2 / L3 / L4 / L5 / L6 / L7 / L8 / L9 / L10 / L11   Cross-subsystem world-tick
  visual-pipeline contracts                                Scene::OnUpdateRuntime seams
  pixels, GPU state, shader math                           Animation × Physics × …
```

The two axes are independent. A feature can require zero renderer tests
and multiple Functional tests (e.g. an AI behaviour tree), or many
renderer tests and zero Functional tests (e.g. a new tone-map curve).
Most engine features that touch gameplay state will land tests on both.

Outside both axes, a small set of subsystem-scoped unit-test files cover
CPU primitives — concurrency (`Async/`, `Tasks/`), containers
(`Containers/`), audio DSP, animation math, networking wire format. These
are not part of either taxonomy but are catalogued under their owning
subsystem directory; new ones land in the matching subsystem folder.

---

## 3. The value heuristic

Run a candidate test through these five questions before writing it. If you
can't answer two or more affirmatively, the test is probably not worth
adding.

1. **What contract does it pin?** State it in one sentence ending with
   "always", "never", or "exactly". If you can't, you're writing a smoke
   test.
2. **Can I edit the production code to make the test fail?** If no, the
   test is decorative.
3. **Could a `static_assert` do this at compile time?** If yes,
   `static_assert` it. Don't pay runtime + CMake + catalogue + CI cost
   for a compile-time fact.
4. **What's the smallest scope that exercises the contract?** Don't spin
   up a `Scene` to test integer arithmetic. Don't build an isolated unit
   harness to test "Animation drives Physics in the same frame."
5. **Will it stay stable when the implementation changes around it?** If
   the test fires on any reasonable refactor (enum reorder, adding a
   struct field, switching `std::vector` to `flat_map`), it pins
   incidental detail, not contract.

---

## 4. Anti-patterns

Recognisable shapes that look like tests but produce little defence. New
tests reproducing any of these should be rejected at review.

### 4.1 Defaults-sanity check

Asserts that a settings struct's default-constructed values match what
the header says. Pins design choices, not invariants — fires on any
header edit, surfaces no real bug.

```cpp
TEST(MySettings, DefaultsAreReasonable)
{
    MySettings s;
    EXPECT_GT(s.MaxParticles, 0u);
    EXPECT_FLOAT_EQ(s.Friction, 0.1f);  // <-- pins exactly what the header says
    // ... 20 more EXPECT_*
}
```

**Fix.** Delete it. If a particular default has cascading effects elsewhere
(e.g. a sentinel value like `UINT32_MAX` for "no query allocated"), pin
that specific contract at the consumer, not at the struct's defaults.

### 4.2 Inline-reimplemented algorithm

The test inlines the loop being tested instead of calling the production
function, and asserts on its own local computation.

```cpp
TEST(System, LerpConverges)
{
    f32 current = 0.0f, target = 1.0f, speed = 2.0f, dt = 0.016f;
    for (int i = 0; i < 1000; ++i)
        current = std::lerp(current, target, std::clamp(speed * dt, 0.0f, 1.0f));
    EXPECT_NEAR(current, target, 0.001f);
}
```

The test cannot fail when the real `System::Tick` changes — it never
calls `System::Tick`. Pure liability.

**Fix.** Call the production code. If the production code can't run in
the test (no GL context, requires `Application::Get()`), refactor the
production code to expose the algorithm via a headless-callable type,
*then* test it.

### 4.3 Static-assert in disguise

A `TEST(...)` body that contains only `static_assert(...); SUCCEED();`.
Costs runtime registration, CMake entry, catalogue entry, CI time — for a
fact already enforced at compile time.

```cpp
TEST(MyType, ConstantsAreSensible)
{
    static_assert(MyType::SIZE >= 4096u);
    static_assert(MyType::ALIGN % 16 == 0);
    SUCCEED();
}
```

**Fix.** Move the `static_assert` to a `_compile.cpp` translation unit or
directly into the production header. Delete the GoogleTest wrapper.

### 4.4 Opt-in perf, always-on smoke

Timing assertion gated behind an env var. When the env var is unset
(every CI run, every developer who doesn't know about it), the test
degrades to "something ran." In neither mode does it pull on a useful
contract.

**Fix.** Commit to L6 perf machinery — record a baseline in
`perf_baselines.txt`, use the anti-flake retry helper, fail on
regression. If perf isn't worth that investment, the test isn't worth
keeping.

### 4.5 Lifecycle smoke

`Init returns true, IsInitialized is true, Shutdown returns false.` Tells
you the flag bit flips. Real lifecycle bugs (double-init races,
partial-shutdown leaks, init-after-shutdown) aren't surfaced because no
real resource is owned.

**Fix.** Write contention / leak / state-machine tests against the
resources the system actually owns. If the subsystem owns nothing real,
the lifecycle test has nothing to defend.

### 4.6 Value-stability brittleness

`EXPECT_EQ(static_cast<i32>(SomeEnum::Foo), 17)`. Fires on any enum
reorder even though no on-disk format actually depends on the integer
value.

**Fix.** If the integer value is load-bearing (it ends up on disk or on
the wire), test the *serialiser* against a pinned blob. If it isn't,
delete the test. Std140 / UBO offset assertions are the *right* form of
this pattern — those pin GPU memory layout that has a hard external
contract.

### 4.7 Type-permutation padding

A test repeated once per typed overload (`GetInt8 / GetUInt8 / GetInt16
/ ...` × `Generation / InRange / Reproducibility`). One parameterised
typed test (`TYPED_TEST_P`) covers the same ground in a fraction of the
lines.

**Fix.** Collapse into `TYPED_TEST_P` or a single TEST body that walks
all the types. Type-template behaviour is invariant in the type
parameter; you don't need to re-prove it for each one.

### 4.8 Vendor port re-test

A 1:1 port of upstream tests for code that is a 1:1 port of upstream
source. The tests re-prove the upstream's contract; we don't catch our
own regressions because we don't edit ported code in ways that break
it.

**Fix.** Keep a thin "we wired this up correctly" smoke per primitive
and rely on the vendor's CI for the rest. Save the test budget for the
glue code we actually author.

### 4.9 Scene-adjacent integration without the Functional fixture

The test manually constructs a `Scene`, builds entities, hooks audio /
animation / physics — but doesn't inherit `FunctionalTest` so it loses
the determinism contract, the headless guarantee, and the `RunFrames`
/ `TickFor` / `TickUntil` ergonomics.

**Fix.** Inherit `OloEngine::Functional::FunctionalTest`, tag the test
`"Functional"` in `test_catalogue.json`, replace manual ticking with
harness helpers.

### 4.10 Top-level one-test file

A whole `.cpp` for one test. Forces a `CMakeLists.txt` entry and a
catalogue entry per concept; encourages "every new thing gets its own
file" sprawl.

**Fix.** Roll into an omnibus subsystem file. A new piece of math
goes into the closest existing math-test file, not its own.

---

## 5. Where does my new test go?

A short decision tree. Walk it for every PR that adds engine code.

```text
Does the change touch the rendering pipeline (shaders, render passes,
GPU resources, command bucket)?
├── YES → at least one renderer-pyramid layer applies. Use §6.3.
│         Default: L1 property test + (L8 golden if it produces pixels).
└── NO → not in the pyramid.

Does the change cross subsystem boundaries (Animation × Physics ×
Scripting × Networking × Audio × Asset × Nav × Save-game × Gameplay)?
├── YES → write a Functional test under OloEngine/tests/Functional/
│          <Subsystem>/. Tag "Functional" in test_catalogue.json. See §7.3.
└── NO → it's a single-subsystem CPU primitive.

Is the contract enforceable at compile time (type traits, layout sizes,
enum-to-string)?
├── YES → static_assert it. Don't write a runtime test for compile-time
│          facts (anti-pattern §4.3).
└── NO → write a focused unit test, scoped to the subsystem.

         Place under OloEngine/tests/<Subsystem>/ if the subsystem has a
         dedicated subdir (Async/, Tasks/, Networking/, Audio/, Memory/,
         Containers/, Templates/). A top-level one-test file is wrong
         (anti-pattern §4.10).
```

Cross-axis features are common (e.g. adding a new component type often
needs L4 *and* Functional). Pin contracts on each axis the change crosses;
don't collapse them into one mega-test — the taxonomy is how failures get
diagnosed.

---

## 6. Renderer testing pyramid

The pyramid pins contracts of the **rendering pipeline only**: math,
shader semantics, CPU↔GPU round-trip, GL state, render graph, perf, NaN
smoke, goldens, cross-vendor, auto-diagnostics, sanitizers + fuzzing.

### 6.1 Design principles

These principles are the "why" behind every renderer test.

**Verify *correctness*, not *similarity*.** A pixel comparison tells you
"something changed." A property test tells you *what is wrong*. Example:
a bloom pass that gained 3.2% energy produces the assertion
*"bloom energy ratio 1.032 exceeds tolerance (expected 1.0 ± 1%)"* —
actionable on its own, without RenderDoc or a reference image. We prefer
*invariant checks* (energy preserved, output monotone, black-in →
black-out) over goldens wherever the algorithm admits one.

**Production code path, procedural inputs.** Every test drives the
*real* shader / render pass / framebuffer against inputs generated in
code. Never depend on authored scenes, textures, or meshes — a change
to the importer, material system, or camera would otherwise invalidate
hundreds of unrelated tests. Synthetic inputs isolate "the algorithm"
from "the content pipeline."

**One test, one property.** Each test pins exactly one contract:
*roughness=0 is a mirror*, *metallic kills diffuse*, *fog is zero at
the camera*. When it fails you know precisely what broke.

**Tests live next to the code.** Rendering tests live under
`OloEngine/tests/Rendering/` and `OloEngine/tests/Rendering/PropertyTests/`,
not in a separate repo or editor workflow. Adding a test is a single C++
file in the same build tree; no editor, no authored data, no golden-file
management by default.

**Tests as documentation.** Test names are contracts:
`PbrFresnelTest.OutputMonotonicallyIncreasesWithAngle`,
`BloomChainEnergyTest.MultiPassDownUpPreservesTotalEnergy`,
`FogTest.DisabledEarlyOut`. Reading the test list should teach you the
design assumptions of each system.

**Failures carry enough context to act.** Every assertion message
includes the relevant numerical context (*ratio / max delta /
per-channel stats / pixel coords*). On top of that, the harness
auto-captures GL state, framebuffer, and the most recent command bucket
(see L10). You should never have to re-run a test with extra logging
just to understand it.

### 6.2 The pyramid

The pyramid is oriented *away* from goldens and *toward* property tests,
following [Wronski's "Approach B"](https://bartwronski.com/2019/08/14/how-not-to-test-graphics-algorithms/).

```text
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
- **L6** runs on every PR but only *asserts* on heavy regressions (<1.5×
  warn, ≥2.5× fail); the cross-machine trend line is evaluated nightly.
- **L8** runs on every PR with a forgiving RMSE → SSIM cascade so small
  vendor-driver drift never blocks the merge queue.
- **L9** runs nightly on Mesa llvmpipe.
- **L10** is not a test layer — it's the auto-capture helper that fires
  when any other layer fails.

### 6.3 Layer reference

Each layer section has the same structure: what it catches, where the
code lives, how it works, current catalogue, known limitations.

#### L1 — Property / behavioural tests

**What it catches.** Algorithm correctness — every mathematical and
physical contract of a rendering algorithm. Bugs in BRDF formulas,
tone-map curves, shadow bias windows, post-process chains, splatmap
blending, terrain normal generation.

**Where the code lives.**

- [`OloEngine/tests/Rendering/PropertyTests/PbrPropertyTests.cpp`](../OloEngine/tests/Rendering/PropertyTests/PbrPropertyTests.cpp)
- [`OloEngine/tests/Rendering/PropertyTests/PostProcessPropertyTests.cpp`](../OloEngine/tests/Rendering/PropertyTests/PostProcessPropertyTests.cpp)
- [`OloEngine/tests/Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp`](../OloEngine/tests/Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp)
- Shared fixture: [`OloEngine/tests/Rendering/PropertyTests/RenderPropertyTest.h`](../OloEngine/tests/Rendering/PropertyTests/RenderPropertyTest.h)

**How it works.** A hidden-window GL 4.6 context is created on first
use and kept for the process. Tests call `OLO_ENSURE_GPU_OR_SKIP()` as
their first line, then generate a procedural input (uniform white,
single bright pixel, linear gradient, etc.), drive the production
shader, read back the framebuffer, and assert a numerical invariant.
No scene loading, no editor, no content pipeline.

**Failure message convention.** Every property assertion formats the
measured value, expected bound, and unit. Example actual output:

```text
Bloom energy ratio 1.0324 exceeds 1.0 ± 1%
    (input sum = 10.0, output sum = 10.324)
```

**Limitations.** Doesn't test integration. A bloom pass that conserves
energy perfectly but reads from the wrong texture slot passes this
layer; that's what L8 (golden) and L4 (state validation) exist for.

#### L2 — Shader unit tests

**What it catches.** Bugs inside a single GLSL function — math errors
in sRGB ↔ linear conversion, GGX distribution, octahedral normal
packing, fog falloff. Tested in isolation on tiny synthetic inputs
via a dedicated probe shader.

**Where the code lives.**

- [`OloEngine/tests/Rendering/PropertyTests/ShaderUnitTests.cpp`](../OloEngine/tests/Rendering/PropertyTests/ShaderUnitTests.cpp)
- Probe shaders: `OloEditor/assets/shaders/tests/ShaderUnit_*.glsl`
  (e.g. `ShaderUnit_SrgbRoundTrip.glsl`, `ShaderUnit_ToneMap.glsl`,
  `ShaderUnit_GgxIntegral.glsl`, `ShaderUnit_OctNormal.glsl`,
  `ShaderUnit_Fog.glsl`, `ShaderUnit_SplatmapChannel.glsl`,
  `ShaderUnit_ShadowSelfShadow.glsl`, `ShaderUnit_DofCoc.glsl`).

**How it works.** Approach A from Wronski: each probe shader calls the
real `.glsl` function under test with known inputs, writes the result
to a 1×N texture, and the C++ side reads back + asserts. We use the
*actual compiled shader code on the actual GPU* rather than a CPU
reimplementation — this catches driver-level precision surprises that
a dual-build CPU version would miss.

**Limitations.** Combinatorial explosion. We test the shared math
primitives in isolation; we do *not* enumerate every shader permutation.
Permutation coverage is a by-product of L1 tests actually exercising
the real pass.

#### L3 — Data round-trip

**What it catches.** Bugs in the binary / YAML serialisation pipeline
for textures, cubemaps, mesh binaries, and the IBL cache — format
mismatches, missing mips, dropped header fields, endianness issues,
version regressions.

**Where the code lives.**

- [`OloEngine/tests/Rendering/PropertyTests/DataRoundTripTests.cpp`](../OloEngine/tests/Rendering/PropertyTests/DataRoundTripTests.cpp)

**How it works.** Procedurally generate a known bit pattern (gradient,
checkerboard, specific floating-point values), pass it through
`serialize → deserialize` (or `upload → readback`), assert equality
within the format's precision budget. Every test is deterministic; no
authored data.

**Limitations.** A value can round-trip perfectly and still bind to the
wrong texture slot at render time — that's for L4.

#### L4 — GPU state validation

**What it catches.** State leakage between passes — shadow pass
forgetting to restore depth writes, `OpenGLFramebuffer::Bind()`
unconditionally enabling blend, prior commands leaking blend state
into terrain callbacks. The layer that pins OloEngine's
historically-troubled state transitions.

**Where the code lives.**

- [`OloEngine/src/OloEngine/Renderer/Debug/GLStateGuard.h`](../OloEngine/src/OloEngine/Renderer/Debug/GLStateGuard.h)
- [`OloEngine/tests/Rendering/PropertyTests/GLStateGuardTest.cpp`](../OloEngine/tests/Rendering/PropertyTests/GLStateGuardTest.cpp)

**How it works.** `GLStateSnapshot::Capture()` queries the critical
set of GL state (depth / blend / stencil / cull / scissor /
polygon-mode / viewport / FBO / VAO / active program + 16 texture units
+ 16 UBO slots) via the non-DSA `glGet*` path. `GLStateGuard` is the
RAII wrapper a pass uses to snapshot on entry and diff on exit; its
`Policy` enum controls whether violations log, assert, or are ignored,
so guards can be rolled out incrementally.

**Limitations.** Only what you thought to track. Vendor-specific state
(e.g. NVIDIA-only flags) is invisible. Debug-only in production builds.

#### L5 — Render graph / hazard validation

**What it catches.** Structural bugs in the frame's pass graph — cycles,
duplicate connections, out-of-order dependencies, and (the big one)
resource hazards: a texture written by pass A and read by pass B
without the B-depends-on-A edge.

**Where the code lives.**

- Resource-aware RDG: [`RenderGraph.h`](../OloEngine/src/OloEngine/Renderer/RenderGraph.h) /
  [.cpp](../OloEngine/src/OloEngine/Renderer/RenderGraph.cpp) +
  [`ResourceHandle.h`](../OloEngine/src/OloEngine/Renderer/ResourceHandle.h)
- Pass declarations via `RenderPass::DeclareRead` / `DeclareWrite` in
  `OloEngine/src/OloEngine/Renderer/RenderPass.h` and every subclass.
- Tests: [`RenderGraphTest.cpp`](../OloEngine/tests/Rendering/RenderGraphTest.cpp),
  [`ResourceHazardValidationTests.cpp`](../OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp).

**How it works.** Each pass declares the `ResourceHandle`s it reads and
writes. On connection, the graph records the edge list;
`RenderGraph::ValidateResourceHazards()` walks the transitive
dependency closure and detects RAW / WAW / WAR hazards. `Renderer3D`
invokes the validator after `SetFinalPass` so any production mis-wiring
fails at construction time, not at submit time.

**Limitations.** Structural only. A graph that passes validation can
still produce wrong pixels — that's L1 and L8.

#### L6 — Performance regression

**What it catches.** Microbenchmark-level slowdowns in individual
post-process passes (tone map, bloom threshold / downsample / upsample
at 512²) and pass-to-pass transition cost via a whole-frame
post-process chain bench.

**Where the code lives.**

- [`OloEngine/tests/Rendering/PropertyTests/PerfRegressionTests.cpp`](../OloEngine/tests/Rendering/PropertyTests/PerfRegressionTests.cpp)
- Baselines: [`perf_baselines.txt`](../OloEngine/tests/Rendering/PropertyTests/perf_baselines.txt)
- Historical TSVs: [`perf_history/`](../OloEngine/tests/Rendering/PropertyTests/perf_history/)
- Trend detector: [`OloEngine/tests/scripts/perf_trend.py`](../OloEngine/tests/scripts/perf_trend.py)

**How it works.** Each bench renders a fullscreen pass, times it via
`GL_TIME_ELAPSED` queries, takes the **minimum of 20 samples after 5
warmup draws** (minimum is robust to thermal / scheduler noise). Policy
is `PASS < 1.5× baseline / WARN 1.5–2.5× / FAIL ≥ 2.5×` with a sanity
ceiling of 100 ms. The bloom passes (typical cost 5–15 µs, very
susceptible to scheduler jitter) additionally route through
`MeasureFullscreenPassStableNs`, which re-measures **once** when the
first sample trips WARN and keeps the faster of the two. This retry
only runs on bad samples; steady-state cost is unchanged.

Every run appends a row to `perf_history/<machine>.tsv`. Machine tag
resolves `OLOENGINE_PERF_MACHINE → COMPUTERNAME → HOSTNAME → "unknown"`.
Schema: `iso_utc \t name \t measured_ns \t baseline_ns \t ratio`.
`perf_trend.py` computes min / median / p95 per (machine, benchmark)
and runs a 3-sigma drift detector that compares the last 30 samples
against the prior 30 — exits 1 when the recent median is more than
three prior-window standard deviations slower.

**Limitations.** All benches remain fragment / fullscreen-pass
oriented — vertex-heavy or draw-call-explosion regressions in the 3D
pipeline (ECS traversal, culling, material resolution) are currently
only observed indirectly via CPU-time cost on the full-scene smoke in
L9. A fully Renderer3D-driven frame budget would drag the whole
scene-loading surface into L6 and is intentionally out of scope.

#### L7 — Smoke / sanity readback

**What it catches.** Catastrophic failures — entire pass rendered
black, NaN propagation, infinite values spraying through the
pipeline, alpha corruption, HDR values exceeding fp16 range.

**Where the code lives.**

- Helper: [`OloEngine/src/OloEngine/Renderer/Debug/RendererValidate.h`](../OloEngine/src/OloEngine/Renderer/Debug/RendererValidate.h)
- Tests: [`OloEngine/tests/Rendering/PropertyTests/RendererValidateTest.cpp`](../OloEngine/tests/Rendering/PropertyTests/RendererValidateTest.cpp)

**How it works.** `RendererValidate::ReadFloatAttachmentStats(fb, index)`
reads back a float framebuffer attachment and returns an
`AttachmentStats` record (min / max / avg / NaN count / Inf count /
per-channel RGBA maxima). Tests pin the contract on synthetic inputs;
the same helper is available in production debug builds after any
pass that can reasonably be spot-checked.

**Limitations.** "Not black and not NaN" is a low bar. L7 is the
safety net under L1, not a replacement for it.

#### L8 — Golden image

**What it catches.** Integration regressions — situations where every
individual pass is correct (L1 / L2 / L4 / L5 green) but the composed
frame still produces wrong pixels. State ordering across passes,
unexpected resource-slot collisions, depth / blend state interactions
that only manifest end-to-end.

**Where the code lives.**

- [`OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp`](../OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp)
- Baselines: `OloEditor/assets/tests/golden/*.png` (committed to the
  repo; regenerate with `OLOENGINE_GOLDEN_REBASE=1`).

**How it works — cascaded RMSE → SSIM.** On every run we read the
rendered frame, compute RGB RMSE over the reference, and decide:

| RMSE tier | Action |
|---|---|
| `RMSE < 0.004` | **Pass** fast-path — bit-for-bit close enough. |
| `RMSE > 0.02` | **Fail** hard — no perceptual tolerance hides a regression this large. |
| `0.004 ≤ RMSE ≤ 0.02` | Escalate to **mean SSIM** (Wang / Bovik 2004) over 8 × 8 non-overlapping windows, C1 = (0.01 · 255)², C2 = (0.03 · 255)². Pass iff `SSIM ≥ 0.985`. |

This two-stage decision exists because each metric alone is wrong:
RMSE over-rates isolated hot-pixel outliers (a 1-px specular highlight
shift that's perceptually invisible). SSIM over-tolerates a uniform
2-LSB brightness shift that RMSE flags every pixel on. The cascade
gets the "surely pass" and "surely fail" cases in microseconds and
only runs the expensive SSIM on the ambiguous middle band. ~90% of
runs resolve at stage 1 in practice.

**On failure we emit.** `<name>.actual.png`, `<name>.diff.png` (red =
worst-channel delta × 8, green = mean-channel delta × 8), plus a text
message containing RMSE, SSIM (if escalated), worst-pixel coordinates
+ delta, per-channel max (R/G/B), and the count of pixels with any
channel delta > 4 LSBs.

**Limitations.** Brittle across GPU vendors even with SSIM. Per-vendor
baseline sets remain a deferred follow-up.

#### L9 — Cross-vendor conformance

**What it catches.** Vendor-specific divergence — a bug that hides on
NVIDIA but manifests on Mesa / Intel / AMD (or vice versa). Precision
differences, fast-math reordering, driver extension quirks.

**Where the code lives.**

- [`.github/workflows/cross-vendor.yml`](../.github/workflows/cross-vendor.yml)

**How it works.** Nightly at 03:47 UTC + manual dispatch: the workflow
downloads `pal1000/mesa-dist-win 24.3.4`, drops its `opengl32.dll`
next to the test binary, sets `GALLIUM_DRIVER=llvmpipe`,
`LIBGL_ALWAYS_SOFTWARE=1`, `MESA_GL_VERSION_OVERRIDE=4.6`, tags the
run with `OLOENGINE_PERF_MACHINE=ci-llvmpipe-windows` +
`OLOENGINE_GOLDEN_VENDOR=llvmpipe`, runs the full suite, then invokes
`perf_trend.py`. On failure it uploads the test output, the appended
perf-history row, and any golden-image diffs as CI artefacts.

**Limitations.** llvmpipe is a software rasteriser — it catches *spec*
divergence cleanly but says little about how a specific vendor driver
actually behaves on real silicon. Hardware-GPU coverage is a deferred
follow-up.

#### L10 — Automatic diagnostic escalation

**What it catches.** Nothing on its own. L10 is the capture helper
that fires when *any other layer fails* so you have enough post-mortem
evidence to debug without re-running the test.

**Where the code lives.**

- [`TestFailureCapture.h`](../OloEngine/tests/Rendering/PropertyTests/TestFailureCapture.h) /
  [.cpp](../OloEngine/tests/Rendering/PropertyTests/TestFailureCapture.cpp)
- Self-tests: [`TestFailureCaptureTest.cpp`](../OloEngine/tests/Rendering/PropertyTests/TestFailureCaptureTest.cpp)
- Registered from: [`OloEngine/tests/OloEngineTest.cpp`](../OloEngine/tests/OloEngineTest.cpp)

**How it works.** A GoogleTest event listener is registered in the
test binary's `main()`. On the **first** failed assertion of a test
it invokes `CaptureAll()`, which writes — into
`OloEditor/assets/tests/captures/<suite>__<name>/` — four artefacts:

| Artefact | Contents |
|---|---|
| `metadata.txt` | Test id, ISO-UTC timestamp, GL vendor / renderer / version / GLSL, assertion summary. |
| `gl_state.txt` | Field-by-field dump of `GLStateSnapshot::Capture()` — the same snapshot `GLStateGuard` uses. |
| `framebuffer.png` | Pixel-perfect RGBA readback of the currently bound draw FBO at its current viewport, Reinhard + gamma-encoded so HDR attachments are always visually useful. |
| `command_bucket.txt` | One-page summary of the most recent captured frame from `FrameCaptureManager` — pre/post-sort / post-batch command counts, sort / batch / execute timings. Only written when the manager has a recorded frame. |

The listener reuses OloEngine's production debug infrastructure —
nothing in the capture path is test-only except the listener glue.
This keeps the dumped state faithful to what the engine's debug
overlay shows at runtime and means adding a new tracked field to
`GLStateSnapshot` automatically propagates to failure dumps.

**Limitations.** If a test crashes the process (SEH, segfault) the
listener never fires. For that class of failure you still have the
GoogleTest crash stack plus artefacts from the *previous* test's run.

#### L11 — Sanitizers & fuzzing

**What it catches.** Memory-safety bugs — buffer overruns,
use-after-free, data races, undefined behaviour in serialisation /
deserialisation paths. These never show up as a "wrong pixel"; they
show up as a corrupt vertex buffer, a torn mesh header, or a
mysterious crash two hours into a long-running test.

**Where the code lives.**

- Sanitizer CMake plumbing: [`cmake/Sanitizers.cmake`](../cmake/Sanitizers.cmake)
  (`OLO_ENABLE_ASAN`, `OLO_ENABLE_TSAN`, `OLO_ENABLE_UBSAN`).
- libFuzzer harnesses: [`OloEngine/tests/Fuzzing/`](../OloEngine/tests/Fuzzing/)
- Fuzz CMake gate: [`OloEngine/tests/Fuzzing/CMakeLists.txt`](../OloEngine/tests/Fuzzing/CMakeLists.txt) —
  toggled by `-DOLO_ENABLE_FUZZING=ON`; requires Clang / ClangCL.
- CI: [`.github/workflows/fuzz.yml`](../.github/workflows/fuzz.yml)

**How it works.** Each harness implements `LLVMFuzzerTestOneInput`,
writes the arbitrary fuzz-provided bytes to a PID-tagged temp file,
and drives the real production deserialiser against it.
`-fsanitize=fuzzer,address,undefined` is set when `OLO_ENABLE_FUZZING`
and the compiler is Clang / ClangCL. Each harness target has a
per-target `<name>-smoke` sub-target that runs 30 s against the
checked-in corpus; the aggregate target is `OloEngine-FuzzSmoke`.

**Current harnesses.**

- `FuzzAnimationBinary` → `AnimationBinarySerializer::Read`.
- `FuzzMeshBinary` → `MeshBinarySerializer::Read`.
- `FuzzInputActionYaml` → `InputActionSerializer::DeserializeContexts`.
- `FuzzImageDecoder` → `stb_image`'s decode paths (PNG / JPG / BMP /
  TGA / PSD / GIF / HDR).
- `FuzzSceneYaml` → `SceneSerializer::DeserializeFromYAML`.
- `FuzzAssimpMesh` → `Assimp::Importer::ReadFileFromMemory` with
  rotating format hints. Assimp has a long CVE history across MDL /
  MD2 / LWO / IFC / X / BVH parsers; this exercises the full import
  pipeline on each input.
- `FuzzSpirvCross` → `spirv_cross::CompilerGLSL` construction +
  `compile()`. Drives the SPIR-V binary parser + reflection +
  GL-decompile path.

Each has a minimal seed corpus in `OloEngine/tests/Fuzzing/corpus/<target>/`.

**Limitations.** 30 s per target nightly is a smoke budget — it
catches regressions, not deep bugs. An OSS-Fuzz integration for
sustained campaigns is a deferred follow-up.

---

## 7. Functional / cross-subsystem axis

The Functional axis catches bugs that no per-subsystem unit test will
ever catch: **two subsystems disagreeing about the same frame**.

### 7.1 What it catches

The bug class is the one that no per-subsystem test sees. Real examples
the harness has pinned:

- A Lua `OnUpdate` callback writes a translation that Physics3D then
  silently overwrites because the body sync happens *after* scripting
  on the same tick.
- A Box2D `b2World_Step` runs on an invalidated world ID because
  `OnPhysics2DStop` ran but `m_PhysicsWorld` wasn't cleared.
- A character controller never moves horizontally because Jolt only
  integrates lateral velocity when grounded *or*
  `ControlMovementInAir=true`, and the test setup forgot the second flag.
- An Audio listener points at a freed `Ref<>` after the scene switches
  because `InitAudioRuntime` wasn't part of the headless tick path.
- A `Scene::DestroyEntity` leaves a Jolt body alive because the per-type
  `OnComponentRemoved` specialisation wasn't wired.
- A YAML scene-deserialise path passes `state.QuestID` and
  `std::move(state)` as two arguments in the same call; MSVC evaluates
  the `move` first, so every restored active quest lands under
  `m_ActiveQuests[""]`. The per-subsystem `QuestJournal` unit test
  cannot see this because the bug lives in the *serialiser*.

None of these are visible at L1–L11. The per-subsystem layers exercise
each piece in isolation; goldens only catch *visible* symptoms after
the fact. Functional is the only axis where the failure class "two
subsystems disagree about the same frame" gets pinned.

### 7.2 The fixture and its contracts

Every Functional test inherits `OloEngine::Functional::FunctionalTest`
(defined in [`OloEngine/tests/Functional/FunctionalTest.{h,cpp}`](../OloEngine/tests/Functional/))
and overrides `BuildScene()` to construct entities, attach components,
and call opt-in subsystem helpers. The test body then advances
simulated time via the latent helpers.

**Headless by default.** `Scene::OnUpdateRuntime` runs **without a GL
context**: no `RenderScene` call, no command bucket population, no
framebuffer attach. Enforced by `m_Scene->SetRenderingEnabled(false)`
in `SetUp`, mirroring what `Scene::OnRuntimeStart` does for
`Application::IsHeadless()` — but the harness never calls
`OnRuntimeStart` because that path requires `Application::Get()`.

Consequences:

- Functional tests parallelise across cores. There is no shared GL
  state.
- They run on WSL2 / Linux containers in CI (`OloServer` is the only
  target that does anyway).
- Visual regressions are not in scope — that is what L8 goldens are for.

**Best-effort determinism.** The fixture injects a fixed `Timestep`
(default 1/60 s) and seeds `FastRandomPCG` per test from a stable hash
of `(suite, name)`. That gives deterministic Animation, Physics3D
solver step, Box2D step, particle emission, and any system that takes
the RNG by reference.

It does not guarantee determinism for subsystems that read wall-clock
time directly (some Networking timers, audio engine clock, debug
overlays). When a Functional test exposes such leakage, the fix is to
inject the clock into that subsystem in a focused PR — not to weaken
the test contract.

**Subsystem opt-in helpers.** Subsystems start in their off state.
Call `Enable*` from `BuildScene()` for the ones the test depends on:

- `EnablePhysics3D()` — starts JoltScene + the engine task scheduler;
  creates Jolt bodies for already-constructed Rigidbody3D + collider
  entities.
- `EnablePhysics2D()` — starts Box2D world; creates 2D bodies.
- `EnableAnimation()` — marker; animation tick is already in
  `Scene::OnUpdateRuntime`.
- `EnableLua()` — process-once `LuaScriptEngine::Init`, scene-context
  wiring for `LuaScriptGlue`, and `Scene::SetRunning(true)` so
  `OnDestroy` can fire.
- `RegisterLuaScript(entity, path)` — attaches a `.lua` to an entity
  and calls `OnCreate`, which production code does from `OnRuntimeStart`.
- `EnableAudio()` — calls `Scene::InitAudioRuntime`.
- `EnableDialogue()` — instantiates `Scene::m_DialogueSystem`.
- `EnableAssetManager(stagedAssets)` — builds an isolated temp project
  under the OS temp dir, copies the listed assets out of
  `OloEditor/SandboxProject/`, writes a minimal `.oloproj`, and brings
  up an `EditorAssetManager` bound to that project. The staged copy
  isolates each test's `AssetRegistry.oar` mutations from the working
  tree.

Helpers are idempotent within a test but **must be called from
`BuildScene()`** (after entity construction, before the test body),
because several iterate existing entities to allocate runtime state
(`Ref<AudioListener>`, Jolt body IDs, etc.).

**Latent / multi-frame helpers.** The fixture exposes three ways to
advance time:

- `RunFrames(N, dt)` — tick exactly `N` frames at fixed `dt`.
- `TickFor(seconds, dt)` — tick until `seconds` of simulated time have
  elapsed (rounded up to the next whole frame).
- `TickUntil(predicate, timeoutSeconds, dt)` — tick until `predicate()`
  returns true. Returns true on success, false on timeout. Checks the
  predicate before the first tick so a condition that's already
  satisfied on entry returns immediately.

Never spin on wall-clock or `std::this_thread::sleep_for`. The contract
is simulated time only.

**Editor-mounted asset access.** `EnableAssetManager` mounts a real
`EditorAssetManager` against an isolated temp-dir copy of the
SandboxProject. This lets tests reference real editor asset types
(textures, meshes, sounds, prefabs, scenes) without dragging in
synthetic placeholders, and without dirtying
`OloEditor/SandboxProject/` itself. Editor content drift can still
break Functional tests at a distance — treat that as *asset-pipeline
regression caught early*, not as test-suite flakiness.

### 7.3 Authoring a Functional test

1. Pick the **seam(s)** the test pins. If only one subsystem is
   exercised, the test belongs in a per-subsystem unit suite or one
   of the renderer-pyramid layers — not in Functional.
2. Place the new `.cpp` under `OloEngine/tests/Functional/<Subsystem>/`.
   The subdirectory becomes the grouping in the generated Functional catalogue
   (`docs/test-catalogue.functional.md`).
3. Inherit `FunctionalTest`. Override `BuildScene()`. Construct
   entities and components, then call `Enable*` for the subsystems
   you depend on.
4. Use `RunFrames` / `TickFor` / `TickUntil` to advance the world.
5. Assertions go in the test body (`TEST_F`), after `TickUntil`
   returns. Standard `EXPECT_*` macros work because the test stays
   synchronous.
6. Classify the new `.cpp` as Functional — **preferred:** add a
   `// OLO_TEST_LAYER: Functional` comment near the top of the file (no
   shared file to conflict on); or add it to `file_layer_map` in
   [`test_catalogue.json`](../OloEngine/tests/scripts/test_catalogue.json).
   The pre-commit hook fails the commit if it's classified by neither.
7. Add the file to `OloEngine/tests/CMakeLists.txt` so it builds.
8. *(Optional)* Run `python OloEngine/tests/scripts/generate_test_catalogue.py`
   to refresh the generated, git-ignored catalogue docs
   (`docs/test-catalogue.*.md`) — nothing to commit.

The conventional file header comment names the seam the test pins:

```cpp
// =============================================================================
// MyFeatureFiresViaSceneTickTest — Functional Test.
//
// Cross-subsystem seam under test:
//   <Subsystem A> × <Subsystem B> × <…> via Scene::OnUpdateRuntime.
//   <One paragraph on what regression class this pins.>
//
// Scenario: <minimal scene construction + expected outcome after N ticks>.
// =============================================================================
```

### 7.4 Known limitations

- **`RenderingFunctionalTest`** — an opt-in fixture base for the rare
  cross-subsystem bug that only manifests with rendering active is
  not yet implemented. The seam exists in `Scene` (the headless flag
  is the only divergence) but the wrapper has not been written.
- **CI categorisation** — Functional tests run alongside
  `OloEngine-Tests`. Splitting them into Smoke / Engine / Product
  categories (separate CI shards) is a deferred follow-up; re-evaluate
  once the catalogue passes ~200 tests.
- **Determinism leaks** — the best-effort contract is adequate for
  most tests; subsystems that take wall-clock time should have their
  clock injection fixed when they show up in a Functional test as
  flake.

---

## 8. CI layout

| Workflow | Trigger | Coverage |
|---|---|---|
| `OloEngine-Tests` | every PR + push | L1–L8, L10, L11 smoke, plus Functional axis and all subsystem unit tests |
| `.github/workflows/cross-vendor.yml` | nightly 03:47 UTC + manual | L9 against Mesa llvmpipe; also runs the perf trend detector |
| `.github/workflows/fuzz.yml` | nightly 04:13 UTC + manual | L11 fuzzers for 30 s each, uploads crash / leak artefacts on failure |

Local quick paths:

```powershell
# Full suite (every layer except long-running L6 stress)
.\build\OloEngine\tests\Debug\OloEngine-Tests.exe

# Single test
.\build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=SuiteName.TestName

# Fast inner loop — everything EXCEPT perf/golden/visual (L6/L7/L8). The exclude
# filter is derived from the catalogue, so it stays in sync as tests are added.
# (Or just run the "run-fast-tests-debug" VS Code task, which does this for you.)
$f = (python OloEngine/tests/scripts/generate_test_catalogue.py --gtest-filter --exclude L6,L7,L8)
.\build\OloEngine\tests\Debug\OloEngine-Tests.exe "--gtest_filter=$f"

# Golden rebase (only after a deliberate visual change)
$env:OLOENGINE_GOLDEN_REBASE = "1"
.\build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=GoldenImage*
Remove-Item Env:OLOENGINE_GOLDEN_REBASE

# Perf rebase (only after a hardware swap or deliberate optimisation)
$env:OLOENGINE_PERF_REBASE = "1"
.\build\OloEngine\tests\Debug\OloEngine-Tests.exe --gtest_filter=PerfRegression*
Remove-Item Env:OLOENGINE_PERF_REBASE
```

Perf baselines are developer-machine local —
[`perf_history/`](../OloEngine/tests/Rendering/PropertyTests/perf_history/)
is gitignored so no local seeding leaks into other developers' runs.

---

## 9. Test catalogues (generated)

Every test `.cpp` under `OloEngine/tests/` is classified onto one of three axes —
the renderer testing pyramid, the Functional / cross-subsystem axis, or plain
subsystem unit tests — by **one** of:

- an in-file `// OLO_TEST_LAYER: <id>` comment near the top of the `.cpp`
  (**preferred** — the classification lives in the test file itself, so adding a
  test never touches a shared file and two branches adding tests can't collide on
  it); **or**
- an entry in
  [`test_catalogue.json`](../OloEngine/tests/scripts/test_catalogue.json) →
  `file_layer_map` (the fallback for files that don't carry a marker).

A file uses one or the other, **not both** — the marker wins, and the generator
errors if a file carries both. Valid `<id>` values are the renderer layers
`L1`–`L11` / `plumbing` / `cullinglod` / `shaderpipe` / `integration` / `meta`,
plus `Functional` and `unit`. Files with no test macros (the gtest `main`,
libFuzzer targets, fixtures/helpers) are not tests and need no entry. Pre-commit
runs [`generate_test_catalogue.py`](../OloEngine/tests/scripts/generate_test_catalogue.py)
in `--check` mode and fails the hook if any test `.cpp` is classified by neither
mechanism — so the classification gate always holds.

The per-file catalogue **tables** are *generated, not committed.* Run

```powershell
python OloEngine/tests/scripts/generate_test_catalogue.py
```

to (re)build them into three documents alongside this guide — one per axis:

| Axis | Generated document |
|---|---|
| Renderer testing pyramid (L1–L11 + plumbing/cullinglod/shaderpipe/integration/meta) | `docs/test-catalogue.renderer.md` |
| Functional / cross-subsystem (grouped by subdirectory) | `docs/test-catalogue.functional.md` |
| Unit / subsystem (grouped by directory) | `docs/test-catalogue.unit.md` |

Those files are **git-ignored** on purpose. The tables change on essentially
every test, and when they lived inline in this doc and were committed, every
parallel PR false-conflicted on the catalogue the moment any other test landed:
GitHub computes its merge/conflict status *without* the `merge=union` driver, so
the regenerated tables always looked like a conflict even though a real merge
resolves cleanly. A generated artifact that changes on nearly every PR should not
be a tracked file — so only this human-written guide is versioned; the rendered
inventory is built on demand (and can be published as a CI artifact if a browsable
copy is wanted).

## 10. References

Primary sources the strategy draws from:

- [**How (not) to test graphics algorithms** — Bart Wronski (2019)](https://bartwronski.com/2019/08/14/how-not-to-test-graphics-algorithms/) — the foundational essay for L1, L8, and the general "property > golden" pyramid orientation.
- [**The Furnace Test** — Brian Karis (SIGGRAPH 2013)](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf) — origin of PBR energy-conservation tests used in `PbrBrdfTest.FurnaceEnergyBoundViaMonteCarlo`.
- **Wang, Bovik, Sheikh, Simoncelli (2004)**, *Image Quality Assessment: From Error Visibility to Structural Similarity* — defines the SSIM formulation used in the L8 cascade.
- [**Filament Test Infrastructure**](https://github.com/google/filament/tree/main/test) — reference for the "shared GLSL / C++ math" pattern in L2 and the headless-GL approach.
- [**Mesa piglit**](https://gitlab.freedesktop.org/mesa/piglit) — the cascaded RMSE / perceptual-metric pattern in L8 is a simplified version of piglit's tolerance model.
- [**Vulkan CTS (dEQP)**](https://github.com/KhronosGroup/VK-GL-CTS) — per-function shader precision tests in L2 take their probe-shader approach from dEQP.
- [**libFuzzer**](https://llvm.org/docs/LibFuzzer.html) — the `LLVMFuzzerTestOneInput` interface underpinning L11.
- [**Google Sanitizers**](https://github.com/google/sanitizers/wiki) — ASan / TSan / UBSan plumbing in `cmake/Sanitizers.cmake`.
- [**Mesa 3D for Windows**](https://github.com/pal1000/mesa-dist-win) — the llvmpipe / OpenGL driver drop used by the cross-vendor CI.
- [**Unreal Engine Functional Tests**](https://docs.unrealengine.com/5.0/en-US/automation-system-in-unreal-engine/) — model for the cross-subsystem Functional axis (Scene-tick fixture, latent assertions).

ADRs (rationale for the two-axis split):

- [`docs/adr/0001-functional-tests-as-separate-axis.md`](adr/0001-functional-tests-as-separate-axis.md)
- [`docs/adr/0002-headless-tick-default-for-functional-tests.md`](adr/0002-headless-tick-default-for-functional-tests.md)
- [`docs/adr/0003-functional-tests-mount-editor-asset-root.md`](adr/0003-functional-tests-mount-editor-asset-root.md)

Glossary of testing-specific terms: [`CONTEXT.md`](../CONTEXT.md).
