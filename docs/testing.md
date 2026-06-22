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
9. [Test catalogues (auto-generated)](#9-test-catalogues-auto-generated)
    - 9.1 [Renderer tests](#91-renderer-tests)
    - 9.2 [Functional tests](#92-functional-tests)
    - 9.3 [Unit / subsystem tests](#93-unit--subsystem-tests)
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
- `FuzzInputActionYaml` → `InputActionSerializer::Deserialize`.
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
   The subdirectory becomes the catalogue grouping in §9.2.
3. Inherit `FunctionalTest`. Override `BuildScene()`. Construct
   entities and components, then call `Enable*` for the subsystems
   you depend on.
4. Use `RunFrames` / `TickFor` / `TickUntil` to advance the world.
5. Assertions go in the test body (`TEST_F`), after `TickUntil`
   returns. Standard `EXPECT_*` macros work because the test stays
   synchronous.
6. Register the new `.cpp` in
   [`OloEngine/tests/scripts/test_catalogue.json`](../OloEngine/tests/scripts/test_catalogue.json)
   → `file_layer_map` with the tag `"Functional"`. The pre-commit
   hook will fail the commit otherwise.
7. Add the file to `OloEngine/tests/CMakeLists.txt` so it builds.
8. Run `python OloEngine/tests/scripts/generate_test_catalogue.py` to
   refresh the auto-catalogue blocks in this document.

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

## 9. Test catalogues (auto-generated)

All three catalogues below are regenerated by
[`OloEngine/tests/scripts/generate_test_catalogue.py`](../OloEngine/tests/scripts/generate_test_catalogue.py)
from the mapping in
[`OloEngine/tests/scripts/test_catalogue.json`](../OloEngine/tests/scripts/test_catalogue.json).
The generator scans the **entire** `OloEngine/tests/` tree — there is no
allowlist and no exclude list. Pre-commit runs the script in `--check`
mode and fails the hook if **any** `.cpp` that declares a `TEST` /
`TEST_F` / `TEST_P` / `TYPED_TEST` is missing from the JSON, wherever it
lives in the tree. Files with no test macros (the gtest `main`, libFuzzer
targets, fixtures/helpers) are not tests and need no entry.

Every test is classified onto one of three axes: the renderer testing
pyramid (§9.1), the Functional / cross-subsystem axis (§9.2), or plain
subsystem unit tests (§9.3, grouped by directory).

**Do not hand-edit** the blocks between `<!-- BEGIN: ... -->` and
`<!-- END: ... -->` markers. The generator overwrites them.

### 9.1 Renderer tests

<!-- BEGIN: renderer-catalogue (generated by OloEngine/tests/scripts/generate_test_catalogue.py) -->

> **Do not edit by hand.** Generated from [test_catalogue.json](../OloEngine/tests/scripts/test_catalogue.json) by [generate_test_catalogue.py](../OloEngine/tests/scripts/generate_test_catalogue.py). Add new test files to the config and run the script (or pre-commit will run it with `--check`).

#### L1 — Property / behavioural tests

| File | Tests | Cases |
|---|---:|---|
| [DeferredOverlayPassTests.cpp](../OloEngine/tests/Rendering/PropertyTests/DeferredOverlayPassTests.cpp) | 6 | **DeferredModelUBOLayout** &mdash; `StructMatchesStd140Expectations`, `PrevModelStoresMat4RoundTrip`<br/>**DeferredDrawMeshCommand** &mdash; `PrevTransformFieldIsPresent`, `PrevTransformRoundTripsThroughMemcpy`<br/>**ForwardOverlayRenderPassConstruction** &mdash; `DefaultConstructsAndExposesSetter`<br/>**GBufferLayout** &mdash; `EntityIDIsTheFifthAttachment` |
| [DeferredPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/DeferredPropertyTests.cpp) | 4 | **DeferredOctNormalTest** &mdash; `RoundTripKeepsHemisphereUnderOneDegree`, `EncodeOutputStaysInUnitRange`<br/>**DeferredSettingsTest** &mdash; `DefaultsMatchPlan`, `MSAASampleCountRoundTripsCommonValues` |
| [DrawIndexedRawOffsetTest.cpp](../OloEngine/tests/Rendering/PropertyTests/DrawIndexedRawOffsetTest.cpp) | 1 | **DrawIndexedRawOffset** &mdash; `BaseIndexDrawsTheCorrectSubmesh` |
| [HZBGeneratorResizeTest.cpp](../OloEngine/tests/Rendering/PropertyTests/HZBGeneratorResizeTest.cpp) | 6 | **HZBGeneratorResizeTest** &mdash; `FirstResizeSetsUVFactorAndHZBDimensions`, `SameBucketResizeRefreshesUVFactor`, `SameBucketHeightOnlyResizeRefreshesUVFactorY`, `SameBucketWidthOnlyResizeRefreshesUVFactorX`, `ChainOfSameBucketResizesAllRefreshUVFactor`, `CrossBucketResizeUpdatesBothTextureAndUVFactor` |
| [OITPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/OITPropertyTests.cpp) | 7 | **OITWeightTest** &mdash; `ClampBoundsAreRespected`, `MonotonicInAlpha`, `NearFragmentsOutweighFar`<br/>**OITResolveTest** &mdash; `SingleFragmentOpaqueMatchesForeground`, `SingleTransparentFragmentIsApproxOver`, `OrderIndependentForTwoFragments`, `EmptyAccumulationPreservesBackground` |
| [OceanFFTSpectrumTest.cpp](../OloEngine/tests/Rendering/PropertyTests/OceanFFTSpectrumTest.cpp) | 33 | **OceanFFT** &mdash; `IsPowerOfTwo`, `SignedFrequencyOrdering`, `RoundTrip1D`, `ImpulseTransformsToConstant`, `ConstantTransformsToDCImpulse`, `ParsevalEnergyConservation`, `RoundTrip2D`<br/>**OceanSpectrum** &mdash; `PhillipsZeroAtOrigin`, `PhillipsZeroPerpendicularToWind`, `PhillipsFavoursWindAlignment`, `PhillipsHighFrequencyFalloff`, `SmallWaveSuppressionDampsHighFrequencies`, `DispersionMonotonicAndDeepWater`, `GenerateH0IsDeterministic`, `GenerateH0SeedChangesField`, `GenerateH0ZeroAtDC`, `AnimatedSpectrumIsHermitianSoFieldIsReal`, `EvaluateFieldIsDeterministic`, `EvaluateFieldAnimatesOverTime`, `ZeroAmplitudeProducesFlatSea`, `HigherAmplitudeRaisesRoughness`, `NormalsAreUnitLengthAndMostlyUpward`, `NoChoppinessGivesUnitJacobianAndNoHorizontalShift`, `ChoppinessIntroducesFolding`, `SampleHeightBilinearMatchesGridPoints`, `SampleHeightBilinearIsPeriodic`, `BandLimitedH0CopiesMatchingFrequencyBins`, `BandLimitedH0SameOrLargerResolutionIsIdentity`, `BandLimitedFieldTracksFullResolutionSurface`<br/>**OceanFFTField** &mdash; `UpdateProducesValidFieldWithoutGpu`, `SampleHeightIsFiniteAndDeterministic`, `NormalisesToMetreScaleWaveHeight`, `FlatSeaSamplesNearZero` |
| [PbrPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PbrPropertyTests.cpp) | 21 | **PbrFresnelTest** &mdash; `NormalIncidenceEqualsF0`, `GrazingIncidenceApproachesOne`, `MonotonicallyDecreasingInCosTheta`, `MatchesCpuReference`<br/>**PbrNdfTest** &mdash; `NonNegativeAndFinite`, `PeaksAtHAlignedWithN`, `Roughness1HEqualsN_Equals_InvPi`, `LowRoughnessConcentratesHighlight`<br/>**PbrDiffuseTest** &mdash; `MetallicOneKillsDiffuse`, `DielectricDiffuseNonZero`<br/>**PbrBrdfTest** &mdash; `PositiveAndFiniteEverywhere`, `HelmholtzReciprocity`, `FurnaceIntegralWithinEnergyBounds`<br/>**PbrNormalMapTest** &mdash; `FlatNormalReturnsGeometricNormal`<br/>**PbrIrradianceTest** &mdash; `UniformWhiteYieldsNormalisedUnity`<br/>**PbrPrefilterTest** &mdash; `UniformWhiteYieldsUnityAtAllRoughness`, `MipChainEnergyIsMonotonicAndMip0ApproxSource`<br/>**PbrPrefilterAdvancedTest** &mdash; `ImportanceUniformWhiteYieldsUnity`, `ImportanceMipChainEnergyIsMonotonic`<br/>**PbrIrradianceAdvancedTest** &mdash; `ImportanceUniformWhiteYieldsUnity`<br/>**PbrBRDFLutAdvancedTest** &mdash; `ProducesValidNonDegenerateLUT` |
| [PostProcessPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PostProcessPropertyTests.cpp) | 17 | **ToneMapMonotonicityTest** &mdash; `ReinhardPreservesLuminanceOrdering`<br/>**ToneMapMonotonicityFixture** &mdash; `HdrRampIsNonDecreasing`<br/>**ToneMapBlackFixture** &mdash; `BlackInputStaysBlack`<br/>**ToneMapExtremeHdrFixture** &mdash; `ExtremeHdrProducesFiniteOutput`<br/>**VignettePropertyTest** &mdash; `CenterBrighterThanCorners`<br/>**ChromaticAberrationPropertyTest** &mdash; `CenterPixelUnaffected`<br/>**FxaaUniformInputTest** &mdash; `UniformInputIsNoOp`<br/>**FxaaEdgeDisplacementTest** &mdash; `EdgePreservesFlatRegions`<br/>**MotionBlurStaticTest** &mdash; `ZeroVelocityIsIdentity`<br/>**DofFocusTest** &mdash; `DepthAtFocusDistanceIsIdentity`, `CocLinearModelMatchesSweep`<br/>**BloomThresholdTest** &mdash; `BlackInputStaysBlack`<br/>**BloomDownsampleTest** &mdash; `UniformInputIsIdentity`<br/>**BloomUpsampleTest** &mdash; `UniformInputIsIdentity`<br/>**BloomCompositeTest** &mdash; `ZeroIntensityPassesSceneThrough`<br/>**BloomChainEnergyTest** &mdash; `MultiPassDownUpPreservesTotalEnergy`<br/>**FogDisabledTest** &mdash; `DisabledFlagProducesZeroInscatter`<br/>*Parametrised via 3 `INSTANTIATE_TEST_SUITE_P`* |
| [ShaderMathIdentityTests.cpp](../OloEngine/tests/Rendering/PropertyTests/ShaderMathIdentityTests.cpp) | 8 | **ShaderMathIdentityTest** &mdash; `BitwiseRadicalInverseMatchesLoopVanDerCorput`, `RadicalInverseStaysInUnitInterval`, `PowHelpersMatchStdPowInUnitInterval`, `Pow4MatchesStdPowBeyondUnitInterval`, `SquaringChainsMatchStdPowForHighExponents`, `OrthonormalBasisIsOrthonormalAndRightHanded`, `OrthonormalBasisFiniteAtPoles`, `ImportanceSampleGGXProducesUnitHemisphereVectors` |
| [ShadowTerrainPropertyTests.cpp](../OloEngine/tests/Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp) | 5 | **ShadowBoundsTest** &mdash; `BoundaryCasesShortCircuit`, `CascadeIndexSweepIsCorrect`<br/>**ShadowBiasTest** &mdash; `SelfShadowAndPeterPanningContract`<br/>**TerrainHeightmapTest** &mdash; `FlatHeightmapProducesUpNormal`<br/>**TerrainSplatmapTest** &mdash; `ChannelIsolationMapsToCorrectLayer` |
| [WaterSurfaceSamplerTest.cpp](../OloEngine/tests/Rendering/PropertyTests/WaterSurfaceSamplerTest.cpp) | 7 | **WaterSurfaceSampler** &mdash; `FlatWaterReturnsPlaneHeight`, `ZeroSteepnessHasNoVerticalDisplacement`, `PlaneHeightShiftsResultByExactlyDelta`, `VerticalDisplacementRespectsTheRendererBound`, `WavesAnimateOverTimeButFreezeAtZeroSpeed`, `IsDeterministic`, `HeightInversionLandsOverTheQueryColumn` |
| [ReflectionProbeSelectionTest.cpp](../OloEngine/tests/Rendering/ReflectionProbeSelectionTest.cpp) | 6 | **ReflectionProbeSelection** &mdash; `EmptyListReturnsNoMatch`, `CameraOutsideAllProbesReturnsNoMatch`, `CameraExactlyOnBoundaryCounts`, `SingleContainingProbeWins`, `PicksClosestWhenMultipleContain`, `DegenerateZeroRadiusProbeIgnored` |

#### L2 — Shader unit tests

| File | Tests | Cases |
|---|---:|---|
| [ShaderCompilationTest.cpp](../OloEngine/tests/Rendering/PropertyTests/ShaderCompilationTest.cpp) | 1 | **ShaderCompilation** &mdash; `AllProductionShadersCompileUnderVulkanTarget` |
| [ShaderUnitTests.cpp](../OloEngine/tests/Rendering/PropertyTests/ShaderUnitTests.cpp) | 10 | **ShaderUnitSrgbTest** &mdash; `RoundTripWithinOneLsb`, `MidpointMatchesReference`<br/>**ShaderUnitIBLTest** &mdash; `BRDFLutGenerationProducesNonZeroSplitSum`<br/>**ShaderUnitSkyboxTest** &mdash; `SamplingKeepsSkyAboveGround`<br/>**ShaderUnitToneMapTest** &mdash; `ReinhardMatchesReference`, `AcesMatchesReference`, `Uncharted2MatchesReference`<br/>**ShaderUnitGgxTest** &mdash; `HemisphereIntegralIsOne`<br/>**ShaderUnitOctNormalTest** &mdash; `RoundTripPreservesUnitNormals`<br/>**ShaderUnitFogTest** &mdash; `EndpointInvariants` |

#### L3 — Data round-trip

| File | Tests | Cases |
|---|---:|---|
| [DataRoundTripTests.cpp](../OloEngine/tests/Rendering/PropertyTests/DataRoundTripTests.cpp) | 7 | **DataRoundTripTest** &mdash; `Rgba32FGpuBitIdentity`, `Rgba8GpuByteIdentity`, `BackpackLegacyObjImportsPbrCompanionMaps`, `BackpackLegacyObjFlipsUvsToMatchTextureUploads`, `IblCacheCubemapRoundTripPreservesAllMips`, `RandomisedRgba32FStressRoundTrip`, `RandomisedRgba8StressRoundTrip` |
| [ModelLoadDeterminismTest.cpp](../OloEngine/tests/Rendering/PropertyTests/ModelLoadDeterminismTest.cpp) | 1 | **ModelLoadDeterminism** &mdash; `Backpack_FreshVsCache` |
| [TextureSaveRoundTripTest.cpp](../OloEngine/tests/Rendering/PropertyTests/TextureSaveRoundTripTest.cpp) | 9 | **TextureSaveRoundTripTest** &mdash; `Rgba8Texture2DToPngIsByteIdentical`, `Rgb8Texture2DToPngHandlesOddWidthAlignment`, `FloatTextureSavesAsPngWithoutGLInvalidOperation`, `FloatTextureWithNaNAndInfSavesAsZeroOrSaturated`, `DepthAsUnsignedIntPromotesToFloatReadback`, `R32FTextureSavesAsPngClampedToGrayscale`, `Rgba32FTexture2DToHdrRoundTripsWithinRGBETolerance`, `CubemapFaceSelectsCorrectLayer`, `InvalidInputsAreRejected` |
| [ShaderBinaryCacheRoundTripTest.cpp](../OloEngine/tests/Rendering/ShaderBinaryCacheRoundTripTest.cpp) | 8 | **ShaderBinaryCacheRoundTrip** &mdash; `RoundTripIsByteExact`, `RecoveredDataLengthExcludesHeader`, `TailBytesSurvive`, `RejectsFileSmallerThanHeader`, `RejectsEmptyStream`, `HeaderOnlyYieldsEmptyPayload`, `LargeBinaryRoundTrip`, `RoundTripThroughRealFile` |

#### L4 — GPU state validation

| File | Tests | Cases |
|---|---:|---|
| [GLStateGuardTest.cpp](../OloEngine/tests/Rendering/PropertyTests/GLStateGuardTest.cpp) | 10 | **GLStateGuardTest** &mdash; `EmptyRegionHasNoLeaks`, `LeakedBlendIsDetected`, `LeakedDepthMaskIsDetected`, `LeakedDrawFboIsDetected`, `LeakedTextureBindingIsDetected`, `LeakedUboBindingIsDetected`, `MultipleLeaksAreAllReported`, `RestoredStateShowsNoLeaks`, `RestorePolicy_RestoresCoreStateOnDtor`, `RestorePolicy_DoesNotUnbindTexturesOrUbosButLogsLeak` |
| [RenderStateTest.cpp](../OloEngine/tests/Rendering/RenderStateTest.cpp) | 32 | **RenderState** &mdash; `DefaultsAreCorrect`, `BlendedObjectsUseTransparentSortKey`, `InfiniteGridSortKeyIsTransparent`, `OpaqueObjectHasCorrectState`, `TransparentObjectHasCorrectState`, `TwoSidedMaterialDisablesCulling`, `WireframeModeUsesLinePolygonMode`, `PolygonOffsetForDecals`, `DepthOnlyPassDisablesColorMask`, `CubeWindingOrderIsCCW`, `CubeHas36Indices`, `CubeHas24Vertices`, `CubeNormalsAreUnitLength`, `CubeIndicesInRange`, `ShaderIncludeSkipsComments`, `ShaderIncludePathNoDuplication`, `WireframeCubeState`, `TransparentSphereState`, `PolygonOffsetOverlayState`, `BlendEnabledRequiresTransparentKey`, `OpaqueBeforeTransparentSortInvariant`, `StencilOutlinePassWriteState`, `StencilOutlinePassReadState`, `CylinderTopCapWindingIsCCW`, `CylinderBottomCapWindingIsCCW`, `CylinderSideWindingIsCCW`, `ConeBaseWindingIsCCW`, `ConeSideWindingIsCCW`, `TorusWindingIsCCW`, `MaterialBlendFlagDeterminesSortKeyType`, `TransparentDepthSortsBackToFront`, `BlendedObjectShouldNotWriteDepth` |

#### L5 — Render graph / hazard validation

| File | Tests | Cases |
|---|---:|---|
| [RenderGraphPathSwitchTests.cpp](../OloEngine/tests/Rendering/RenderGraphPathSwitchTests.cpp) | 12 | **RenderGraphPathSwitch** &mdash; `ForwardToDeferredInsertsDeferredPassesAndEdges`, `DeferredToForwardRemovesDeferredOnlyPasses`, `ForwardToDeferredMissingSceneToDecalExplicitEdge_DerivedEdgeSufficient`, `AlternatingRebuildsHaveStablePassCounts`, `PassOrderReflectsCurrentTopologyAfterSwitch`, `ThreeWayCycleCleansUpAllPathSpecificPasses`, `MissingDecalExplicitEdge_DerivedEdgeSufficient`, `MissingLightGridExplicitEdge_DerivedEdgeSufficient`, `DeferredDecalOrderingIsTopologicallyValid`<br/>**RenderGraphDeterminism** &mdash; `ForwardPathBuildOrderIsStableAcrossFreshInstances`, `DeferredPathBuildOrderIsStableAcrossFreshInstances`, `RebuildAfterResetMatchesFreshBuild` |
| [RenderGraphTest.cpp](../OloEngine/tests/Rendering/RenderGraphTest.cpp) | 222 | **RenderGraph** &mdash; `PassNodesAreRetrievableAsRenderPasses`, `GetNodeReturnsNullForUnknown`, `GetNodeSubmissionInfoReturnsAllRegisteredEntries`, `NodeSubmissionInfoReportsResourceDeclarations`, `GraphNodeStaticDeclarationsPopulateRegistryAndSubmissionInfo`, `ValidateExecutionTopologyReturnsFalseForCycles`, `CompiledHazardValidationReportsDynamicFeedbackHazards`, `ExecuteProvidesActiveCommandContextScope`, `SetupSelectedPrimaryInputFramebufferTracksCurrentFirstValidHandle`, `SetupSelectedPrimaryInputFramebufferTextureTracksCurrentFirstValidHandlePair`, `SetupSelectedPrimaryOutputFramebufferTracksCurrentHandleAndResetsBetweenFrames`, `UICompositePublishesVersionedOutputAndFinalPrefersProducerOwnedVersion`, `FXAAPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersion`, `VignettePublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenFXAAIsAbsent`, `ToneMapPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `ColorGradingPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `ChromaticAberrationPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `FogPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `PrecipitationPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `TAAPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `SSSPublishesVersionedOutputAndAOApplyAndBloomPreferProducerOwnedVersion`, `AOApplyPublishesVersionedOutputAndBloomPrefersProducerOwnedVersion`, `BloomPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `DOFPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `MotionBlurPublishesVersionedOutputAndDownstreamPassesPreferProducerOwnedVersionWhenLaterSeamsAreAbsent`, `AddNodeMakesItRetrievable`, `GraphNodeSetupAndExecuteUseCanonicalSubmissionPath`, `GraphNodesDeriveProducerConsumerDependencyFromDeclarations`, `GraphNodeReachabilityCullsUnusedNode`, `GraphNodeFlagsDriveSubmissionMetadata`, `GraphNodeLifecycleHooksTrackInitResizeAndRenderScale`, `PassAddedAfterGraphInitializationInheritsCurrentLifecycleState`, `NodeAddedAfterGraphInitializationInheritsCurrentLifecycleState`, `PlansBarrierFromWriterToReaderTransition`, `PlansFramebufferAndTextureFetchBarrierForRenderTargetToSample`, `ReportsMissingProducerDiagnosticForReadOnlyResource`, `DerivedDependenciesAreRebuiltFromCurrentFrameDeclarations`, `ReportsStaleExtractionHandleDiagnostic`, `ReportsExtractionOfCulledResourceDiagnostic`, `ExtractTextureBeforeBuildRootsProducerAndInvokesCallback`, `ExtractFramebufferBeforeBuildRootsProducerAndInvokesCallback`, `DumpToJsonWritesCompiledGraphDetails`, `BuilderExtractTextureRootsProducerAndInvokesCallback`, `BuilderExtractFramebufferRootsProducerAndInvokesCallback`, `LinearChainOrder`, `DiamondDependency`, `ExecutionDependencyOrdering`, `AllPassesPresentInOrder`, `IndependentPassesAllExecute`, `SetFinalPassIsFinalPass`, `UnreachablePassesAreCulledFromExecution`, `SideEffectingUnreachablePassStillExecutes`, `GetConnectionsComplete`, `MultipleExecuteIdempotent`, `SinglePassGraph`<br/>**RenderGraphExternalTextureSinks** &mdash; `BuilderRegisteredTextureSinkRootsProducerAndInvalidatesWithoutNewCopy`, `BuilderRegisteredFramebufferSinkRootsProducerAndKeepsAttachmentIndex`, `DumpToJsonIncludesExternalTextureSinkContracts`<br/>**RenderGraphStructural** &mdash; `ProductionPassOrderingAlwaysRespected`, `FinalPresentSinkUsesOrderingOnly`, `DuplicateConnectPassIsIdempotent`, `EachPassExecutesExactlyOncePerExecute`, `CycleIsDetectedAndDoesNotCrash`, `DerivedEdgeDoesNotIntroduceReverseCycle`, `BuilderPassDependenciesOverrideReverseRegistrationOrder`, `DerivedEdgesSatisfyDeferredCoreWithoutManualEdges`, `DerivedEdgesSatisfySceneToSSAOWithoutManualEdge`, `DerivedEdgesSatisfySceneToGTAOWithoutManualEdge`, `ConnectingToMissingPassDoesNotCorruptGraph`<br/>**RenderGraphResetTopology** &mdash; `ClearsPassesAndAllowsRebuild`, `PreservesPassReferenceOwnership`, `MultipleResetsAreSafe`, `ImportedHandleSlotsAreRebackedAfterReset`, `ClearsGraphNodeBuildDeclarations`<br/>**RenderGraphTransientPool** &mdash; `StartsEmptyAndReportsZeroMemory`, `ResetAndShutdownKeepPoolEmpty`, `ResizeEvictsStalePoolEntries`, `UnreachableTransientResourceIsNotPlannedForAllocation`, `NonOverlappingTransientResourcesReuseAliasSlot`, `UnsupportedFramebufferFormatIsNotPlannedForAllocation`, `PhaseD_RG16FFramebufferFormatIsNowAllocatable`, `PhaseD_SSAOPassDeclaresTransientRawFramebuffer`, `PhaseD_SSAOAOBufferDeclaredAsTransientTexture`, `PhaseD_SSAOBlurFramebufferDeclaredAsTransientFramebuffer`, `PhaseD_RGBA32FFramebufferFormatIsAllocatable`, `PhaseD_SelectionOutlinePassDeclaresPingPongJFAFramebuffers`, `PhaseD_BloomMipChainDeclaredAsTransientFramebuffers`, `PhaseD_GTAOEdgeTextureDeclaredAsTransientTexture`, `PhaseD_GTAOAOBufferDeclaredAsTransientTexture`, `PhaseD_GTAODenoisePingPongDeclaredAsTransientTextures`, `PhaseD_HZBDepthDeclaredAsTransientMipChainTexture`, `PhaseD_WaterRefractionDeclaredAsTransientTexture`, `RG16FloatTextureIsPlannedForAllocation`, `UnsupportedImageFormatIsNotPlannedForAllocation`, `MissingDimensionsTextureIsNotPlannedForAllocation`, `MissingDimensionsFramebufferIsNotPlannedForAllocation`, `ZeroSizeTransientBufferIsNotPlannedForAllocation`, `DumpToJsonIncludesTransientAliasDiagnostics`, `MaterializationEnabledIsSafeForNonAllocatableTransient`, `TransientTextureIsAllocatedFromPoolWhenMaterializationEnabled`, `MaterializedTransientExtractionReturnsValidTextureWithGpuContext`, `NonAllocatableTransientExtractionInvokesCallbackWithZeroTexture`, `PhaseD_DeclareTransientFramebufferReturnsValidHandleBeforeBuild`, `PhaseD_OITBufferDeclaredAsSharedTransientMRTFramebuffer`, `SameFrameImportsAndBuilderTransientsKeepStableHandles`, `PhaseD_OITBufferDepthAttachmentParticipatesInCompatibility`, `PhaseD_MRTAliasGroupDiffersFromSingleAttachmentKey`, `PhaseD_MRTEstimatedBytesAreCorrect`, `PhaseD_VelocityDeclaredAsTransientTexture`, `PhaseD_SceneDepthDeclaredAsTransientTexture`, `PhaseD_SceneNormalsDeclaredAsTransientTexture`, `PhaseD_DeferredGBufferRootsDeclaredAsTransientTextures`, `PhaseD_DeferredMSCompanionsDeclaredAsTransientTextures`, `PhaseD_SceneColorDeclaredAsTransientMRTFramebuffer`, `PhaseD_PostProcessChainRGBA16FOutputsDeclaredAsTransient`, `PhaseD_PostProcessChainRGBA8OutputsDeclaredAsTransient`, `PhaseD_PostProcessTransientNotInImportedResources`, `PhaseH_ScratchTransientsRemainGraphOwned`, `TrimMaxBucketSizeDefaultIsTwo`, `SetTransientPoolMaxBucketSizeRoundTrips`, `TrimOnEmptyPoolIsNoop`<br/>**RenderGraphTypedHandles** &mdash; `FramebufferAttachmentViewResolvesImportedFramebufferAttachment`, `FramebufferDepthAttachmentViewResolvesImportedFramebufferDepthAttachment`, `ExternallyBackedTransientFramebufferViewsResolveBackingAndRemainTransient`, `ExternallyBackedTransientTextureViewsResolveBackingAndRemainTransient`, `FramebufferAttachmentViewsCanBeCreatedFromTransientFramebufferDescriptors`, `TextureMipViewResolvesImportedTextureAndTracksMipDimensions`, `TextureMipViewsCanBeCreatedFromTransientTextureDescriptors`, `TextureArrayLayerViewResolvesImportedTextureArrayAndTracksLayerMetadata`, `TextureArrayLayerViewsCanBeCreatedFromTransientTextureDescriptors`, `TextureCubeFaceViewResolvesImportedCubeTextureAndTracksFaceMetadata`, `TextureMultisampleResolveViewResolvesSingleSampleBackingAndTracksResolvedMetadata`, `MultisampleParentWriterFeedsResolveViewReaderAcrossCompileStages`, `ParentFramebufferWriterFeedsAttachmentViewReaderAcrossCompileStages`, `ParentTextureWriterFeedsMipViewReaderAcrossCompileStages`, `MipViewWriterFeedsParentTextureReaderAcrossCompileStages`, `ParentTextureWriterFeedsArrayLayerViewReaderAcrossCompileStages`, `ArrayLayerViewWriterFeedsParentTextureReaderAcrossCompileStages`, `ParentTextureWriterFeedsCubeFaceViewReaderAcrossCompileStages`, `CubeFaceViewWriterFeedsParentTextureReaderAcrossCompileStages`<br/>**RenderGraphPassFlags** &mdash; `PassWorkTypeDefaultsToGraphics`, `ComputePassTypeRoundTrips`, `CopyPassTypeRoundTrips`, `AsyncComputeCandidateFlagRoundTrips`, `NeverCullPreventsCulling`, `NodeSubmissionInfoReportsWorkTypeAndAsyncFlag`<br/>**RenderGraphComputeHoist** &mdash; `NoCandidatesLeavesOrderUnchanged`, `IndependentComputePassIsHoistedToFront`, `DependentComputePassRemainsAfterDependency`, `MultipleComputePassesAllHoisted`<br/>**RenderGraphDumpJson** &mdash; `PassFlagsAreSurfacedInDump`, `GraphDigestContainsComputeCountsForAllGraphicsPasses`<br/>**RenderGraphDumpDot** &mdash; `ComputePassColoredDifferentlyToGraphics`<br/>**RenderGraphAsyncBatch** &mdash; `NoCandidatesReturnsEmptyBatches`, `SingleComputePassFormsBatchWithCorrectSignalPass`, `IndependentComputePassHasEmptyWaitAndSignalLists`, `ConsecutiveComputePassesGroupedInOneBatch`, `ComputeBatchWaitsForGraphicsPrerequisite`<br/>**RenderGraphSubmissionPlan** &mdash; `PureGraphicsGraphHasOnlyPassCommands`, `ComputePassWrappedInBatchBeginEnd`, `PassCommandsCarryCorrectWorkType`, `BatchBeginCarriesWaitAndInputResources`, `BatchEndCarriesSignalAndOutputResources`, `DumpToJsonIncludesSubmissionPlan`, `MultipleComputePassesSameIndexGetOneBatchPair`, `PlanPreservesHoistedExecutionOrder`<br/>**RenderGraphExecutePlanDriven** &mdash; `PureGraphicsGraphPassesExecuteInOrder`, `CulledPassIsSkippedInPlanDrivenExecution`, `BatchEventHookFiresBeginAndEndForAsyncComputePass`, `BatchEventHookBatchIndexIsZeroForFirstBatch`, `PostPassHookStillFiresForEachPass`<br/>**RenderGraphTemporalHistoryContracts** &mdash; `ExtractHistoryTextureRecordsContractAndInvokesCallback`, `BuilderDeclaredHistoryExtractionRootsProducerAndDeduplicatesRuntimeContract`, `RegisteredHistorySinkCountsAsImportedAndInvalidatesWithoutNewCopy`, `InvalidHistoryContractReportsDiagnosticAndSkipsCallback`, `DumpToJsonIncludesHistoryResourcesAndContracts`, `ExtractHistoryTextureFromFramebufferAttachmentRecordsContractAndInvokesCallback`<br/>**RenderGraphAsyncBatchResources** &mdash; `NoBatchResourceDepsWhenNoAccessDeclarations`, `SingleResourceFlowsIntoBatch`, `BatchOutputFlowsToGraphicsPass`, `IndependentBatchHasNoCrossBoundaryResources`, `DumpToJsonIncludesBatchResourceDeps`<br/>**RenderGraphResourceTransitions** &mdash; `NoTransitionsWhenNoBarriersPlanned`, `SingleTransitionCapturesProducerAndConsumer`, `ProducerIsLastWriterBeforeConsumer`, `ExternalImportHasNoProducerPass`, `DumpToJsonIncludesResourceTransitions`<br/>**RenderGraphResourceLifetimes** &mdash; `NoLifetimesWhenNoResourcesDeclared`, `TransientResourceHasCorrectFirstAndLastPass`, `ImportedResourceHasExternalFirstWrite`, `WriteOnlyResourceHasEmptyLastRead`, `DumpToJsonIncludesResourceLifetimes`<br/>**RenderGraphSubresourceRange** &mdash; `FullRangeByDefaultWhenNoRangeSpecified`, `MipRangePreservedInTransition`, `LayerRangePreservedInTransition`, `MultiLayerDeclarationsProducePerLayerTransitions`, `SliceRangePreservedInTransition`, `PlannedBarrierCarriesRange`, `DumpToJsonIncludesRange`<br/>**RenderGraphCrossLaneSync** &mdash; `PureGraphicsGraphHasNoCrossLaneTransitions`, `ComputeToGraphicsTransitionIsCrossLane`, `DumpToJsonIncludesCrossLaneSyncFields`<br/>**RenderGraphQueueAwareScheduler** &mdash; `LegalOverlapDisjointResourcesNoHazard`, `ForbiddenOverlapComputeWritesAfterGraphicsRead`, `OrderingPreservedAfterComputeHoist`, `GTAOStyleComputeToGraphicsCrossLaneTransition`, `HazardValidatorRemainsGreenAfterComputeHoist`<br/>**RenderGraphResolveFailureTelemetry** &mdash; `InvalidTypedHandleResolvesAreRecorded`, `ValidTextureResolveDoesNotEmitTextureFailure`, `DumpToJsonUsesResolveFailureFieldNames`<br/>**RenderGraphSceneColorChain** &mdash; `RMWChainDrivesRAWEdgesViaBuilderCallbacks`, `DeferredSceneColorRMWChainViaBuilderCallbacksIsHazardFree`, `RmwPassRemainsReachableWhenAllOptionalRmwChainStepsAreAbsent`, `EarlierRmwPassRemainsReachableWhenLaterRmwPassOverwritesLatestVersion`, `RmwContributorsRemainReachableWhenConsumerReadsBaseResource`<br/>**RenderGraphBuildDiagnostics** &mdash; `RegistrationOrderSensitivityIsReportedForReverseRmwChain`, `ExplicitDependencyRemovesRegistrationOrderSensitivity`, `SetupDeclaredPassDependencyRemovesRegistrationOrderSensitivity`, `TransitiveSetupDependenciesSuppressRedundantRegistrationOrderSensitivity`, `SceneColorRMWChainIsRegistrationOrderIndependentWithExplicitDependsOnPass`, `OITAccumContributorChainIsRegistrationOrderIndependentWithExplicitDependsOnPass`, `DumpToJsonIncludesRegistrationOrderSensitivityDiagnostics` |
| [ResourceHazardValidationTests.cpp](../OloEngine/tests/Rendering/ResourceHazardValidationTests.cpp) | 101 | **RenderGraphResourceHazards** &mdash; `LinearChainWithHandoffIsHazardFree`, `DeclaredRAWPair_DerivedEdgePreventsHazardWithoutExplicitDependency`, `Slice27_DeclarationChainTransitivityIsHazardFree`, `WriteAfterReadWithoutDependencyIsFlagged`, `TransitiveDependencyCountsAsDependency`, `DiamondReadersOfSharedResourceIsHazardFree`, `ReadOnlyResourceHasNoHazards`, `UndeclaredPassDoesNotContributeHazards`, `ProductionShapedGraphIsHazardFree`, `ProductionShapedGraph_DerivedEdgePreventsHazardForShadow`, `IblProducerConsumerIsHazardFree`, `IblDeclarationsAloneSufficient`, `UICompositeInChainIsHazardFree`, `UICompositeSkippedByFinalIsFlagged`, `ResourceHandleEqualityIsNameBased`, `ImportedResourceIsTrackedByRegistry`, `TypedHandleLookupMatchesDeclaredKinds`, `UnknownResourceHandleLookupReturnsInvalid`, `StaleTypedHandleIsRejectedAfterTopologyReset`, `RecreatedResourceGetsNewGeneration`, `SamePassOverlappingReadWriteWithoutFeedbackIsFlagged`, `SamePassOverlappingReadWriteWithFeedbackIsAllowed`, `FeedbackDeclarationIsRangeScoped`, `DynamicDecalProjectionContractDerivesSceneDepthProducerEdge`, `DynamicOITDepthContractDerivesPrepareAndContributorEdges`, `ImportedProducedAndConsumedWithoutBackingIsFlagged`, `WriteNewVersionClonesFramebufferDescriptorAndUsesDistinctNames`, `ExplicitVersionHandlesDeriveRewriteChainsWithoutManualDependencies`<br/>**RenderGraphConfigureTopology** &mdash; `ForwardPathIsHazardFree`, `ForwardPlusPathIsHazardFree`, `DeferredPathIsHazardFree`, `StartupBaselineEdges_DerivedEdgesMakeGraphHazardFree`, `Slice28_AOApplyPassToPostProcessPassDerivedEdge`, `Slice28_FullPostChainNoExplicitEdgesIsHazardFree`, `Slice28_FXAAAndSelectionOutlineVariantIsHazardFree`, `Slice28_SelectionOutlineOnlyVariantIsHazardFree`, `Slice29_ParticleToOITResolveDerivedEdge`, `Slice29_OITResolveToSSSPassDerivedEdge`, `Slice29_FullGeometryTailNoExplicitEdgesIsHazardFree`, `Slice30_SSAOToAOApplyDerivedEdge`, `Slice30_GTAOToAOApplyDerivedEdge`, `Slice30_DualAOWritersNeedExplicitOrdering`, `Slice31_SceneToSSAODerivedEdgeWithoutWaterEdge`, `Slice31_SceneToGTAODerivedEdgeWithoutWaterEdge`, `Slice32_SceneToFoliageDerivedFromSceneColor`, `Slice32_FoliageToDecalDerivedFromSceneColor`, `Slice32_DecalToWaterDerivedFromSceneColor`, `Slice32_WaterToParticleDerivedFromSceneColor`, `Slice33_SceneToDeferredOpaqueDecalDerivedFromSceneDepth`, `Slice33_DeferredOpaqueDecalToDeferredLightingDerivedFromSceneColor`, `Slice33_DeferredLightingToForwardOverlayDerivedFromSceneColor`, `Slice33_ForwardOverlayToFoliageDerivedFromSceneColor`, `MissingShadowToSceneExplicitEdge_DerivedEdgeSufficient`, `SceneToDeferredLightingCanBeTransitiveViaDecal`, `MissingSceneToDeferredOpaqueDecalExplicitEdge_DerivedEdgeSufficient`, `ForwardPathWithSelectionOutlineIsHazardFree`, `DeferredPathWithSelectionOutlineIsHazardFree`, `ForwardPathWithGTAOIsHazardFree`, `DeferredPathWithGTAOIsHazardFree`, `DecalNodePresence`, `DecalNodeOrderingIsHazardFree`, `ResetTopologyAndRebuildAcrossPathsNoLeaks`, `Slice34_SSAOOnlyInGraphHasNoAOBufferWAW`, `Slice34_GTAOOnlyInGraphHasNoAOBufferWAW`, `Slice34_NoneAOTechniqueNoAOPassInGraphIsHazardFree`, `Slice35_OITResolveAndSSSOrderingDerivesFromDeclarations`, `Slice35_SSSColorRAWEdgeToAOApplyDerivesFromDeclarations`, `Slice36_ForwardGeometryPassesSceneColorRAWEdgeFromDeclarations`, `Slice36_DecalPassSceneDepthRAWEdgeFromDeclarations`, `Slice36_ParticleAndWaterAfterSceneColorWriter`, `Slice36_FoliageAndOverlayAfterSceneColorWriter`, `PhaseH_DecalPassResolvesSceneDepthFromBlackboardOnly`, `Slice37_SSAOPassSelfResolvesSceneDepthAndNormals`, `Slice37_GTAOPassSelfResolvesSceneDepthAndNormals`, `Slice37_AOPassesAfterSceneDepthWriterNoHazards`, `Slice38_AOApplyPassSelfResolvesSceneColor`, `Slice38_AOApplyPassSelfResolvesAOBufferAndSceneDepth`, `Slice38_AOApplyPassPrefersSSSColorOverSceneColor`, `Slice39_PostProcessPassSelfResolvesInputChain`, `Slice39_PostProcessPassSelfResolvesSceneDepthAndAOBuffer`, `Slice39_PostProcessPassSelfResolvesShadowMapAndVelocity`, `Slice40_DOFPassSelfResolvesInputAndSceneDepth`, `Slice40_MotionBlurPassSelfResolvesInputChain`, `Slice40_TAAPassSelfResolvesInputDepthAndVelocity`, `Slice41_DeferredLightingPassSelfResolvesSceneColorAndGBuffer`, `Slice41_DeferredLightingPassSelfResolvesSceneDepth`, `Slice41_DeferredLightingPassSelfResolvesMSAAVariants`, `Slice42_FogPassSelfResolvesInputAndSceneDepth`, `Slice42_FogPassSelfResolvesShadowMapCSM`, `Slice42_FogPassSelfResolvesUpstreamChain`, `PhaseH_PostChainDepthAndShadowUsersResolveBlackboardInputsOnly`, `Slice43_BloomPassSelfResolvesPostProcessColor`, `Slice43_ChromaticAberrationPassSelfResolvesUpstreamChain`, `Slice43_ColorGradingPassSelfResolvesUpstreamChain`, `Slice43_ToneMapAndVignettePassSelfResolveUpstream`, `Slice44_FXAAPassSelfResolvesUpstreamChain`, `Slice44_PrecipitationPassSelfResolvesUpstreamChain`, `Slice44_SelectionOutlinePassSelfResolvesUpstreamChain`, `Slice44_UICompositeAndFinalPassSelfResolveUpstream`<br/>**RGCommandContextBlackboard** &mdash; `Slice35_GetBlackboardReturnsNullptrWithoutGraph`, `Slice35_GetBlackboardReturnsGraphBlackboardWhenAttached` |

#### L6 — Performance regression

| File | Tests | Cases |
|---|---:|---|
| [CommandBucketBenchmarkTest.cpp](../OloEngine/tests/Rendering/CommandBucketBenchmarkTest.cpp) | 4 | **SortScalingTest** &mdash; `SortCompletesAndOrderIsValid`<br/>**CommandBucketBenchmark** &mdash; `ParallelSubmitAndMerge`, `AllocatorResetStability`, `LargeCommandMemoryPressure`<br/>*Parametrised via 1 `INSTANTIATE_TEST_SUITE_P`* |
| [PerfRegressionTests.cpp](../OloEngine/tests/Rendering/PropertyTests/PerfRegressionTests.cpp) | 6 | **PerfRegressionTest** &mdash; `ToneMapPassTimingIsMeasurable`, `BloomThresholdPassTimingIsMeasurable`, `BloomDownsamplePassTimingIsMeasurable`, `BloomUpsamplePassTimingIsMeasurable`, `WholeFramePostprocessChainTimingIsMeasurable`, `SceneDrawBurstBudget` |

#### L7 — Smoke / sanity readback

| File | Tests | Cases |
|---|---:|---|
| [AssetPreviewRendererTest.cpp](../OloEngine/tests/Rendering/PropertyTests/AssetPreviewRendererTest.cpp) | 5 | **AssetPreviewRendererTest** &mdash; `InitializeAndShutdownAreIdempotent`, `RenderMaterialPreviewReturnsCorrectlySizedTexture`, `RenderMaterialPreviewWithNullMaterialReturnsNull`, `RenderMeshPreviewWithoutMaterialUsesDefaultAndProducesTexture`, `RenderingBeforeInitializeReturnsNullWithoutCrashing` |
| [RendererValidateTest.cpp](../OloEngine/tests/Rendering/PropertyTests/RendererValidateTest.cpp) | 5 | **RendererValidateTest** &mdash; `CleanFramebufferPassesValidation`, `NanPixelsAreDetected`, `InfPixelsAreDetected`, `Fp16OverflowIsDetected`, `RejectsUnsupportedFormatsGracefully` |
| [VideoOverlayVisualEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/VideoOverlayVisualEvidenceTest.cpp) | 1 | **VideoOverlayScene** &mdash; `FullscreenImageCompositesLetterboxed` |

#### L8 — Golden image

| File | Tests | Cases |
|---|---:|---|
| [AutoExposureEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/AutoExposureEvidenceTest.cpp) | 1 | **AutoExposureBrightScene** &mdash; `AutoExposureDarkensAnOverBrightScene` |
| [GoldenImageTests.cpp](../OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp) | 8 | **GoldenImageSsimTest** &mdash; `IdenticalImagesYieldSsimOne`, `TinyUniformShiftKeepsSsimHigh`, `StructuralDestructionCollapsesSsim`, `SsimIsSymmetric`<br/>**GoldenImageTest** &mdash; `ReinhardHdrRampGolden`, `FxaaHardEdgeGolden`, `SceneShadowIntegrationGolden`, `SceneSplatmapIntegrationGolden` |
| [OceanFFTVisualEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/OceanFFTVisualEvidenceTest.cpp) | 3 | **OceanFFTVisualEvidenceTest** &mdash; `CaptureFFTOceanFromMultipleAngles`, `FFTToggleVisiblyChangesSurface`, `GpuComputeToggleLeavesSurfaceUnchanged` |
| [PCSSVisualEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/PCSSVisualEvidenceTest.cpp) | 1 | **PCSSVisualEvidenceTest** &mdash; `SoftShadowsRenderAndDifferFromHardPCF` |
| [ProceduralSkyBakeTest.cpp](../OloEngine/tests/Rendering/PropertyTests/ProceduralSkyBakeTest.cpp) | 2 | **ProceduralSkyBakeTest** &mdash; `GeneratesNonBlackEnvironmentMapWithIBL`, `HigherTurbidityProducesBrighterAverage` |
| [ProceduralSkyVisualTest.cpp](../OloEngine/tests/Rendering/PropertyTests/ProceduralSkyVisualTest.cpp) | 1 | **ProceduralSkyVisualTest** &mdash; `WritesFaceGridPng` |
| [SSGIVisualEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/SSGIVisualEvidenceTest.cpp) | 1 | **SSGIVisualEvidenceTest** &mdash; `ColorBleedAppearsOnFloor` |
| [SSRVisualEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/SSRVisualEvidenceTest.cpp) | 1 | **SSRVisualEvidenceTest** &mdash; `ReflectionAppearsOnMirrorFloor` |
| [SceneRenderEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/SceneRenderEvidenceTest.cpp) | 1 | **LitCubeScene** &mdash; `RendersThroughScenePipelineAndProducesPng` |
| [SphereAreaLightVisualTest.cpp](../OloEngine/tests/Rendering/PropertyTests/SphereAreaLightVisualTest.cpp) | 1 | **SphereAreaLightVisual** &mdash; `RendersAndProducesPng` |
| [StarNestReflectionEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/StarNestReflectionEvidenceTest.cpp) | 1 | **MetallicSphereUnderNebula** &mdash; `ReflectsNebulaThroughScenePipeline` |
| [StarNestSkyBakeTest.cpp](../OloEngine/tests/Rendering/PropertyTests/StarNestSkyBakeTest.cpp) | 2 | **StarNestSkyBakeTest** &mdash; `GeneratesNonBlackEnvironmentMapWithIBL`, `HigherIntensityProducesBrighterAverage` |
| [StarNestSkyVisualTest.cpp](../OloEngine/tests/Rendering/PropertyTests/StarNestSkyVisualTest.cpp) | 1 | **StarNestSkyVisualTest** &mdash; `WritesFaceGridPng` |
| [TerrainGenerationEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/TerrainGenerationEvidenceTest.cpp) | 1 | **TerrainGenerationEvidenceTest** &mdash; `GeneratedTerrainIsTexturedAndBanded` |
| [UnderwaterCausticsVisualTest.cpp](../OloEngine/tests/Rendering/PropertyTests/UnderwaterCausticsVisualTest.cpp) | 5 | **UnderwaterCausticsVisualTest** &mdash; `CausticsBrightenAndTextureTheSeabed`, `RefractionDistortsTheSubmergedImage`, `GodRaysBrightenWhenLookingTowardSun`, `GodRaysFormVisibleShaftsThroughOccluders`, `GodRaysVanishWhenSunBehindCamera` |
| [WaterBuoyancyVisualEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/WaterBuoyancyVisualEvidenceTest.cpp) | 1 | **FloatingCubeOnOcean** &mdash; `CubeFloatsOnRenderedOceanAndProducesPng` |
| [WaterVisualEvidenceTest.cpp](../OloEngine/tests/Rendering/PropertyTests/WaterVisualEvidenceTest.cpp) | 1 | **WaterVisualEvidenceTest** &mdash; `CaptureWaterFromMultipleAngles` |

#### plumbing — Pipeline plumbing (command bucket, dispatch, frame data)

| File | Tests | Cases |
|---|---:|---|
| [CommandAllocatorTest.cpp](../OloEngine/tests/Rendering/CommandAllocatorTest.cpp) | 17 | **CommandAllocator** &mdash; `AllocateReturnsNonNull`, `AllocateMultipleNonOverlapping`, `AlignmentIs16Byte`, `MultiBlockAllocation`, `ResetReusesMemory`, `CreateCommandPacketProducesValidPacket`, `AllocatePacketWithCommandPlacementNew`, `StressTestManyAllocations`, `AllocationCountTracksCorrectly`, `ThreadCacheIsReusedAfterReset`, `MultiBlockResetDoesNotLeak`<br/>**ThreadLocalCache** &mdash; `SingleBlockForSmallAllocations`, `MultiBlockAllocationGrows`, `ResetReusesBlocksWithoutLeaking`, `ResetAndReallocateSameMemoryFootprint`, `OversizedAllocationGetsLargerBlock`, `AllocateReturnsAlignedPointers` |
| [CommandBucketTest.cpp](../OloEngine/tests/Rendering/CommandBucketTest.cpp) | 27 | **CommandBucketTest** &mdash; `SortPreservesAllCommands`, `SortOrderMatchesDrawKey`, `SortStabilityForEqualKeys`, `EmptyBucketSort`, `SingleCommandSort`, `ClearResetsState`, `ResetFreesMemory`, `SortReducesStateChanges`, `StatisticsTrackSubmissions`, `ParallelSubmissionMerge`, `MixedCommandTypes`, `TimingAccessors`, `DisabledSortingSkipsSort`<br/>**CommandBucketBatchTest** &mdash; `BatchConvertsMeshToInstanced`, `BatchRejectsDifferentRenderStateIndex`, `BatchAcceptsSameRenderStateIndex`, `BatchRejectsDifferentMaterialDataIndex`, `AnimatedMeshesAreNotBatched`, `HashTableGroupsNonAdjacentCommands`, `SingleCommandGroupsRemainDrawMesh`, `BatchRespectsMaxMeshInstances`, `BatchedTransformsAreContiguous`, `TenThousandInstancesCollapseToSinglePacket`, `BatchedEntityIDAndPrevTransformSurviveCollapse`, `IdentityColorAndCustomSkipParallelStreamAllocation`, `SameLODBatchesAndDifferentLODsStaySeparate`, `NonDefaultColorAndCustomSurviveCollapse` |
| [CommandDispatchTest.cpp](../OloEngine/tests/Rendering/CommandDispatchTest.cpp) | 8 | **CommandDispatch** &mdash; `DispatchTableIsComplete`, `InvalidTypeReturnsNull`, `CommandTypeToStringCoversAll`, `PODRenderStateFieldCount`<br/>**CommandDispatchIntegration** &mdash; `SetViewportDispatch`, `ClearDispatch`, `SetDepthTestDispatch`, `BucketExecuteDispatchesInOrder` |
| [CommandPacketTest.cpp](../OloEngine/tests/Rendering/CommandPacketTest.cpp) | 16 | **CommandPacket** &mdash; `InitializePopulatesType`, `InitializeSetsCommandType`, `DrawMeshPacketStoresShaderAndMaterialKeys`, `GetCommandDataReturnsCorrectType`, `CommandSizeMatchesType`, `MultiplePacketsAreIndependent`, `ComparisonFollowsDrawKeySorting`, `CanBatchWithSameTypeSameKeyFields`, `CannotBatchDifferentCommandTypes`, `CannotBatchDifferentRenderStateIndex`, `CanBatchSameRenderStateIndex`, `CloneDeepCopiesData`, `SortKeyPreservedFromMetadata`, `DefaultMetadataKeepsSortKeyZeroed`, `AllocatePacketWithCommandPath`, `MetadataDependencyAndStaticFlags` |
| [DrawKeyTest.cpp](../OloEngine/tests/Rendering/DrawKeyTest.cpp) | 18 | **DrawKey** &mdash; `BitwiseRoundTrip_Opaque`, `BitwiseRoundTrip_Transparent`, `BitwiseRoundTrip_Custom`, `OpaqueBeforeTransparent`, `OpaqueDepthFrontToBack`, `TransparentDepthBackToFront`, `TotalOrdering`, `SortStability`, `SettersAreIndependent`, `DefaultIsZero`, `ExplicitU64Constructor`, `EqualityOperator`, `HigherViewportSortsLast`, `ViewLayerOrderingWithinViewport`, `SameShaderGroupsTogether`, `ToStringViewLayerType`, `ToStringRenderMode`<br/>**DrawKeyFieldCombinations** &mdash; `PackingRoundTrip`<br/>*Parametrised via 1 `INSTANTIATE_TEST_SUITE_P`* |
| [FrameCaptureTest.cpp](../OloEngine/tests/Rendering/FrameCaptureTest.cpp) | 30 | **CapturedCommandData** &mdash; `ConstructionPreservesFields`, `TypedAccessRoundTrip`, `NullDataHandled`, `IsDrawCommand`, `IsStateCommand`, `IsBindCommand`, `GpuTimingAccessors`, `CopyIsIndependent`<br/>**CapturedFrameData** &mdash; `CanStoreCommandsAtMultipleStages`<br/>**FrameCaptureManager** &mdash; `InitialStateIsIdle`, `CaptureNextFrameTransition`, `StartStopRecording`, `MaxCapturedFramesConfig`, `ClearCaptures`, `SelectedFrameIndex`, `GetSelectedFrameReturnsNulloptWhenEmpty`<br/>**FrameCapturePipelineTest** &mdash; `CaptureNextFrameRecordsSingleFrame`, `CapturedCommandsPreserveTypes`, `RecordingCapturesMultipleFrames`, `MaxCapturedFramesTrimsOldest`, `PostSortOrderDiffersFromPreSort`, `DrawCallAndStateChangeStats`, `CaptureGenerationIncrements`<br/>**FrameExportTest** &mdash; `ExportToCSVCreatesValidFile`, `ExportToMarkdownCreatesValidFile`, `ExportToCSVWithNoSelectedFrameFails`, `ExportToMarkdownWithNoSelectedFrameFails`, `GenerateExportFilenameContainsFrameNumber`, `CSVContainsCorrectSortKeyData`, `MarkdownSortAnalysisPresent` |
| [FrameDataBufferTest.cpp](../OloEngine/tests/Rendering/FrameDataBufferTest.cpp) | 23 | **FrameDataBuffer** &mdash; `BoneMatrixWriteReadRoundTrip`, `TransformWriteReadRoundTrip`, `MultipleAllocationsNonOverlapping`, `ResetClearsState`, `AllocationFailsWhenFull`, `NoCapacityCreepOverResetCycles`, `StatisticsTrack`, `WorkerScratchIsolation`, `PrepareAndMergeCycle`, `RenderStateTableAllocateReturnsValidIndex`, `RenderStateTableDeduplicatesIdenticalStates`, `RenderStateTableDifferentStatesGetDifferentIndices`, `RenderStateTableRoundTrip`, `RenderStateTableResetsEachFrame`, `RenderStateTableMultipleUniqueStates`, `MaterialDataTableAllocateReturnsValidIndex`, `MaterialDataTableDeduplicatesIdenticalData`, `MaterialDataTableDifferentDataGetDifferentIndices`, `MaterialDataTableRoundTrip`, `MaterialDataTableResetsEachFrame`, `MaterialDataTableMultipleUniqueEntries`, `PBRMaterialAllTextureFieldsRoundTrip`, `PBRAndLegacyMaterialsDedupIndependently` |
| [FramePipelineTest.cpp](../OloEngine/tests/Rendering/FramePipelineTest.cpp) | 6 | **FramePipelineTest** &mdash; `OpaqueBeforeTransparent`, `AllSubmittedCommandsPresent`, `SortReducesShaderStateChanges`, `IsolatedBuckets`, `MultiFrameResetCycle`, `ViewLayerSortingPriority` |
| [PODCommandTest.cpp](../OloEngine/tests/Rendering/PODCommandTest.cpp) | 8 | **PODCommand** &mdash; `AllCommandsTrivialCopy`, `CommandSizeBound`, `DrawMeshFieldRoundTrip`, `ZeroInitNoNaN`, `PODRenderStateTrivialCopy`, `PODRenderStateDefaults`, `CommandTypeToStringCoverage`, `CommandHeaderDefault` |
| [RenderGraphFingerprintTest.cpp](../OloEngine/tests/Rendering/RenderGraphFingerprintTest.cpp) | 4 | **RenderGraphFingerprint** &mdash; `SSREnableTogglesFingerprint`, `EveryTopologyToggleChangesFingerprint`, `AOTechniqueAndRenderPathChangeFingerprint`, `ValueOnlyParamDoesNotChangeFingerprint` |

#### cullinglod — Culling, LOD, occlusion

| File | Tests | Cases |
|---|---:|---|
| [BoundingVolumeTest.cpp](../OloEngine/tests/Rendering/BoundingVolumeTest.cpp) | 17 | **BoundingBox** &mdash; `ConstructFromMinMax`, `CenterIsAverage`, `SizeIsCorrect`, `ExtentsAreHalfSize`, `ContainsCorners`, `UnionContainsBoth`, `TransformPreservesContainment`, `ConstructFromPoints`, `DegenerateZeroSizeBox`, `ConstructFromZeroPoints`<br/>**BoundingSphere** &mdash; `ConstructFromCenterRadius`, `ContainsCenter`, `ConstructFromBoundingBox`, `ConstructFromPoints`, `DegenerateZeroRadius`, `TransformPreservesContainment`, `ConstructFromZeroPoints` |
| [FrustumCullingTest.cpp](../OloEngine/tests/Rendering/FrustumCullingTest.cpp) | 20 | **Frustum** &mdash; `OriginVisibleInDefaultFrustum`, `PointBehindCameraNotVisible`, `PointBeyondFarPlaneNotVisible`, `PointBeforeNearPlaneNotVisible`, `FullyInsideSphereNeverCulled`, `FullyOutsideSphereAlwaysCulled`, `SphereTouchingPlaneIsVisible`, `BoundingSphereVisibility`, `BoxInsideFrustumVisible`, `BoxOutsideFrustumNotVisible`, `BoundingBoxVisibility`, `OrthographicFrustumCulling`, `UpdateChangesPlanes`, `PlaneNormalsAreNormalized`, `StressRandomSpheres_NoNaN`<br/>**Plane** &mdash; `ConstructFromNormalAndDistance`, `ConstructFromNormalAndPoint`, `SignedDistanceAbovePositive`, `SignedDistanceBelowNegative`, `SignedDistanceOnPlaneZero` |
| [LODTest.cpp](../OloEngine/tests/Rendering/LODTest.cpp) | 14 | **LODGroup** &mdash; `EmptyGroupReturnsInvalid`, `SingleLevelAlwaysSelected`, `MultipleLevelsSelectCorrectly`, `BoundaryDistancesSelectCorrectLevel`, `BiasOneHasNoEffect`, `BiasGreaterThanOneKeepsHighDetailLonger`, `BiasLessThanOneFavorsLowerDetail`, `VeryHighBiasAlwaysSelectsHighestDetail`, `VeryLowBiasAlwaysSelectsLowestDetail`, `ZeroDistanceSelectsFirstLevel`, `NegativeDistanceTreatedAsVeryClose`, `VeryLargeDistanceSelectsLastLevel`, `AllLevelsSameDistanceSelectsFirst`, `ManyLevelsCorrectSelection` |
| [MeshOptimizationTest.cpp](../OloEngine/tests/Rendering/MeshOptimizationTest.cpp) | 28 | **MeshOptimization** &mdash; `OptimizeMeshPreservesVertexCount`, `OptimizeMeshPreservesTriangleContent`, `OptimizeMeshHandlesEmptyMesh`, `OptimizeMeshRemapsBoneInfluences`, `GenerateLODReducesTriangles`, `GenerateLODReturnsNullForEmptyMesh`, `GenerateLODPreservesVertexValidity`, `GenerateLODWithVeryLowRatioProducesMinimalMesh`, `GenerateShadowIndicesProducesSameCount`, `GenerateShadowIndicesValidIndices`, `GenerateShadowIndicesHandlesEmptyMesh`, `OptimizeMeshGeneratesShadowIndices`, `GenerateLODWithAttributesReducesTriangles`, `GenerateLODWithAttributesHandlesEmptyMesh`, `AnalyzeMeshReturnsValidStats`, `AnalyzeMeshHandlesEmptyMesh`, `OptimizedMeshHasBetterCacheStats`, `GenerateMeshletsProducesOutput`, `GenerateMeshletsRespectsLimits`, `GenerateMeshletsHandlesEmptyMesh`, `MeshletBoundsHavePositiveRadius`, `SpatialSortPreservesGeometry`, `SpatialSortHandlesEmptyMesh`, `EncodeDecodeVertexBufferRoundTrip`, `EncodeDecodeIndexBufferRoundTrip`, `EncodeVertexBufferCompresses`, `ShadowIndicesAreSpatialSorted`, `AttributeAwareLODProducesValidOutput` |
| [MeshSourceBoundsTest.cpp](../OloEngine/tests/Rendering/MeshSourceBoundsTest.cpp) | 4 | **MeshSourceBounds** &mdash; `StaticMeshReportsNoBoneInfluences`, `StaticMeshLocalAABBIsTightAroundVertices`, `RiggedMeshExpandsAABBAfterRealBoneDataSet`, `ZeroWeightSetVertexBoneDataDoesNotFlagAsSkinned` |
| [OcclusionIntegrationTest.cpp](../OloEngine/tests/Rendering/OcclusionIntegrationTest.cpp) | 17 | **OcclusionIntegration** &mdash; `DrawMeshCommandDefaultQueryIndex`, `DrawMeshCommandQueryIndexRoundTrip`, `DrawMeshCommandStillTriviallyCopyable`, `DrawMeshCommandSizeBound`, `ParticleSphereInsideFrustum`, `ParticleSphereBehindCamera`, `ParticleSpherePartiallyIntersecting`, `ParticleSphereBeyondFarPlane`, `FoliageBoundsInsideFrustum`, `FoliageBoundsBehindCamera`, `FoliageBoundsFarAway`, `PerInstanceCullingFiltersCorrectly`, `PerInstanceCullingAllVisible`, `PerInstanceCullingNoneVisible`, `BoundingBoxCenterExtentsForProxy`, `BoundingBoxAsymmetricProxy`, `BoundingSphereFromBoundingBox` |
| [OcclusionStateTest.cpp](../OloEngine/tests/Rendering/OcclusionStateTest.cpp) | 19 | **OcclusionState** &mdash; `DefaultConstruction`, `MutableFields`<br/>**OcclusionStateManagerTest** &mdash; `GetOrCreateNewState`, `GetOrCreateReturnsSameState`, `HasReturnsFalseForUnknown`, `HasReturnsTrueAfterCreate`, `RemoveDeletesState`, `RemoveNonExistentNoOp`, `MultipleObjects`, `AllocateSequential`, `AllocateRespectsMaxQueries`, `FreeAndReallocate`, `RemoveFreesQueryIndex`, `RemoveWithNoQueryNoFreeListCorruption`, `FrameCounterStartsAtZero`, `BeginFrameIncrementsCounter`, `ClearResetsFrameCounter`, `SimulateTemporalCoherence`, `StressAllocFree` |

#### shaderpipe — Shader pack / binding layout / ShaderGraph

| File | Tests | Cases |
|---|---:|---|
| [AutoExposureMathTest.cpp](../OloEngine/tests/Rendering/AutoExposureMathTest.cpp) | 19 | **AutoExposureMathTest** &mdash; `LuminanceUsesRec709Weights`, `BlackPixelsGoToBinZero`, `BinMappingIsMonotonicAndClamped`, `BinRoundTripRecoversLuminance`, `UniformHistogramRecoversThatLuminance`, `AllBlackFloorsAtMinLogLum`, `BlackPixelsDoNotDragTheAverage`, `MixedHistogramAveragesBetweenExtremes`, `AdaptationConvergesToTarget`, `AdaptationIsFrameRateIndependent`, `SeparateUpAndDownSpeeds`, `AdaptationEdgeCases`, `DoublingLuminanceIsOneStopOfEV`, `BrighterSceneGivesSmallerExposure`, `ExposureCompensationIsInStops`, `ExposureRespectsClamp`, `StepDrivesExposureTowardSceneOverTime`, `SanitizeReplacesNonFiniteWithFiniteDefaults`, `SanitizeOrdersInvertedMinMaxPairs` |
| [GPUFrustumCullParityTest.cpp](../OloEngine/tests/Rendering/GPUFrustumCullParityTest.cpp) | 2 | **GPUFrustumCullParity** &mdash; `RandomisedInstancesMatchCPU`, `EdgeCaseInstancesMatchCPU` |
| [InstanceDataLayoutTest.cpp](../OloEngine/tests/Rendering/InstanceDataLayoutTest.cpp) | 6 | **InstanceDataLayout** &mdash; `StructSizeMatchesStd430`, `FieldOffsetsMatchGLSLBlock`, `DefaultsAreIdentityAndNeutral`, `BindingConstantIsStable`, `GLSLLayoutMentionsAllFieldsAndBinding`<br/>**InstancedMeshComponentDefaults** &mdash; `FieldsHaveSafeDefaults` |
| [ProceduralSkyMathTest.cpp](../OloEngine/tests/Rendering/ProceduralSkyMathTest.cpp) | 11 | **ProceduralSkyMath** &mdash; `PerezCoefficientsAreLinearInTurbidity`, `CoefficientsMatchReferenceAtT2_5`, `ZenithLuminanceIsPositiveAtCommonElevations`, `ZenithChromaticityIsInPlausibleRange`, `SubHorizonSunIsClampedAboveHorizon`, `ZeroLengthSunDefaultsToZenith`, `SunDiskCosAngleIsConsistent`, `HashChangesWhenParametersChange`, `SkyIsBrighterTowardSunThanAntiSun`, `BluerAwayFromSunForClearSky`, `AllOutputsAreFinite` |
| [SSRHiZTraversalTest.cpp](../OloEngine/tests/Rendering/SSRHiZTraversalTest.cpp) | 5 | **SSRHiZTraversal** &mdash; `MinPyramidStoresNearestSurfaceOfEachBlock`, `SkipPredicateNeverHidesACloserSurface`, `HiZSkippingMatchesNoSkipMarch`, `HiZTakesLargeStepsThroughEmptySpace`, `NoSkipDDACrossChecksIndependentLinearMarch` |
| [ScatterBrushMathTest.cpp](../OloEngine/tests/Rendering/ScatterBrushMathTest.cpp) | 7 | **ScatterBrushSlopeFilter** &mdash; `FlatGroundPasses`, `FortyFiveDegreeAtDot07RoughlyAtThreshold`, `ZeroThresholdDisablesFilter`, `OverhangFails`<br/>**ScatterBrushVariantEncoding** &mdash; `SingleVariantProducesZero`, `EndpointsAreExactlyZeroAndOne`, `OutOfRangeIndexClamps` |
| [ScreenSpaceGIMathTest.cpp](../OloEngine/tests/Rendering/ScreenSpaceGIMathTest.cpp) | 14 | **ScreenSpaceGI** &mdash; `SSGIUBOAlignment`, `SSGIUBOGetSizeMatchesSizeof`, `SSGIUBOLayoutSizeMatchesShader`, `SSGIBindingIsUniqueAndExpected`, `ViewPositionProjectRoundTrip`, `OctahedralNormalRoundTrip`, `BuildBasisIsOrthonormal`, `HemisphereSamplesAreUnitAndInHemisphere`, `CosineWeightedExpectedCosineIsTwoThirds`, `MarchStepsBoundedByDistanceAndStepCap`, `IndirectDiffuseIsAlbedoTimesMeanRadiance`, `CompositeIsAdditive`, `EdgeFadeVanishesAtBorders`, `SanitizeClampsNonFiniteAndRanges` |
| [ScreenSpaceReflectionMathTest.cpp](../OloEngine/tests/Rendering/ScreenSpaceReflectionMathTest.cpp) | 11 | **ScreenSpaceReflection** &mdash; `SSRUBOAlignment`, `SSRUBOGetSizeMatchesSizeof`, `SSRUBOLayoutSizeMatchesShader`, `SSRBindingIsUniqueAndExpected`, `ViewPositionProjectRoundTrip`, `OctahedralNormalRoundTrip`, `ReflectionVectorIsMirroredAcrossNormal`, `FresnelSchlickEndpointsAndMonotonicity`, `RoughnessFadeCutoff`, `EdgeFadeVanishesAtBorders`, `SanitizeClampsNonFiniteAndRanges` |
| [ShaderBindingLayoutTest.cpp](../OloEngine/tests/Rendering/ShaderBindingLayoutTest.cpp) | 25 | **ShaderBindingLayout** &mdash; `CameraUBOAlignment`, `MaterialUBOAlignment`, `PBRMaterialUBOAlignment`, `ModelUBOAlignment`, `AnimationUBOAlignment`, `MultiLightUBOAlignment`, `ShadowUBOAlignment`, `TerrainUBOAlignment`, `BrushPreviewUBOAlignment`, `FoliageUBOAlignment`, `DecalUBOAlignment`, `IBLParametersUBOAlignment`, `TerrainUBOSizeStable`, `BrushPreviewUBOSizeStable`, `FoliageUBOSizeStable`, `DecalUBOSizeStable`, `UBOBindingSlotUniqueness`, `TextureSlotUniqueness`, `SSBOSlotUniqueness`, `AnimationConstantsConsistency`, `ShaderConstantGeneratorRoundTrip`, `KnownUBOBindingRecognized`, `UnknownUBOBindingRejected`, `AllTextureSlotsWithinGLMinimum`, `UBOGetSizeMatchesSizeof` |
| [ShaderCrossConsistencyTest.cpp](../OloEngine/tests/Rendering/ShaderCrossConsistencyTest.cpp) | 2 | **ShaderCrossConsistency** &mdash; `BlockNamesHaveUniqueBindings`, `SamplerBindingsHaveConsistentType` |
| [ShaderPackTest.cpp](../OloEngine/tests/Rendering/ShaderPackTest.cpp) | 11 | **ShaderPackTest** &mdash; `LoadValid`, `ContainsQuery`, `GetShaderNames`, `LoadEntryValid`, `LoadEntryNotFound`, `LoadNonexistentFile`, `LoadInvalidMagic`, `LoadWrongVersion`, `LoadTruncatedFile`, `DefaultConstructor`, `EmptyPack` |
| [ShaderReflectionBindingTest.cpp](../OloEngine/tests/Rendering/ShaderReflectionBindingTest.cpp) | 1 | **ShaderReflectionBinding** &mdash; `AllProductionShaderBindingsMatchCppLayout` |
| [ShaderStageContractTest.cpp](../OloEngine/tests/Rendering/ShaderStageContractTest.cpp) | 2 | **ShaderStageContract** &mdash; `EveryFragmentShaderDeclaresAtLeastOneOutput`, `EveryVertexShaderWritesGlPosition` |
| [ShaderStageInterfaceTest.cpp](../OloEngine/tests/Rendering/ShaderStageInterfaceTest.cpp) | 1 | **ShaderStageInterface** &mdash; `VertexOutputsMatchFragmentInputs` |
| [ShaderUBOSizeConsistencyTest.cpp](../OloEngine/tests/Rendering/ShaderUBOSizeConsistencyTest.cpp) | 3 | **ShaderUBOSizeConsistency** &mdash; `GlslBlockSizeNeverExceedsCppStruct`, `CrossStageUBOLayoutAgreesWithinShader`, `CrossShaderUBOMemberOffsetsAgree` |
| [SphereAreaLightMathTest.cpp](../OloEngine/tests/Rendering/SphereAreaLightMathTest.cpp) | 9 | **SphereAreaLightMath** &mdash; `RepresentativePointReducesToCentreWhenRadiusZero`, `RepresentativePointLiesOnOrInsideSphereSurface`, `RepresentativePointStaysOnTheReflectionSideWhenRayMissesSphere`, `ReflectionRayHitsSphereCentre_RepresentativePointEqualsCentre`, `NormalisationIsExactlyOneAtZeroRadius`, `NormalisationBoundedInUnitInterval`, `NormalisationShrinksAsRadiusGrows`, `NormalisationGrowsTowardOneAsDistanceIncreases`, `NormalisationMatchesKarisAnalyticAtKnownSample` |
| [StarNestSkyMathTest.cpp](../OloEngine/tests/Rendering/StarNestSkyMathTest.cpp) | 10 | **StarNestSkyMath** &mdash; `ComputeUBOPacksFieldsInOrder`, `NonFiniteOffsetFallsBackToDefault`, `StepSizeAndTileStayStrictlyPositive`, `LoopCountsClampToShaderCeilings`, `SaturationAndFadesStayInUnitRange`, `HashChangesWhenParametersChange`, `EvaluateIsFiniteAndNonNegativeEverywhere`, `EvaluateIsDeterministic`, `HigherIntensityIsBrighter`, `ZeroBrightnessKeepsTheBaseFieldFinite` |
| [ShaderGraphCommandTest.cpp](../OloEngine/tests/ShaderGraph/ShaderGraphCommandTest.cpp) | 11 | **ShaderGraphCommandTest** &mdash; `AddNodeExecuteAddsNode`, `AddNodeUndoRemovesNode`, `AddNodeRedoRestoresNode`, `RemoveNodeUndoRestoresNodeAndLinks`, `AddLinkUndoRemovesLink`, `RemoveLinkUndoRestoresLink`, `MoveNodeUndoRestoresPosition`, `HistoryCanUndoCanRedo`, `NewCommandClearsRedoStack`, `HistoryClearResetsStacks`, `MultipleUndoRedoCycles` |
| [ShaderGraphCompilerTest.cpp](../OloEngine/tests/ShaderGraph/ShaderGraphCompilerTest.cpp) | 13 | **ShaderGraphCompilerTest** &mdash; `EmptyGraphFailsCompilation`, `MinimalGraphCompiles`, `OutputContainsVertexAndFragmentSections`, `OutputContainsMRTLayout`, `FloatParameterExposed`, `UBOBlockGenerated`, `MathNodeGeneratesCode`, `TextureSamplerBindingGenerated`, `MinimalComputeGraphCompiles`, `ComputeGraphWithBuffersCompiles`, `ComputeGraphCustomWorkgroupSize`, `ComputeGraphIsNotPBR`, `PBRGraphIsNotCompute` |
| [ShaderGraphSerializationTest.cpp](../OloEngine/tests/ShaderGraph/ShaderGraphSerializationTest.cpp) | 12 | **ShaderGraphSerializationTest** &mdash; `SerializeProducesNonEmptyYAML`, `RoundTripPreservesGraphName`, `RoundTripPreservesNodeCount`, `RoundTripPreservesLinkCount`, `RoundTripPreservesParameterName`, `RoundTripPreservesNodeTypes`, `DeserializeInvalidYAMLReturnsFalse`, `DeserializeMissingRootNodeReturnsFalse`, `DeserializedGraphIsDirty`, `DeserializedGraphCanCompile`, `RoundTripPreservesComputeWorkgroupSize`, `RoundTripPreservesBufferBinding` |
| [ShaderGraphTest.cpp](../OloEngine/tests/ShaderGraph/ShaderGraphTest.cpp) | 24 | **ShaderGraphTest** &mdash; `AddNodeAndFindIt`, `RemoveNodeCleansUpLinks`, `FindPinAcrossNodes`, `AddLinkConnectsCompatiblePins`, `AddLinkRejectsIncompatibleTypes`, `InputPinCanOnlyHaveOneLink`, `WouldCreateCycleDetectsCycle`, `WouldCreateCycleAllowsValidLink`, `ValidateEmptyGraphIsInvalid`, `ValidateGraphWithOutputNodeIsValid`, `ValidateGraphWithMultipleOutputsIsInvalid`, `TopologicalOrderPutsOutputLast`, `NodeFactoryCreatesAllRegisteredTypes`, `NodeFactoryReturnsNullForUnknown`, `ComputeOutputNodeCreation`, `ComputeBufferNodeCreation`, `ComputeInvocationIDNodes`, `ValidateComputeGraphIsValid`, `ValidateMixedOutputNodesIsInvalid`, `FindOutputNodeReturnsComputeOutput`<br/>**ShaderGraphTypeTest** &mdash; `FloatBroadcastsToVectors`, `Vec4TruncatesToVec3AndVec2`, `IncompatibleTypesCannotConvert`, `GenerateTypeConversionProducesValidGLSL` |

#### integration — Feature-level integration tests

| File | Tests | Cases |
|---|---:|---|
| [LightCullingTest.cpp](../OloEngine/tests/LightCullingTest.cpp) | 15 | **ForwardPlus** &mdash; `GPUPointLightSize`, `GPUSpotLightSize`, `GPUSphereAreaLightSize`, `ForwardPlusUBOSize`, `DefaultGridConfig`, `TileCountCalculation`, `TileCountCalculationNonMultiple`, `TileCountWith32pxTiles`, `SSBOBindingsDontConflict`, `UBOBindingIsUnique`, `ModeEnumValues`, `PointLightPacking`, `SpotLightPacking`, `SphereAreaLightPacking`, `LightIndexEncodingLayout` |
| [MorphTargetTest.cpp](../OloEngine/tests/MorphTargetTest.cpp) | 28 | **MorphTargetTest** &mdash; `DefaultConstruction`, `NamedConstruction`, `ConvertToSparse`, `ConvertToDense`<br/>**MorphTargetSetTest** &mdash; `AddAndFindTarget`, `GetVertexCount`<br/>**MorphTargetEvaluatorTest** &mdash; `ZeroWeightsReturnBase`, `SingleTargetFullWeight`, `HalfWeight`, `MultipleTargetsAdditive`, `SparseEvaluation`<br/>**MorphTargetComponentTest** &mdash; `SetAndGetWeight`, `WeightClamping`, `ResetAllWeights`, `HasActiveWeights`, `GetOrderedWeights`<br/>**FacialExpressionLibraryTest** &mdash; `RegisterAndApply`, `ApplyWithBlend`, `BlendBetweenExpressions`<br/>**MorphTargetKeyframeTest** &mdash; `AnimationClipStoresMorphKeyframes`, `GetMorphTracksLazyBuildsSortsAndInvalidates`<br/>**MorphTargetSystemTest** &mdash; `SampleMorphKeyframesAtExactKeys`, `SampleMorphKeyframesInterpolated`, `SampleMorphKeyframesMultipleTargets`, `EmptyClipDoesNotCrash`, `EvaluateMorphTargetsReturnsFalseWithNoActiveWeights`, `EvaluateMorphTargetsAppliesWeights`<br/>**MorphTargetGPUvsCPUTest** &mdash; `CPUReferenceMatchesExpected` |
| [PostProcessTest.cpp](../OloEngine/tests/PostProcessTest.cpp) | 15 | **PostProcessSettings** &mdash; `DefaultsAreReasonable`, `BloomParameterRanges`, `DOFParameterRanges`, `MotionBlurParameterRanges`<br/>**PostProcessUBOData** &mdash; `SizeIs80Bytes`, `DefaultsMatchSettings`, `FieldOffsets_Std140Compatible`<br/>**MotionBlurUBOData** &mdash; `SizeIs128Bytes`, `DefaultsAreIdentityMatrices`<br/>**ShadowUBO** &mdash; `SizeConsistency`, `FieldLayout`<br/>**TonemapOperator** &mdash; `ValuesMatchGLSLDefines`<br/>**ShaderBindingLayout** &mdash; `PostProcessUBOSlot`, `PostProcessTextureSlots`, `MotionBlurUBOSlot` |
| [DeccerCubesLoadingTest.cpp](../OloEngine/tests/Rendering/DeccerCubesLoadingTest.cpp) | 1 | **DeccerCubesLoaderFixture** &mdash; `LoadsWithMeshesAndFiniteBounds`<br/>*Parametrised via 1 `INSTANTIATE_TEST_SUITE_P`* |
| [OceanFFTGpuContractTest.cpp](../OloEngine/tests/Rendering/PropertyTests/OceanFFTGpuContractTest.cpp) | 4 | **OceanFFTGpuContractTest** &mdash; `InverseFFTOfDCImpulseIsConstant`, `InverseFFT2DMatchesCpuReference`, `EvaluatedFieldMatchesCpuReference`, `FieldGpuModeMatchesCpuModeTextures` |
| [RendererAttachedSmokeTest.cpp](../OloEngine/tests/Rendering/PropertyTests/RendererAttachedSmokeTest.cpp) | 4 | **EmptyScene** &mdash; `RendererInitAndTickDoNotCrash`<br/>**SceneWithCamera** &mdash; `TickDoesNotCrash`<br/>**SceneRenders3D** &mdash; `FullPipelineTickDoesNotCrash`, `RenderLeavesNoGlobalGlStateBehind` |
| [RenderingRegressionTest.cpp](../OloEngine/tests/Rendering/RenderingRegressionTest.cpp) | 5 | **RenderingRegression** &mdash; `ShadowUBOIntFieldsDefaultZeroed`, `WaterWavelengthMustBePositive`, `WaterWavelengthCopied`, `CascadeDebugDefaultDisabled`, `PBRMaterialUBOHasIBLField` |
| [SelectionOutlineTest.cpp](../OloEngine/tests/Rendering/SelectionOutlineTest.cpp) | 28 | **SelectionOutlineUBO** &mdash; `SizeIs304Bytes`, `SizeIs16ByteAligned`, `MaxSelectedEntitiesIs64`, `FieldOffsets_Std140Compatible`, `DefaultOutlineColorIsOrange`, `DefaultSelectedCountIsZero`, `DefaultOutlineWidthIsOne`, `SelectedIDsDefaultToNegativeOneSentinel`, `BindingSlotIs27`, `PackSingleEntityID`, `Pack64EntityIDs`<br/>**SelectionOutlineGraph** &mdash; `PassInsertedBetweenPostProcessAndUIComposite`, `TopologicalOrderRespectsChain`<br/>**JumpFloodUBO** &mdash; `SizeIs48Bytes`, `SizeIs16ByteAligned`, `BindingSlotIs29`, `FieldOffsets_Std140Compatible`, `DefaultOutlineColorIsOrange`, `DefaultThicknessValues`, `DefaultStepIsOne`<br/>**JFAStepSequence** &mdash; `PassCount1ProducesSingleStep`, `PassCount2ProducesCorrectSteps`, `PassCount3ProducesCorrectSteps`, `PassCount4ProducesCorrectSteps`, `PassCount0ClampsTo1`, `PassCount5ClampsTo4`, `NegativePassCountClampsTo1`, `AllStepsArePowersOfTwo` |
| [WaterRenderingTest.cpp](../OloEngine/tests/Rendering/WaterRenderingTest.cpp) | 62 | **WaterRendering** &mdash; `WaterUBOAlignment`, `WaterUBOSizeStable`, `WaterUBOGetSizeMatchesSizeof`, `WaterUBOBindingSlot`, `WaterUBOKnownBinding`, `WaterUBOFieldRoundTrip`, `DrawWaterCommandTrivialCopy`, `DrawWaterCommandSizeBound`, `DrawWaterCommandZeroInitNoNaN`, `WaterComponentDefaults`, `WaterComponentCopyOmitsRuntime`, `WaterComponentAssignmentOmitsRuntime`, `DrawWaterCommandTypeExists`, `WavelengthDefaultsNonZero`, `WavelengthPackedIntoWaveDir`, `WaterTextureBindingSlots`, `MaxWaveDisplacementBoundsActualGerstner`, `PatchInFrontOfCameraNotCulled`, `PatchBehindCameraCulled`, `PatchOffToSideCulled`, `PatchCrossingPlaneNotCulled`, `MarginExpandsAcceptanceRegion`, `AllSixPlanesParticipateInCull`, `TessParamsCarryFrustumCullFlag`, `UnderwaterFogUBOAlignment`, `UnderwaterFogUBOFieldOffsets`, `UnderwaterFogUBOBindingSlot`, `UnderwaterFogTransmittanceAtCameraIsOne`, `UnderwaterFogBeerLambertFalloff`, `UnderwaterFogSaturatesAtDistance`, `UnderwaterFogClampsNegativeInputs`, `UnderwaterFogStateDefaultsInactive`, `RefractionOffsetDisabledWhenStrengthZeroOrNegative`, `RefractionOffsetBoundedByStrength`, `RefractionOffsetHardCapsRunawayStrength`, `RefractionOffsetIsDeterministicAndAnimates`, `RefractionOffsetSurvivesNonFiniteInput`, `CausticPatternInUnitRange`, `CausticPatternVariesInSpaceAndTime`, `CausticPatternSurvivesNonFiniteInput`, `CausticDepthFadeZeroAtAndAboveSurface`, `CausticDepthFadeZeroBeyondMaxDepth`, `CausticDepthFadeMonotonicBetween`, `GodRayDecaySumZeroForDegenerateInput`, `GodRayDecaySumMatchesGeometricSeries`, `GodRayDecaySumGrowsWithSamples`, `GodRayDecaySumSurvivesNonFiniteInput`, `GodRayDappleInUnitRange`, `GodRayDappleVariesInSpaceAndTime`, `GodRayDappleSurvivesNonFiniteInput`, `GodRaySunScreenUVCentersWhenLookingAtSun`, `GodRaySunScreenUVRejectsSunBehindCamera`, `GodRaySunScreenUVRejectsDegenerateSunDir`, `UnderwaterSegment_CameraBelowFragmentBelow_FullRay`, `UnderwaterSegment_BothAbove_Zero`, `UnderwaterSegment_CameraAboveFragmentBelow_EntryToFrag`, `UnderwaterSegment_CameraBelowFragmentAbove_CamToExit`, `UnderwaterSegment_AtWaterlineSplitsRayByDirection`, `UnderwaterSegment_RespectsNonZeroSurfacePlane`, `WaterFace_CameraAbove_KeepsTopsDiscardsUndersides`, `WaterFace_CameraBelow_KeepsUndersidesDiscardsTops`, `WaterFace_AtWaterline_SplitsPerFragment` |
| [ShadowMapTest.cpp](../OloEngine/tests/ShadowMapTest.cpp) | 33 | **CascadeSplitTest** &mdash; `MonotonicallyIncreasing`, `CoversNearFarRange`, `LambdaZeroIsUniform`, `LambdaOneIsLogarithmic`, `ClampedMaxShadowDistance`, `AllSplitsPositive`, `DifferentLambdasProduceDifferentDistributions`<br/>**ShadowMapMatrixTest** &mdash; `ComputeCSMCascadesProducesValidMatrices`, `CascadePlaneDistancesMonotonicallyIncrease`, `CascadeFarPlaneCappedByMaxShadowDistance`, `CSMMatricesProjectKnownPointToValidNDC`, `SpotLightShadowProducesValidPerspectiveMatrix`, `SpotLightIndexOutOfRangeIsIgnored`, `PointLightProduces6FaceMatrices`, `PointLightShadowParamsStorePositionAndRange`, `PointLightIndexOutOfRangeIsIgnored`, `PointLightFaceMatricesProject90DegreeFOV`, `BeginFrameResetsPerFrameState`, `CascadeDebugToggle`<br/>**ShadowSettingsTest** &mdash; `DefaultValues`, `SetSettingsUpdatesValues`, `EnableDisableToggle`<br/>**ShadowUBOTest** &mdash; `StructSizeIsNonZero`, `StructSizeMultipleOf16`, `MaxConstants`, `DefaultInitializationZeroed`<br/>**Texture2DArrayTest** &mdash; `IsRefCounted`, `IsAbstract`<br/>**Texture2DArraySpecificationTest** &mdash; `DefaultValues`, `FormatEnumValues`<br/>**ShaderBindingLayoutTest** &mdash; `ShadowUBOBinding`, `ShadowTextureBindings`, `ShadowBindingsDoNotConflict` |
| [SlugFontRenderingTest.cpp](../OloEngine/tests/SlugFontRenderingTest.cpp) | 16 | **SlugDataTest** &mdash; `GlyphLookupReturnsNullForMissing`, `GlyphLookupFindsInserted`, `GetAdvanceUsesKerning`, `GetAdvanceWithoutKerning`<br/>**SlugFontProcessorTest** &mdash; `GlyphBoundsAreValid`, `ExtractCurvesProducesNonEmptyForVisibleGlyphs`, `SpaceGlyphHasNoCurves`<br/>**SlugGlyphRenderDataTest** &mdash; `DefaultValuesAreZero`<br/>**SlugCurveTest** &mdash; `ControlPointStorage`<br/>**FontMeasureLineTest** &mdash; `EmptyStringReturnsZero`, `AsciiMeasurementIsPositive`, `Utf8CharCountsAsOneGlyph`, `CarriageReturnIsSkipped`, `TabExpandsToFourSpaces`, `MissingGlyphFallsBackToQuestion`, `MultipleCodepointsAccumulate` |
| [SphericalHarmonicsTest.cpp](../OloEngine/tests/SphericalHarmonicsTest.cpp) | 23 | **SphericalHarmonicsTest** &mdash; `ZeroClearsAllCoefficients`, `AccumulateAddsCoefficients`, `ScaleMultipliesAllCoefficients`, `GPULayoutRoundtripPreservesData`, `GPULayoutValidityFlag`, `GPULayoutUnusedWComponentsAreZero`, `BasisFunctionDCTermIsConstant`, `BasisFunctionLinearTermsMatchDirection`, `BasisFunctionOppositeDirections`, `ConstantLightProducesConstantIrradiance`, `IrradianceIsNonNegative`, `DirectionalLightHigherInLitDirection`, `SizeConstants`<br/>**LightProbeVolumeComponentTest** &mdash; `TotalProbeCount`, `TotalProbeCountSingle`, `GridIndexLinearization`, `WorldToGridCorners`<br/>**SHProjectionTest** &mdash; `ConstantCubemapProjectsToDCTermOnly`, `SkyCubemapProducesPositiveZenithIrradiance`, `ProjectionScalesLinearlyWithInputIntensity`, `RejectsEmptyOrUndersizedInput`, `ChannelAsymmetryIsPreservedThroughProjectionAndScaling`, `UniformWhiteAfterCosineLobeScalingYieldsUnity` |
| [SceneStreamingTest.cpp](../OloEngine/tests/Streaming/SceneStreamingTest.cpp) | 18 | **StreamingSettings** &mdash; `DefaultValues`, `HysteresisGuarantee`<br/>**StreamingVolumeComponent** &mdash; `DefaultValues`, `ManualMode`<br/>**StreamingRegion** &mdash; `DefaultState`, `StateTransitions`, `BoundsStorage`, `LRUFrameTracking`, `EntityUUIDs`<br/>**SceneStreamerConfig** &mdash; `DefaultValues`, `CustomValues`<br/>**StreamingRegionSerializer** &mdash; `ParseInvalidYAML`, `MetadataRoundTrip`<br/>**RegionMetadata** &mdash; `DefaultValues`<br/>**SceneStreamer** &mdash; `DefaultConstruction`, `InitWithoutScene`, `ConfigAccessors`<br/>**StreamingActivationMode** &mdash; `EnumValues` |

#### meta — Test-framework self-tests

| File | Tests | Cases |
|---|---:|---|
| [TestFailureCaptureTest.cpp](../OloEngine/tests/Rendering/PropertyTests/TestFailureCaptureTest.cpp) | 6 | **TestFailureCaptureTest** &mdash; `DirectoryForSanitizesNames`, `MetadataIsWrittenWithoutGlContext`, `LatestFrameSummaryIsNoOpWithoutCaptures`, `GLStateSnapshotIsWrittenWhenGpuAvailable`, `FboPngIsWrittenWhenGpuAvailable`, `CaptureAllProducesExpectedArtefacts` |

**Totals.** 97 renderer-scope test files, 1320 TEST / TEST_F / TEST_P declarations across all layers.

<!-- END: renderer-catalogue -->

### 9.2 Functional tests

<!-- BEGIN: functional-catalogue (generated by OloEngine/tests/scripts/generate_test_catalogue.py) -->

> **Do not edit by hand.** Generated from [test_catalogue.json](../OloEngine/tests/scripts/test_catalogue.json) by [generate_test_catalogue.py](../OloEngine/tests/scripts/generate_test_catalogue.py). Add new test files to the config and run the script (or pre-commit will run it with `--check`).

#### AI (5 files)

| File | Tests | Cases |
|---|---:|---|
| [BehaviorTreeAdvancesViaSceneTickTest.cpp](../OloEngine/tests/Functional/AI/BehaviorTreeAdvancesViaSceneTickTest.cpp) | 1 | **BehaviorTreeAdvancesViaSceneTickTest** &mdash; `RootTaskRunsAndWritesBlackboardAfterOneTick` |
| [BehaviorTreeSequenceShortCircuitsOnFailureTest.cpp](../OloEngine/tests/Functional/AI/BehaviorTreeSequenceShortCircuitsOnFailureTest.cpp) | 1 | **BehaviorTreeSequenceShortCircuitsOnFailureTest** &mdash; `FirstChildRunsThirdChildSkippedAfterMiddleFails` |
| [GoapAgentPlansViaSceneTickTest.cpp](../OloEngine/tests/Functional/AI/GoapAgentPlansViaSceneTickTest.cpp) | 1 | **GoapAgentPlansViaSceneTickTest** &mdash; `AgentPlansAndExecutesToGoalAcrossSceneTicks` |
| [GoapAuthoredFromLuaViaSceneTickTest.cpp](../OloEngine/tests/Functional/AI/GoapAuthoredFromLuaViaSceneTickTest.cpp) | 2 | **GoapAuthoredFromLuaViaSceneTickTest** &mdash; `LuaBuiltAgentPlansAndReachesGoal`, `RuntimeStopReleasesLuaBuiltAgentBeforeStateTeardown` |
| [StateMachineTransitionsViaSceneTickTest.cpp](../OloEngine/tests/Functional/AI/StateMachineTransitionsViaSceneTickTest.cpp) | 1 | **StateMachineTransitionsViaSceneTickTest** &mdash; `FsmStartsInIdleAndTransitionsToChaseAfterConditionMet` |

#### AnimationPhysics (26 files)

| File | Tests | Cases |
|---|---:|---|
| [AnimationBlendCompletesViaSceneTickTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/AnimationBlendCompletesViaSceneTickTest.cpp) | 1 | **AnimationBlendCompletesViaSceneTickTest** &mdash; `BlendCompletesAfterDurationAndCurrentClipSwapsToNext` |
| [AnimationContinuesAfterPauseResumeTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/AnimationContinuesAfterPauseResumeTest.cpp) | 1 | **AnimationContinuesAfterPauseResumeTest** &mdash; `ClipTimeResumesFromPauseSnapshot` |
| [AnimationGraphMultipleEntitiesAdvanceIndependentlyTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/AnimationGraphMultipleEntitiesAdvanceIndependentlyTest.cpp) | 1 | **AnimationGraphMultipleEntitiesAdvanceIndependentlyTest** &mdash; `EachEntityKeepsItsOwnStateMachineState` |
| [AnimationGraphStateMachineTickTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/AnimationGraphStateMachineTickTest.cpp) | 1 | **AnimationGraphStateMachineTickTest** &mdash; `RuntimeGraphIsStartedAndCurrentStateMatchesDefaultAfterTick` |
| [AnimationKeepsTickingUnderPhysicsLoadTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/AnimationKeepsTickingUnderPhysicsLoadTest.cpp) | 1 | **AnimationKeepsTickingUnderPhysicsLoadTest** &mdash; `AllBodiesStableAndAnimationAdvanced` |
| [CharacterControllerContactCallbackTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/CharacterControllerContactCallbackTest.cpp) | 1 | **CharacterControllerContactCallbackTest** &mdash; `CallbackFiresWhenControllerTouchesStaticWall` |
| [CharacterControllerJumpsTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/CharacterControllerJumpsTest.cpp) | 1 | **CharacterControllerJumpsTest** &mdash; `JumpRisesThenFallsBackUnderGravity` |
| [CharacterControllerWalksTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/CharacterControllerWalksTest.cpp) | 1 | **CharacterControllerWalksTest** &mdash; `SetLinearVelocityMovesEntityAlongGround` |
| [CharacterPushesDynamicBodyTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/CharacterPushesDynamicBodyTest.cpp) | 1 | **CharacterPushesDynamicBodyTest** &mdash; `WalkingIntoDynamicBodyMovesIt` |
| [ComponentRemovedAtRuntimeTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/ComponentRemovedAtRuntimeTest.cpp) | 1 | **ComponentRemovedAtRuntimeTest** &mdash; `RemovingRigidbodyReleasesJoltBodyAndStopsTransformUpdates` |
| [EntityAddedAtRuntimeTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/EntityAddedAtRuntimeTest.cpp) | 1 | **EntityAddedAtRuntimeTest** &mdash; `BodyAddedAfterPhysicsStartActuallySimulates` |
| [EntityDestroyedMidTickTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/EntityDestroyedMidTickTest.cpp) | 1 | **EntityDestroyedMidTickTest** &mdash; `DestroyingOneBodyDoesNotBreakOthersOrAnimation` |
| [FreeFallLandingTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/FreeFallLandingTest.cpp) | 1 | **FreeFallLandingTest** &mdash; `BallLandsAndAnimationKeepsTicking` |
| [InitialLinearVelocityIsAppliedTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/InitialLinearVelocityIsAppliedTest.cpp) | 1 | **InitialLinearVelocityIsAppliedTest** &mdash; `BodyTravelsFiveMetersInOneSecondAtFiveMetersPerSecond` |
| [MorphTargetGraphAnimationTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/MorphTargetGraphAnimationTest.cpp) | 3 | **MorphTargetGraphAnimationTest** &mdash; `GraphTickSamplesMorphWeightFromActiveClip`, `MorphWeightTracksClipTimeAndLoops`, `SampledWeightDrivesCpuVertexDeformation` |
| [NoiseAnimatorViaSceneTickTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/NoiseAnimatorViaSceneTickTest.cpp) | 3 | **NoiseAnimatorViaSceneTickTest** &mdash; `ChainWobblesAndStateComponentIsCreated`, `DisabledComponentIsPassthroughAndCreatesNoState`, `RemovingComponentDropsRuntimeState` |
| [Physics2DSimulatesViaSceneTickTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/Physics2DSimulatesViaSceneTickTest.cpp) | 1 | **Physics2DSimulatesViaSceneTickTest** &mdash; `BoxFallsAndLandsOnStaticFloor` |
| [PhysicsJoint3DTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/PhysicsJoint3DTest.cpp) | 32 | **PhysicsJoint3DTest** &mdash; `FixedJointWeldsDynamicBodyToStaticAnchor`, `PointJointActsAsBallSocketPendulum`, `DistanceJointCatchesFallingBodyAtMaxLength`, `HingeJointSwingsAboutAxisAndRespectsAngleLimit`, `SliderJointConstrainsMotionToAxisAndLimit`, `ConeJointConfinesSwingWithinHalfAngle`, `BreakableJointSurvivesLoadBelowBreakForce`, `BreakableJointBreaksWhenForceExceedsBreakForce`, `BreakableJointBreaksWhenTorqueExceedsBreakTorque`, `UnbreakableByDefaultIgnoresLoad`, `HingeVelocityMotorDrivesRotationAgainstGravity`, `HingePositionMotorSettlesAtTargetAngle`, `SliderVelocityMotorDrivesBodyAlongAxis`, `SliderPositionMotorSettlesAtTargetPosition`, `HingeFrictionResistsSwingWithoutMotor`, `SliderFrictionResistsSlideWithoutMotor`, `SliderSoftLimitSpringSagsPastHardStop`, `HingeSoftLimitSpringSagsPastHardStop`, `SwingTwistConfinesSwingWithinCone`, `SwingTwistLimitsTwistAboutAxis`, `SwingTwistNormalisesInvertedTwistRange`, `SixDOFAllLockedHoldsBodyLikeWeld`, `SixDOFFreeTranslationAxisAllowsMotion`, `SixDOFLimitedTranslationStopsAtBound`, `PulleyLiftsLighterBodyAsHeavierBodyDescends`, `GearCouplesRotationByRatio`, `RackAndPinionCouplesRackSlideToPinionSpin`, `PathJointHoldsBodyOnHorizontalRail`, `PathVelocityMotorDrivesBodyAlongRail`, `PathPositionMotorSettlesAtTargetFraction`, `PathWithTooFewPointsSkipsConstraint`, `ComponentSurvivesSaveGameRoundTrip` |
| [PhysicsJointCollideConnectedTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/PhysicsJointCollideConnectedTest.cpp) | 5 | **PhysicsJointCollideConnectedTest** &mdash; `CollideConnectedFalseLetsBodyPassThroughConnectedBody`, `CollideConnectedTrueKeepsBodiesColliding`, `NoCollideDisablingIsPairwiseNotTransitive`, `CollideConnectedSurvivesSceneYAMLRoundTrip`, `CollideConnectedSurvivesSaveGameRoundTrip` |
| [PhysicsLayerFilteringTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/PhysicsLayerFilteringTest.cpp) | 1 | **PhysicsLayerFilteringTest** &mdash; `AlphaPassesThroughBetaAndBothLandOnGround` |
| [RigidbodyDisableGravityFlagTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/RigidbodyDisableGravityFlagTest.cpp) | 1 | **RigidbodyDisableGravityFlagTest** &mdash; `BodyWithDisableGravityStaysAtRestWhileControlFalls` |
| [SpringBoneJiggleViaSceneTickTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/SpringBoneJiggleViaSceneTickTest.cpp) | 3 | **SpringBoneJiggleViaSceneTickTest** &mdash; `ChainLagsAnimatedRootAndStateComponentIsCreated`, `DisabledComponentIsPassthroughAndCreatesNoState`, `RemovingComponentDropsRuntimeState` |
| [TriggerEndEventFiresOnSeparationTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/TriggerEndEventFiresOnSeparationTest.cpp) | 1 | **TriggerEndEventFiresOnSeparationTest** &mdash; `EnterAndExitBothInvokeTheCallback` |
| [TriggerVolumeFiresOverlapEventTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/TriggerVolumeFiresOverlapEventTest.cpp) | 1 | **TriggerVolumeFiresOverlapEventTest** &mdash; `OverlapEventFiresWhenCharacterEntersTrigger` |
| [TwoCharacterControllersCoexistTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/TwoCharacterControllersCoexistTest.cpp) | 1 | **TwoCharacterControllersCoexistTest** &mdash; `EachControllerIntegratesItsOwnVelocityIndependently` |
| [WaterBuoyancyTest.cpp](../OloEngine/tests/Functional/AnimationPhysics/WaterBuoyancyTest.cpp) | 5 | **WaterBuoyancyTest** &mdash; `LightBodySettlesAtWaterline`, `FloatHeightTracksTheWaterPlane`, `DenseBodySinks`, `WithoutWaterTheBodyFreeFalls`, `RestsAtTheWaveSurfaceHeight` |

#### Asset (1 file)

| File | Tests | Cases |
|---|---:|---|
| [AssetManagerImportsStagedAssetTest.cpp](../OloEngine/tests/Functional/Asset/AssetManagerImportsStagedAssetTest.cpp) | 1 | **AssetManagerImportsStagedAssetTest** &mdash; `RegistryRecognizesStagedTextureAndImportIsIdempotent` |

#### Audio (1 file)

| File | Tests | Cases |
|---|---:|---|
| [AudioListenerPositionUpdatesViaSceneTickTest.cpp](../OloEngine/tests/Functional/Audio/AudioListenerPositionUpdatesViaSceneTickTest.cpp) | 1 | **AudioRuntimeTicksViaSceneTest** &mdash; `ListenerRefIsAllocatedAndTickingRunsCleanly` |

#### Cinematic (2 files)

| File | Tests | Cases |
|---|---:|---|
| [CinematicAssetPlaybackTest.cpp](../OloEngine/tests/Functional/Cinematic/CinematicAssetPlaybackTest.cpp) | 1 | **CinematicAssetPlaybackTest** &mdash; `ImportedSequenceDrivesEntitiesByHandle` |
| [CinematicDrivesEntitiesTest.cpp](../OloEngine/tests/Functional/Cinematic/CinematicDrivesEntitiesTest.cpp) | 3 | **CinematicDrivesEntitiesTest** &mdash; `PosesEntitiesFiresEventAndFinishes`, `MidwayTranslationIsInterpolated`<br/>**CinematicLoopTest** &mdash; `LoopingSequenceNeverFinishesAndRefiresZeroEvent` |

#### Diagnostics (1 file)

| File | Tests | Cases |
|---|---:|---|
| [EmptyTickReproTest.cpp](../OloEngine/tests/Functional/Diagnostics/EmptyTickReproTest.cpp) | 3 | **EmptyTickReproTest** &mdash; `EmptySceneSingleTickReturns`<br/>**CameraOnlyTickTest** &mdash; `CameraOnlySceneTickReturns`<br/>**SpriteOnlyTickTest** &mdash; `SpriteOnlySceneTickReturns` |

#### Dialogue (4 files)

| File | Tests | Cases |
|---|---:|---|
| [DialogueAcceptsQuestAndGatesOnStateTest.cpp](../OloEngine/tests/Functional/Dialogue/DialogueAcceptsQuestAndGatesOnStateTest.cpp) | 3 | **DialogueAcceptsQuestAndGatesOnStateTest** &mdash; `AcceptActionMovesQuestToActiveAndPublishesStarted`, `BareAcceptActionUsesQuestGiverOfferedQuest`, `ConditionNodeBranchesOnQuestActiveState` |
| [DialogueAdvanceMovesToNextNodeTest.cpp](../OloEngine/tests/Functional/Dialogue/DialogueAdvanceMovesToNextNodeTest.cpp) | 1 | **DialogueAdvanceMovesToNextNodeTest** &mdash; `AdvanceTraversesGraphEdgeAndEndsAfterTerminalNode` |
| [DialogueSelectChoiceBranchesToTargetTest.cpp](../OloEngine/tests/Functional/Dialogue/DialogueSelectChoiceBranchesToTargetTest.cpp) | 1 | **DialogueSelectChoiceBranchesToTargetTest** &mdash; `SelectChoiceWalksTheChosenEdgeAndLandsOnTargetNode` |
| [DialogueTextRevealsOverTicksTest.cpp](../OloEngine/tests/Functional/Dialogue/DialogueTextRevealsOverTicksTest.cpp) | 1 | **DialogueTextRevealsOverTicksTest** &mdash; `StartDialogueDisplaysAndRevealProgressesAcrossTicks` |

#### Gameplay (19 files)

| File | Tests | Cases |
|---|---:|---|
| [AbilityActivationEffectAppliesToCasterTest.cpp](../OloEngine/tests/Functional/Gameplay/AbilityActivationEffectAppliesToCasterTest.cpp) | 1 | **AbilityActivationEffectAppliesToCasterTest** &mdash; `InstantSelfDamageActivationEffectDeductsHealthOnNextTick` |
| [AbilityBlockedByOwnerTagTest.cpp](../OloEngine/tests/Functional/Gameplay/AbilityBlockedByOwnerTagTest.cpp) | 1 | **AbilityBlockedByOwnerTagTest** &mdash; `OwnerHoldingBlockedTagPreventsActivation` |
| [AbilityCooldownTicksDownViaSceneTickTest.cpp](../OloEngine/tests/Functional/Gameplay/AbilityCooldownTicksDownViaSceneTickTest.cpp) | 1 | **AbilityCooldownTicksDownViaSceneTickTest** &mdash; `CooldownStartsOnActivationAndDecrementsViaSceneTick` |
| [AbilityRequiredTagGatesActivationTest.cpp](../OloEngine/tests/Functional/Gameplay/AbilityRequiredTagGatesActivationTest.cpp) | 1 | **AbilityRequiredTagGatesActivationTest** &mdash; `RequiredTagAbsenceBlocksActivationPresenceAllowsIt` |
| [AbilityResourceCostDeductedFromManaTest.cpp](../OloEngine/tests/Functional/Gameplay/AbilityResourceCostDeductedFromManaTest.cpp) | 1 | **AbilityResourceCostDeductedFromManaTest** &mdash; `ActivationDeductsCostAndRefusesWhenInsufficient` |
| [ChanneledAbilityAutoDeactivatesAtEndOfDurationTest.cpp](../OloEngine/tests/Functional/Gameplay/ChanneledAbilityAutoDeactivatesAtEndOfDurationTest.cpp) | 1 | **ChanneledAbilityAutoDeactivatesAtEndOfDurationTest** &mdash; `ChannelRemainingHitsZeroAndGrantedTagIsRemoved` |
| [DamageKillsEntityAndFlipsDeathTagTest.cpp](../OloEngine/tests/Functional/Gameplay/DamageKillsEntityAndFlipsDeathTagTest.cpp) | 1 | **DamageKillsEntityAndFlipsDeathTagTest** &mdash; `OverkillDamageDropsHealthAndScheduleNextTickFlipsTags` |
| [GameplayEffectExpiresAndRevertsAttributeTest.cpp](../OloEngine/tests/Functional/Gameplay/GameplayEffectExpiresAndRevertsAttributeTest.cpp) | 1 | **GameplayEffectExpiresAndRevertsAttributeTest** &mdash; `DurationEffectModifiesCurrentValueThenRevertsOnExpiry` |
| [InventoryAutoPickupOnProximityTest.cpp](../OloEngine/tests/Functional/Gameplay/InventoryAutoPickupOnProximityTest.cpp) | 1 | **InventoryAutoPickupOnProximityTest** &mdash; `NearbyAutoPickupIsConsumedIntoInventory` |
| [InventoryEventsEmittedTest.cpp](../OloEngine/tests/Functional/Gameplay/InventoryEventsEmittedTest.cpp) | 4 | **InventoryEventsEmittedTest** &mdash; `AddItemPublishesItemAdded`, `RemoveItemByDefinitionPublishesItemRemoved`, `EquipAndUnequipPublishEvents`, `AutoPickupPublishesItemAddedViaTick` |
| [InventoryStackConsolidationTest.cpp](../OloEngine/tests/Functional/Gameplay/InventoryStackConsolidationTest.cpp) | 1 | **InventoryStackConsolidationTest** &mdash; `TwoAddsOfStackableItemMergeIntoOneSlotWithStackCountTwo` |
| [InventoryTransferItemBetweenContainersTest.cpp](../OloEngine/tests/Functional/Gameplay/InventoryTransferItemBetweenContainersTest.cpp) | 1 | **InventoryTransferItemBetweenContainersTest** &mdash; `TransferFromPlayerToChestMovesItemAndClearsSource` |
| [MultiStageQuestAdvancesThroughStagesTest.cpp](../OloEngine/tests/Functional/Gameplay/MultiStageQuestAdvancesThroughStagesTest.cpp) | 1 | **MultiStageQuestAdvancesThroughStagesTest** &mdash; `FinishingStageZeroSwitchesToStageOneObjectives` |
| [PeriodicDamageEffectTicksHealthDownTest.cpp](../OloEngine/tests/Functional/Gameplay/PeriodicDamageEffectTicksHealthDownTest.cpp) | 1 | **PeriodicDamageEffectTicksHealthDownTest** &mdash; `HealthDropsByOneStackPerPeriodAcrossTicks` |
| [PeriodicEffectStackingScalesDamageTest.cpp](../OloEngine/tests/Functional/Gameplay/PeriodicEffectStackingScalesDamageTest.cpp) | 1 | **PeriodicEffectStackingScalesDamageTest** &mdash; `ThreeStacksDeductFifteenPerPeriodNotFive` |
| [QuestEventsEmittedTest.cpp](../OloEngine/tests/Functional/Gameplay/QuestEventsEmittedTest.cpp) | 8 | **QuestEventsEmittedTest** &mdash; `AcceptPublishesQuestStarted`, `ObjectiveProgressCompletionAndAutoCompleteCascade`, `MultiStageAdvancePublishesStageAdvanced`, `AbandonPublishesQuestAbandoned`, `ExplicitFailPublishesQuestFailed`, `TimedQuestDeadlinePublishesQuestFailedViaTick`, `NotifyKillDrivesObjectiveEventsAcrossActiveQuests`, `CompleteWithBranchPublishesBranchChoice` |
| [QuestObjectiveCompletionAdvancesStageTest.cpp](../OloEngine/tests/Functional/Gameplay/QuestObjectiveCompletionAdvancesStageTest.cpp) | 1 | **QuestObjectiveCompletionAdvancesStageTest** &mdash; `IncrementToTargetMarksCompleteAndAutoFinalizesQuest` |
| [TimedQuestFailsAfterDeadlineTest.cpp](../OloEngine/tests/Functional/Gameplay/TimedQuestFailsAfterDeadlineTest.cpp) | 1 | **TimedQuestFailsAfterDeadlineTest** &mdash; `ActiveQuestTransitionsToFailedAfterTimeLimitElapses` |
| [ToggledAbilityReactivateCancelsTest.cpp](../OloEngine/tests/Functional/Gameplay/ToggledAbilityReactivateCancelsTest.cpp) | 1 | **ToggledAbilityReactivateCancelsTest** &mdash; `SuccessiveActivationsToggleActiveStateAndGrantedTag` |

#### Navigation (2 files)

| File | Tests | Cases |
|---|---:|---|
| [NavAgentReachesTargetTest.cpp](../OloEngine/tests/Functional/Navigation/NavAgentReachesTargetTest.cpp) | 1 | **NavAgentReachesTargetTest** &mdash; `AgentMovesAcrossFloorAndArrivesNearTarget` |
| [NavMeshQueryFindPathBetweenPointsTest.cpp](../OloEngine/tests/Functional/Navigation/NavMeshQueryFindPathBetweenPointsTest.cpp) | 1 | **NavMeshQueryFindPathBetweenPointsTest** &mdash; `DirectFindPathReturnsPolylineConnectingStartAndEnd` |

#### Networking (2 files)

| File | Tests | Cases |
|---|---:|---|
| [PhysicsTransformReplicationTest.cpp](../OloEngine/tests/Functional/Networking/PhysicsTransformReplicationTest.cpp) | 1 | **PhysicsTransformReplicationTest** &mdash; `NetArchiveRoundTripPreservesPostPhysicsTransform` |
| [SnapshotApplyAcrossScenesTest.cpp](../OloEngine/tests/Functional/Networking/SnapshotApplyAcrossScenesTest.cpp) | 1 | **SnapshotApplyAcrossScenesTest** &mdash; `ServerSnapshotMirrorsToClientSceneAndScenesStayIndependent` |

#### Particles (1 file)

| File | Tests | Cases |
|---|---:|---|
| [ParticleSystemEmitsAndExpiresTest.cpp](../OloEngine/tests/Functional/Particles/ParticleSystemEmitsAndExpiresTest.cpp) | 1 | **ParticleSystemEmitsAndExpiresTest** &mdash; `BurstEmitsThenAllParticlesExpire` |

#### SaveGame (8 files)

| File | Tests | Cases |
|---|---:|---|
| [AbilityComponentRoundTripTest.cpp](../OloEngine/tests/Functional/SaveGame/AbilityComponentRoundTripTest.cpp) | 1 | **AbilityComponentRoundTripTest** &mdash; `AttributesAbilitiesAndTagsSurviveSceneYAMLRoundTrip` |
| [HierarchyPreservedAcrossSaveLoadTest.cpp](../OloEngine/tests/Functional/SaveGame/HierarchyPreservedAcrossSaveLoadTest.cpp) | 1 | **HierarchyPreservedAcrossSaveLoadTest** &mdash; `ChildRetainsParentReferenceAfterRoundTrip` |
| [InventoryComponentSceneYAMLRoundTripTest.cpp](../OloEngine/tests/Functional/SaveGame/InventoryComponentSceneYAMLRoundTripTest.cpp) | 1 | **InventoryComponentSceneYAMLRoundTripTest** &mdash; `ItemsAndCurrencySurviveSceneYAMLRoundTrip` |
| [NetworkIdentitySurvivesSaveLoadTest.cpp](../OloEngine/tests/Functional/SaveGame/NetworkIdentitySurvivesSaveLoadTest.cpp) | 1 | **NetworkIdentitySurvivesSaveLoadTest** &mdash; `AuthorityOwnerAndReplicatedFlagRoundTripCorrectly` |
| [QuestJournalSceneYAMLRoundTripTest.cpp](../OloEngine/tests/Functional/SaveGame/QuestJournalSceneYAMLRoundTripTest.cpp) | 1 | **QuestJournalSceneYAMLRoundTripTest** &mdash; `ActiveAndCompletedQuestsAndPlayerStateSurviveSceneYAMLRoundTrip` |
| [RegisteredComponentsSurviveSaveLoadTest.cpp](../OloEngine/tests/Functional/SaveGame/RegisteredComponentsSurviveSaveLoadTest.cpp) | 1 | **RegisteredComponentsSurviveSaveLoadTest** &mdash; `PreviouslyDroppedComponentsRoundTripThroughSaveGame` |
| [Save2DBodyVelocityRoundTripTest.cpp](../OloEngine/tests/Functional/SaveGame/Save2DBodyVelocityRoundTripTest.cpp) | 1 | **Save2DBodyVelocityRoundTripTest** &mdash; `SavedRuntimeVelocityIsAppliedOnReload` |
| [SceneRoundTripAfterTickTest.cpp](../OloEngine/tests/Functional/SaveGame/SceneRoundTripAfterTickTest.cpp) | 1 | **SceneRoundTripAfterTickTest** &mdash; `CapturedAfterTickRestoresIdenticalState` |

#### Scene (11 files)

| File | Tests | Cases |
|---|---:|---|
| [DeterministicReplayProducesSameStateTest.cpp](../OloEngine/tests/Functional/Scene/DeterministicReplayProducesSameStateTest.cpp) | 1 | **DeterministicReplayProducesSameStateTest** &mdash; `TwoIdenticalRunsProduceMatchingTransforms` |
| [DuplicateTagFindableTest.cpp](../OloEngine/tests/Functional/Scene/DuplicateTagFindableTest.cpp) | 1 | **DuplicateTagFindableTest** &mdash; `LookupAlwaysReturnsALiveEntityUntilTheLastDuplicateIsGone` |
| [EntityCreateWithUUIDTest.cpp](../OloEngine/tests/Functional/Scene/EntityCreateWithUUIDTest.cpp) | 2 | **EntityCreateWithUUIDTest** &mdash; `CreateEntityWithUUIDIsLookupableByThatUUID`, `ManyEntitiesHaveUniqueUUIDs` |
| [EntityNameMapStaysConsistentTest.cpp](../OloEngine/tests/Functional/Scene/EntityNameMapStaysConsistentTest.cpp) | 1 | **EntityNameMapStaysConsistentTest** &mdash; `CreateRenameDestroyKeepsNameLookupCorrect` |
| [EntitySetParentCycleRejectionTest.cpp](../OloEngine/tests/Functional/Scene/EntitySetParentCycleRejectionTest.cpp) | 1 | **EntitySetParentCycleRejectionTest** &mdash; `ParentingCycleIsRejectedAndHierarchyStaysIntact` |
| [HierarchyChildFollowsPhysicsParentTest.cpp](../OloEngine/tests/Functional/Scene/HierarchyChildFollowsPhysicsParentTest.cpp) | 1 | **HierarchyChildOnDestroyedParentTest** &mdash; `ChildDoesNotDanglePointingAtDeadParent` |
| [MultipleScenesCoexistTest.cpp](../OloEngine/tests/Functional/Scene/MultipleScenesCoexistTest.cpp) | 1 | **MultipleScenesCoexistTest** &mdash; `BothScenesTickAndLandIndependently` |
| [PhysicsRestartIsCleanTest.cpp](../OloEngine/tests/Functional/Scene/PhysicsRestartIsCleanTest.cpp) | 1 | **PhysicsRestartIsCleanTest** &mdash; `SecondPhysicsCycleSimulatesIdenticallyToFirst` |
| [PrimaryCameraResolutionTest.cpp](../OloEngine/tests/Functional/Scene/PrimaryCameraResolutionTest.cpp) | 1 | **PrimaryCameraResolutionTest** &mdash; `ReturnsTheCameraWithPrimaryTrueAndTracksHandoff` |
| [ScenePauseFreezesAllSubsystemsTest.cpp](../OloEngine/tests/Functional/Scene/ScenePauseFreezesAllSubsystemsTest.cpp) | 1 | **ScenePauseFreezesAllSubsystemsTest** &mdash; `PauseStopsAllTickingSubsystemsResumeRestartsThem` |
| [SceneStepAdvancesOneFrameTest.cpp](../OloEngine/tests/Functional/Scene/SceneStepAdvancesOneFrameTest.cpp) | 1 | **SceneStepAdvancesOneFrameTest** &mdash; `StepAdvancesExactlyTheRequestedFrameCountThenRePauses` |

#### Scripting (8 files)

| File | Tests | Cases |
|---|---:|---|
| [LuaCompletesQuestViaIncrementObjectiveTest.cpp](../OloEngine/tests/Functional/Scripting/LuaCompletesQuestViaIncrementObjectiveTest.cpp) | 1 | **LuaCompletesQuestViaIncrementObjectiveTest** &mdash; `LuaIncrementObjectiveByThreeAutoCompletesSingleStageQuest` |
| [LuaDrivesTerrainRegenerationTest.cpp](../OloEngine/tests/Functional/Scripting/LuaDrivesTerrainRegenerationTest.cpp) | 1 | **LuaDrivesTerrainRegenerationTest** &mdash; `LuaSetsSeedAndRegeneratesChangingTheHeightField` |
| [LuaRaycastHitsPhysicsBodyTest.cpp](../OloEngine/tests/Functional/Scripting/LuaRaycastHitsPhysicsBodyTest.cpp) | 1 | **LuaRaycastHitsPhysicsBodyTest** &mdash; `ScriptCastingRayAlongXObservesNearFaceOfStaticTarget` |
| [LuaReadsTransformOfAnotherEntityTest.cpp](../OloEngine/tests/Functional/Scripting/LuaReadsTransformOfAnotherEntityTest.cpp) | 1 | **LuaReadsTransformOfAnotherEntityTest** &mdash; `TrackerScriptCopiesTargetXIntoOwnTranslation` |
| [LuaScriptMutatesTransformViaSceneTickTest.cpp](../OloEngine/tests/Functional/Scripting/LuaScriptMutatesTransformViaSceneTickTest.cpp) | 1 | **LuaScriptMutatesTransformViaSceneTickTest** &mdash; `OnUpdateRunsEachTickAndWritesTranslation` |
| [LuaScriptOnDestroyFiresTest.cpp](../OloEngine/tests/Functional/Scripting/LuaScriptOnDestroyFiresTest.cpp) | 1 | **LuaScriptOnDestroyFiresTest** &mdash; `OnDestroyCallbackFiresWhenScriptEntityIsDestroyed` |
| [LuaScriptSetsRigidbody2DVelocityTest.cpp](../OloEngine/tests/Functional/Scripting/LuaScriptSetsRigidbody2DVelocityTest.cpp) | 1 | **LuaScriptSetsRigidbody2DVelocityTest** &mdash; `BodyTranslatesAfterLuaSetsLinearVelocity` |
| [LuaSetsAbilityAttributeViaSceneTickTest.cpp](../OloEngine/tests/Functional/Scripting/LuaSetsAbilityAttributeViaSceneTickTest.cpp) | 1 | **LuaSetsAbilityAttributeViaSceneTickTest** &mdash; `LuaDrivenHealthToZeroFlipsAliveToDeadOnSubsequentTick` |

**Totals.** 91 Functional test files, 154 TEST / TEST_F declarations across all subsystems.

<!-- END: functional-catalogue -->

### 9.3 Unit / subsystem tests

Tests that belong to neither the renderer pyramid nor the Functional axis —
plain per-subsystem unit tests (Async, Audio, Networking, Serialization, the
container/template/task libraries, and the cross-subsystem grab-bag at the top
level of `tests/`). Grouped by directory.

<!-- BEGIN: unit-catalogue (generated by OloEngine/tests/scripts/generate_test_catalogue.py) -->

> **Do not edit by hand.** Generated from [test_catalogue.json](../OloEngine/tests/scripts/test_catalogue.json) by [generate_test_catalogue.py](../OloEngine/tests/scripts/generate_test_catalogue.py). Add new test files to the config and run the script (or pre-commit will run it with `--check`).

#### (top-level) (65 files)

| File | Tests | Cases |
|---|---:|---|
| [AnimationStateMachineTest.cpp](../OloEngine/tests/AnimationStateMachineTest.cpp) | 10 | **AnimationStateMachineTest** &mdash; `ParameterSetAndGet`, `TriggerConsumption`, `TransitionConditionFloat`, `TransitionConditionBool`, `TransitionConditionTrigger`, `TransitionANDLogic`, `BasicTransition`, `AnyStateTransition`, `ExitTimeTransition`, `CrossFadeBlending` |
| [AnimationSystemTest.cpp](../OloEngine/tests/AnimationSystemTest.cpp) | 4 | **AnimationSystem** &mdash; `NonAnimatedBonePreservesBindPoseTransform`, `BindPoseResetSkippedWhenNotInitialized`, `BlendingPreservesNonAnimatedBoneTransform`, `AnimatedBoneIsUpdatedFromKeyframes` |
| [AppLaunchSmokeTest.cpp](../OloEngine/tests/AppLaunchSmokeTest.cpp) | 3 | **AppLaunchSmoke** &mdash; `OloServerLaunchesCleanly`, `OloEditorLaunchesCleanly`, `OloRuntimeLaunchesCleanly` |
| [AssetBinaryIntegrityTest.cpp](../OloEngine/tests/AssetBinaryIntegrityTest.cpp) | 2 | **AssetBinaryIntegrity** &mdash; `AllPngFilesHaveValidPngMagic`, `AllWavFilesHaveValidRiffWavePrefix` |
| [AssetCSharpScriptValidityTest.cpp](../OloEngine/tests/AssetCSharpScriptValidityTest.cpp) | 4 | **AssetCSharpScriptValidity** &mdash; `EveryCSharpFileDeclaresClassMatchingFilename`, `EveryEntityDerivedScriptIsReferencedByAtLeastOneScene`, `EveryCSharpFileIsListedInCMakeSources`, `EveryNonEntityHelperClassIsReferencedByAnotherSource` |
| [AssetContentValidityTest.cpp](../OloEngine/tests/AssetContentValidityTest.cpp) | 24 | **AssetContentValidity** &mdash; `AllSandboxScenesAreStructurallyValid`, `AllSandboxItemsAreStructurallyValid`, `AllSandboxQuestsAreStructurallyValid`, `AllSandboxDialoguesAreStructurallyValid`, `AllSandboxScenesHaveUniqueEntityUUIDs`, `AllSandboxScenePrefabComponentsHaveRequiredUUIDs`, `AllSandboxScenesHaveUniquePrefabEntityIDsPerPrefab`, `AllSandboxShaderGraphsAreStructurallyValid`, `SandboxSceneReferencedScriptsExistOnDisk`, `SandboxAssetRegistryDeserialisesAndPathsResolve`, `EverySupportedAssetOnDiskIsInTheRegistry`, `SandboxAssetRegistryPathsMatchOnDiskCasing`, `AllSandboxSceneAssetHandlesAreInTheRegistry`, `ShaderCacheEntriesAllHaveLiveGlslSources`, `AllCacheFilesMatchKnownPattern`, `EveryProductionShaderIsReferencedFromSource`, `SandboxProjectFilePathsAreAllRelative`, `DISABLED_RebaseAssetRegistry`, `SceneScriptFieldNamesMatchCSharpDeclarations`, `SandboxEditorPreferencesAreStructurallyValid`, `AllSandboxScenePathReferencesResolve`, `AllSandboxScenePathsUseForwardSlashes`, `AllSandboxAssetYAMLPathsUseForwardSlashes`, `SandboxInputActionsAreStructurallyValid` |
| [AssetCreationTest.cpp](../OloEngine/tests/AssetCreationTest.cpp) | 3 | **AssetCreationTest** &mdash; `MeshColliderAsset_Creation`, `ScriptFileAsset_Creation`, `ColliderMaterial_Basic` |
| [AssetExtensionsCoverageTest.cpp](../OloEngine/tests/AssetExtensionsCoverageTest.cpp) | 2 | **AssetExtensionsCoverage** &mdash; `EveryAssetTypeHasAtLeastOneRegisteredExtension`, `ExtensionMapIsRoundTripSymmetric` |
| [AssetGenerationTest.cpp](../OloEngine/tests/AssetGenerationTest.cpp) | 8 | **AssetGenerationTest** &mdash; `UnknownHandleReturnsZero`, `IncrementFromZero`, `MultipleIncrements`, `IndependentHandles`, `ZeroHandleTracked`, `ConcurrentIncrementsSameHandleNoLostUpdates`, `ConcurrentIncrementsDistinctHandles`, `ConcurrentReadersAndWriter` |
| [AssetLoadedEventTest.cpp](../OloEngine/tests/AssetLoadedEventTest.cpp) | 6 | **AssetLoadedEventTest** &mdash; `ExposesPayload`, `IdentifiesItselfAsAssetLoaded`, `IsDistinctFromAssetReloaded`, `ToStringContainsPayload`, `DispatcherInvokesMatchingHandler`, `DispatcherSkipsMismatchedHandler` |
| [AssetLuaScriptValidityTest.cpp](../OloEngine/tests/AssetLuaScriptValidityTest.cpp) | 4 | **AssetLuaScriptValidity** &mdash; `AllSandboxLuaScriptsParseCleanly`, `EveryLuaScriptDeclaresAtLeastOneEngineHook`, `EveryLuaScriptIsReferencedByAtLeastOneScene`, `AllHookDeclarationsMatchEngineSignature` |
| [AssetPackTest.cpp](../OloEngine/tests/AssetPackTest.cpp) | 16 | **AssetPackFileTest** &mdash; `HeaderSizeIs24Bytes`, `DefaultHeaderHasCorrectMagicAndVersion`, `IndexOffsetMustBeAtLeastHeaderSize`<br/>**AssetPackTest** &mdash; `LoadFailsIfFileDoesNotExist`, `LoadFailsWithInvalidMagicNumber`, `LoadFailsWithZeroIndexOffset`, `LoadFailsWithZeroAssetCount`, `LoadSucceedsWithZeroSceneCount`, `LoadFailsWithWrongVersion`, `LoadFailsWithIndexOffsetBeyondFileSize`, `LoadSucceedsWithValidMinimalPack`, `UnloadMakesPackNotLoaded`, `IdempotentLoadReturnsTrueOnSecondCall`, `AssetInfoFieldOrderMatchesLoader`, `LoadSucceedsWithMultipleAssetsNoScenes`, `WrongFieldOrderProducesGarbledData` |
| [AssetRegistryInvariantsTest.cpp](../OloEngine/tests/AssetRegistryInvariantsTest.cpp) | 6 | **AssetRegistryInvariants** &mdash; `AddAssetMakesBothIndicesAgree`, `RemoveAssetClearsBothIndices`, `UpdateMetadataKeepsHandleSwapsPath`, `ClearEmptiesEverything`, `GenerateHandleProducesUniqueNonZeroHandles`, `GetAssetsOfTypeFiltersAcrossMutations` |
| [AssetSceneLoadTest.cpp](../OloEngine/tests/AssetSceneLoadTest.cpp) | 1 | **AssetSceneLoad** &mdash; `AllSandboxScenesDeserialiseThroughEditorAssetManager` |
| [AudioDSPTest.cpp](../OloEngine/tests/AudioDSPTest.cpp) | 24 | **AudioDSP_Denormals** &mdash; `FlushesSubnormalToZero`, `PreservesNormalFloat`, `PreservesZero`, `PreservesNegative`<br/>**AudioDSP_DelayLine** &mdash; `ConstructsWithValidSize`, `SetAndGetDelay`, `PushPopProducesDelayedSample`, `MultiChannelIndependence`, `ResetClearsBuffer`<br/>**AudioDSP_Allpass** &mdash; `SetFeedback`, `ProcessOutputsDifferentFromInput`, `AllpassPreservesEnergy`, `MuteClearsBuffer`<br/>**AudioDSP_Comb** &mdash; `SetDampAndFeedback`, `ProcessProducesOutput`, `CombDecays`, `MuteClearsBuffer`<br/>**AudioDSP_Reverb** &mdash; `ConstructsWithSampleRate`, `SettersAndGetters`, `ProcessReplaceSilenceProducesSilence`, `ProcessReplaceImpulseProducesReverbTail`, `MuteSilencesOutput`, `DifferentSampleRates`, `FreezeMode` |
| [AudioEventQueueTest.cpp](../OloEngine/tests/AudioEventQueueTest.cpp) | 11 | **AudioEventQueue** &mdash; `BasicPushPop`, `EmptyQueue`, `QueueFull`, `MessageQueue`, `DifferentValueTypes`, `LongMessageTruncation`, `MultithreadedProducerConsumer`, `MultithreadedStressTest`, `ClearQueue`, `WrapAround`, `PerformanceBenchmark` |
| [AudioSpatializerTest.cpp](../OloEngine/tests/AudioSpatializerTest.cpp) | 19 | **VBAP_RealtimeGains** &mdash; `DefaultIsZero`, `WriteAndRead`, `DoubleWriteReturnsLatest`<br/>**VBAP_VectorAngle** &mdash; `ForwardIsZero`, `RightIs90`, `LeftIsMinus90`, `Vec2Overload`<br/>**VBAPTest** &mdash; `InitSucceeds`, `VirtualSourceCountMatchesExpected`, `InverseMatricesCountMatchesSpeakers`, `ClearResetsData`, `UpdateProducesNonZeroGains`, `GainSumApproximatelyOne`<br/>**AudioTransform** &mdash; `DefaultValues`, `Equality`<br/>**SampleBufferOps** &mdash; `ContentMatchesIdentical`, `ContentMatchesDiffers`, `AddAndApplyGainRampConstant`, `AddAndApplyGainRampInterpolates` |
| [AutoSaveTest.cpp](../OloEngine/tests/AutoSaveTest.cpp) | 4 | **FileSystemIsNewerTest** &mdash; `ReturnsFalseWhenFileADoesNotExist`, `ReturnsFalseWhenFileBDoesNotExist`, `ReturnsFalseWhenBothDoNotExist`, `DetectsNewerFileByInjectedMTime` |
| [BehaviorTreeTest.cpp](../OloEngine/tests/BehaviorTreeTest.cpp) | 36 | **BTBlackboardTest** &mdash; `SetAndGetBool`, `SetAndGetInt`, `SetAndGetFloat`, `SetAndGetString`, `SetAndGetVec3`, `HasAndRemove`, `Clear`, `GetAllReturnsAllKeys`<br/>**BTSequenceTest** &mdash; `AllSucceed_ReturnsSuccess`, `OneFailure_ReturnsFailure`, `Running_PausesAndResumes`, `EmptyChildren_ReturnsSuccess`<br/>**BTSelectorTest** &mdash; `FirstSuccess_ReturnsSuccess`, `AllFail_ReturnsFailure`, `Running_PausesAtRunningChild`, `EmptyChildren_ReturnsFailure`<br/>**BTParallelTest** &mdash; `RequireAll_AllSucceed_ReturnsSuccess`, `RequireOne_OneSucceeds_ReturnsSuccess`, `RequireAll_OneRunning_ReturnsRunning`<br/>**BTDecoratorTest** &mdash; `Inverter_FlipsSuccess`, `Inverter_FlipsFailure`, `Inverter_PassesThroughRunning`, `Repeater_FixedCount`, `ConditionalGuard_AllowsWhenMatch`, `ConditionalGuard_BlocksWhenMismatch`, `ConditionalGuard_BlocksWhenKeyMissing`<br/>**BTTaskTest** &mdash; `Wait_ReturnsRunningThenSuccess`, `SetBlackboardValue_SetsKey`, `CheckBlackboardKey_SucceedsWhenMatch`, `CheckBlackboardKey_FailsWhenMismatch`<br/>**BehaviorTreeContainerTest** &mdash; `TickWithRoot`, `TickWithNoRoot_ReturnsFailure`, `Reset_ResetsRoot`<br/>**BehaviorTreeAssetTest** &mdash; `HasCorrectAssetType`, `StoresNodesAndRoot`<br/>**AIRegistryTest** &mdash; `RegisterBuiltInTypes` |
| [BlendTreeTest.cpp](../OloEngine/tests/BlendTreeTest.cpp) | 10 | **BlendTreeTest** &mdash; `Simple1D_AtFirstThreshold`, `Simple1D_AtMidThreshold`, `Simple1D_BetweenThresholds`, `Simple1D_AtLastThreshold`, `Simple1D_ClampBelowFirst`, `Simple1D_ClampAboveLast`, `SimpleDirectional2D_AtChild`, `EmptyBlendTree`, `SingleChild`, `GetDurationReturnsWeightedAverageDuration` |
| [ComponentRoundTripTest.cpp](../OloEngine/tests/ComponentRoundTripTest.cpp) | 63 | **ComponentRoundTrip** &mdash; `TransformComponentSurvivesYAMLRoundTrip`, `CameraComponentSurvivesYAMLRoundTrip`, `SpriteRendererComponentSurvivesYAMLRoundTrip`, `CircleRendererComponentSurvivesYAMLRoundTrip`, `DirectionalLightComponentSurvivesYAMLRoundTrip`, `PointLightComponentSurvivesYAMLRoundTrip`, `SpotLightComponentSurvivesYAMLRoundTrip`, `SphereAreaLightComponentSurvivesYAMLRoundTrip`, `TextComponentSurvivesYAMLRoundTrip`, `LocalizedTextComponentSurvivesYAMLRoundTrip`, `Rigidbody2DComponentSurvivesYAMLRoundTrip`, `Rigidbody3DComponentSurvivesYAMLRoundTrip`, `BoxCollider2DComponentSurvivesYAMLRoundTrip`, `CircleCollider2DComponentSurvivesYAMLRoundTrip`, `SphereCollider3DComponentSurvivesYAMLRoundTrip`, `AudioListenerComponentSurvivesYAMLRoundTrip`, `BoxCollider3DComponentSurvivesYAMLRoundTrip`, `MeshCollider3DComponentSurvivesYAMLRoundTrip`, `ConvexMeshCollider3DComponentSurvivesYAMLRoundTrip`, `TriangleMeshCollider3DComponentSurvivesYAMLRoundTrip`, `CapsuleCollider3DComponentSurvivesYAMLRoundTrip`, `CharacterController3DComponentSurvivesYAMLRoundTrip`, `PhysicsJoint3DComponentSurvivesYAMLRoundTrip`, `LightProbeComponentSurvivesYAMLRoundTrip`, `LightProbeVolumeComponentSurvivesYAMLRoundTrip`, `ReflectionProbeComponentSurvivesYAMLRoundTrip`, `EnvironmentMapComponentSurvivesYAMLRoundTrip`, `UIRectTransformComponentSurvivesYAMLRoundTrip`, `UIPanelComponentSurvivesYAMLRoundTrip`, `UITextComponentSurvivesYAMLRoundTrip`, `UIButtonComponentSurvivesYAMLRoundTrip`, `UISliderComponentSurvivesYAMLRoundTrip`, `UICheckboxComponentSurvivesYAMLRoundTrip`, `UIProgressBarComponentSurvivesYAMLRoundTrip`, `UIInputFieldComponentSurvivesYAMLRoundTrip`, `UIScrollViewComponentSurvivesYAMLRoundTrip`, `UICanvasComponentSurvivesYAMLRoundTrip`, `UIGridLayoutComponentSurvivesYAMLRoundTrip`, `UIDropdownComponentSurvivesYAMLRoundTrip`, `UIToggleComponentSurvivesYAMLRoundTrip`, `UIWorldAnchorComponentSurvivesYAMLRoundTrip`, `StreamingVolumeComponentSurvivesYAMLRoundTrip`, `BehaviorTreeComponentSurvivesYAMLRoundTrip`, `StateMachineComponentSurvivesYAMLRoundTrip`, `InventoryComponentSurvivesYAMLRoundTrip`, `ItemPickupComponentSurvivesYAMLRoundTrip`, `ItemContainerComponentSurvivesYAMLRoundTrip`, `QuestGiverComponentSurvivesYAMLRoundTrip`, `ParticleSystemComponentSurvivesYAMLRoundTrip`, `NetworkIdentityComponentSurvivesYAMLRoundTrip`, `NetworkInterestComponentSurvivesYAMLRoundTrip`, `AudioSourceComponentSurvivesYAMLRoundTrip`, `SerializeLoadSerializeProducesIdenticalYAML`, `SerializeLoadSerializeWithWideComponentSetIsIdempotent`, `EntityHierarchyParentAndChildrenSurviveYAMLRoundTrip`, `WaterComponentUnderwaterFieldsSurviveYAMLRoundTrip`, `BuoyancyComponentSurvivesYAMLRoundTrip`, `InstancedMeshComponentInstancesSurviveYAMLRoundTrip`, `InstancedMeshComponentNonFiniteInstanceDataIsSanitizedOnLoad`, `SpringBoneComponentSurvivesYAMLRoundTrip`, `SpringBoneComponentNonFiniteFieldsAreSanitizedOnLoad`, `NoiseAnimationComponentSurvivesYAMLRoundTrip`, `NoiseAnimationComponentNonFiniteFieldsAreSanitizedOnLoad` |
| [ComponentSerializerCoverageTest.cpp](../OloEngine/tests/ComponentSerializerCoverageTest.cpp) | 2 | **ComponentSerializerCoverage** &mdash; `EveryDeclaredComponentIsHandledBySceneSerializer`, `EverySerializerComponentReferenceHasMatchingStruct` |
| [ComponentTupleCoverageTest.cpp](../OloEngine/tests/ComponentTupleCoverageTest.cpp) | 2 | **ComponentTupleCoverage** &mdash; `EveryDeclaredComponentIsInAllComponentsTuple`, `EveryAllComponentsTupleEntryHasMatchingStruct` |
| [ContainerTest.cpp](../OloEngine/tests/ContainerTest.cpp) | 13 | **ContainerSmoke** &mdash; `TBitArrayBasicInitAndIndexing`, `TSparseArrayAddAndIterate`, `TSetAddDuplicateAndContains`, `TMapAddFindRemove`, `TMultiMapAddDuplicateKeys`<br/>**TArrayTest** &mdash; `GenericAppendFromTArrayView`, `GenericAppendFromConstTArrayView`, `GenericAppendFromCArray`, `GenericAppendEmptyView`, `AppendFromRawPointer`, `TIsTArrayOrDerivedFromTArray`, `TArrayElementsAreCompatible`, `TElementTypeWorks` |
| [CoreUtilitiesTest.cpp](../OloEngine/tests/CoreUtilitiesTest.cpp) | 3 | **CoreUtilitiesTest** &mdash; `HashSystemTest`, `IdentifierSystemTest`, `IsSpecializedTest` |
| [DialogueSystemTest.cpp](../OloEngine/tests/DialogueSystemTest.cpp) | 10 | **DialogueTreeAssetTest** &mdash; `FindNodeReturnsCorrectNode`, `FindNodeReturnsNullForMissing`, `GetConnectionsFromReturnsAll`, `GetConnectionsFromWithPort`, `GetConnectionsFromEmptyForLeafNode`, `RootNodeIDAccessor`, `AssetTypeIsDialogueTree`, `NodePropertyAccess`<br/>**DialogueComponentTest** &mdash; `CopyDoesNotCopyHasTriggered`, `AssignmentDoesNotCopyHasTriggered` |
| [DialogueTreeSerializationTest.cpp](../OloEngine/tests/DialogueTreeSerializationTest.cpp) | 6 | **DialogueTreeSerializationTest** &mdash; `SerializeAndDeserializeRoundTrip`, `DeserializeRejectsMissingRootNode`, `DeserializeRejectsDanglingConnection`, `DeserializeRejectsDuplicateNodeID`, `SingleNodeNoConnections`, `RoundTripPreservesAllPropertyTypes` |
| [DialogueVariablesTest.cpp](../OloEngine/tests/DialogueVariablesTest.cpp) | 3 | **DialogueVariables** &mdash; `RoundTripAcrossAllSupportedTypes`, `MissingKeysReturnTheCallerProvidedDefault`, `HasReportsPresenceAndClearWipesEverything` |
| [EngineSubsystemSmokeTest.cpp](../OloEngine/tests/EngineSubsystemSmokeTest.cpp) | 6 | **EngineSubsystemSmoke** &mdash; `ProjectLoadSandboxProjectSucceeds`, `ProjectLoadMissingPathFailsCleanly`, `ProjectLoadMalformedYAMLFailsCleanly`, `ProjectSaveActiveLoadRoundTrip`, `ProjectSaveActiveLoadExtendedFieldsRoundTrip`, `SandboxProjectStartSceneResolves` |
| [FastRandomTest.cpp](../OloEngine/tests/FastRandomTest.cpp) | 8 | **FastRandomTest** &mdash; `AllGeneratorsProduceVaryingValuesFromAFreshSeed`, `SameSeedProducesIdenticalSequenceAcrossAllGenerators`, `PCGSeed42ProducesPinnedOutputVector`, `EveryGetXInRangeRespectsTheRequestedBounds`, `EqualBoundsAlwaysReturnTheBound`, `SwappedBoundsStayWithinTheImpliedRange`, `Int32InRangeIsApproximatelyUniformOverTenBuckets`, `RandomUtilsDispatchesToGlobalRNG` |
| [FunctionalTestFixtureCoverageTest.cpp](../OloEngine/tests/FunctionalTestFixtureCoverageTest.cpp) | 1 | **FunctionalTestFixtureCoverage** &mdash; `EveryEnableHelperIsCalledBySomeFunctionalTest` |
| [GamepadTest.cpp](../OloEngine/tests/GamepadTest.cpp) | 20 | **GamepadDeadzoneTest** &mdash; `InsideDeadzoneReturnsZero`, `ZeroInputReturnsZero`, `FullDeflectionReturnsOne`, `ExactlyAtDeadzoneReturnsZero`, `JustOutsideDeadzoneReturnsSmallValue`, `DirectionIsPreserved`<br/>**GamepadCodesTest** &mdash; `ButtonToString`, `AxisToString`, `StringToButton`, `StringToAxis`<br/>**InputBindingTest** &mdash; `GamepadButtonFactory`, `GamepadAxisFactory`, `GamepadButtonDisplayName`, `GamepadAxisDisplayName`<br/>**GamepadActionTest** &mdash; `GamepadButtonActionPressed`, `GamepadButtonActionReleased`, `GamepadAxisPositiveThreshold`, `GamepadAxisNegativeThreshold`, `MixedKeyboardAndGamepadBindings`<br/>**GamepadSerializationTest** &mdash; `RoundTrip` |
| [GameplayEventBusTest.cpp](../OloEngine/tests/GameplayEventBusTest.cpp) | 5 | **GameplayEventBusTest** &mdash; `PublishReachesSubscriberWithPayload`, `EventsAreRoutedByType`, `MultipleHandlersFireInSubscriptionOrder`, `PublishWithNoSubscribersIsNoOp`, `ClearDropsAllSubscriptions` |
| [HeadlessTest.cpp](../OloEngine/tests/HeadlessTest.cpp) | 16 | **HeadlessMode** &mdash; `ApplicationSpecDefaultsToNonHeadless`, `ApplicationSpecHeadlessFlag`, `DefaultTickRate`, `CustomTickRate`, `TickRateFromServerConfig`<br/>**ServerConfig** &mdash; `DefaultValues`<br/>**ServerConfigSerializer** &mdash; `ParseEmptyCommandLine`, `ParsePortFlag`, `ParseMultipleFlags`, `YAMLRoundTrip`, `LoadMissingFileReturnsDefaults`, `CommandLineOverridesConfigFile`<br/>**ServerConsole** &mdash; `ImplementsIConsoleInterface`, `RegisterAndHasCommands`, `RegisterCustomCommand`, `MessageSendCallback` |
| [InputActionTest.cpp](../OloEngine/tests/InputActionTest.cpp) | 41 | **InputBindingTest** &mdash; `KeyFactory`, `MouseFactory`, `Equality`, `DisplayNameKeyboard`, `DisplayNameMouse`<br/>**InputActionMapTest** &mdash; `AddAndRetrieve`, `RemoveAction`, `HasAction`, `DuplicateAddOverwrites`, `EmptyMap`, `MultipleBindings`<br/>**InputActionManagerTest** &mdash; `UnknownActionReturnsFalse`, `SetActionMapClearsState`, `GetActionMapReturnsReference`<br/>**DefaultGameActionsTest** &mdash; `HasExpectedActions`, `MoveUpHasWAndArrowAndDPad`<br/>**InputActionSerializerTest** &mdash; `RoundTrip`, `EmptyMap`, `InvalidFile`, `MalformedYAML`, `MissingFields`, `UnknownBindingType`, `FuzzRegression_RootIsScalar`, `FuzzRegression_RootIsSequence`, `FuzzRegression_InputActionMapIsNull`, `FuzzRegression_InputActionMapIsScalar`, `FuzzRegression_ActionsContainsNonMap`, `FuzzRegression_ActionNameIsMap`, `FuzzRegression_BindingTypeIsSequence`, `FuzzRegression_CodeFieldIsNonScalar`, `FuzzRegression_AxisThresholdIsNaN`, `FuzzRegression_RawGarbage`<br/>**InputStateTransitionTest** &mdash; `KeyPressedOnFirstFrame`, `KeyHeldAcrossFrames`, `KeyReleasedAfterHeld`, `JustReleasedOnlyOneFrame`, `MouseButtonTransitions`, `MultipleBindingsSameAction`, `RapidPressReleaseCycle`, `UnboundActionStaysIdle`, `StaleStateCleanedAfterActionRemoval` |
| [InventoryTest.cpp](../OloEngine/tests/InventoryTest.cpp) | 37 | **InventoryTestFixture** &mdash; `ItemDatabase_GetRegisteredItem`, `ItemDatabase_GetUnregisteredItem`, `ItemDatabase_GetByCategory`, `ItemDatabase_GetByTag`, `ItemDatabase_GetAll`, `Inventory_AddItem`, `Inventory_CapacityLimit`, `Inventory_WeightLimit`, `Inventory_RemoveByDefinition`, `Inventory_RemoveAll`, `Inventory_RemoveTooMany`, `Inventory_HasItem`, `Inventory_FindItem`, `Inventory_SwapItems`, `Inventory_MoveToEmptySlot`, `Inventory_SortByCategory`, `Inventory_SortByRarity`, `Equipment_EquipUnequip`, `Equipment_AttributeModifiers`, `Equipment_AffixModifiers`, `Equipment_SlotStringConversion`, `LootTable_BasicRoll`, `LootTable_NothingWeight`, `LootTable_EmptyTable`, `LootTable_StatisticalDistribution`, `LootTable_ItemLevelFilter`, `AffixDatabase_RegisterAndGet`, `AffixDatabase_RegisterPoolAndGet`, `AffixDatabase_GetAll`, `AffixDatabase_ClearRemovesAll`, `LootTable_RollWithAffixPool`, `LootTable_AffixTierSelection`, `LootTable_AffixPrefixSuffixLimits`, `LootTable_LegacyAffixFallback`, `AffixTypeStringConversion`<br/>**ItemStringConversion** &mdash; `CategoryRoundTrip`, `RarityRoundTrip` |
| [JoltShapesMeshMassPropertiesTest.cpp](../OloEngine/tests/JoltShapesMeshMassPropertiesTest.cpp) | 11 | **JoltShapesMeshMassProperties** &mdash; `UnitCubeAtOriginHasUnitVolumeAndZeroCentroid`, `UnitCubeAtOffsetTracksCentroid`, `NonUniformScaleAppliesToVolumeAndCentroid`, `NegativeScaleProducesPositiveVolume`, `UVSphereApproachesAnalyticVolume`, `EmptyMeshReportsInvalid`, `TrianglePastIndexRangeIsSkipped`, `FlatQuadReportsInvalid`, `TrailingPartialTriangleIsIgnored`, `ReversedWindingProducesPositiveVolume`, `ExtremeScaleOverflowToInfReportsInvalid` |
| [LocalizationTest.cpp](../OloEngine/tests/LocalizationTest.cpp) | 53 | **StringTable** &mdash; `LoadFromYAMLPopulatesKeysAndMetadata`, `MalformedYAMLLeavesPriorContentsUntouched`, `OtherOnlyPluralRuleParses`<br/>**TextFormatter** &mdash; `NamedParameterSubstitution`, `MissingParameterLeavesTokenLiteral`, `DoubleBracesEmitLiteralBraces`, `PluralOneOtherSelectsByCount`, `PluralFrenchLikeCollapsesZeroAndOne`, `PluralPolishLikePicksThreeForms`, `PluralRussianLikePicksFourForms`, `PluralArabicLikePicksSixForms`, `PluralOtherOnlyAlwaysPicksFirstForm`, `NonIntegerCountFallsBackToTokenLiteral`, `UnclosedBraceLeavesRemainderLiteral`, `GenderTokenSelectsByStringValue`, `SelectTokenDispatchesByLabelWithElseFallback`<br/>**LocalizationFixture** &mdash; `GetReturnsFallbackBeforeAnyLocaleIsLoaded`, `LoadLocaleAndLookupKey`, `FormatUsesActiveLocaleTemplate`, `SetCurrentLocaleSwitchesActiveTableAndFiresEvent`, `SetCurrentLocaleFailsForUnknownCode`, `InitializeScansDirectory`, `ReloadCurrentLocaleRereadsFile`, `MissingKeyFallbackIsConfigurable`, `ConcurrentGetIsSafeAcrossLocaleSwitch`, `PerLocaleAccessorsBypassActiveLocale`, `GenerationIncrementsOnLocaleLoad`, `GenerationIncrementsOnLocaleSwitch`, `GenerationIncrementsOnReloadCurrentLocale`, `SystemUpdatesTextComponentOnLocaleSwitch`, `SystemSkipsEntitiesWithEmptyKey`, `SystemRefreshesNewlyAddedComponent`, `CsvRoundTripPreservesEveryKey`, `CsvQuotingHandlesCommasAndQuotesAndNewlines`, `CsvImportWarnsOnUnknownLocaleColumn`, `ResolveLocalizedTextDispatchesByPrefix`, `PseudoLocaleWrapsAndDecoratesValues`, `MissingKeyReportingAccumulatesAcrossLookups`, `FormatNumberUsesLocaleSeparators`, `NegotiateLocaleFallsBackThroughLanguageTag`, `ActiveLocalePersistsAcrossSaveLoad`, `LintCatchesMissingAndExtraParameters`, `SaveLocaleToFileRoundTripsWithEdits`, `ResolveLocalizedAssetPathPicksLocaleVariant`, `MaxLengthLintCatchesOverflowing`, `FormatCurrencyHonoursLocaleSymbolAndPlacement`, `FormatListJoinsWithLocaleJoiners`, `FormatDateUsesDefaultPatternsAndCustomOverrides`, `FormatRelativeTimePicksLargestUnit`<br/>**UTF8** &mdash; `DecodesAsciiAndMultiByteSequences`, `ReplacesInvalidSequencesWithReplacementCharacter`, `CountCodepointsHandlesMixedAndInvalid`<br/>**LocalizedTextComponent** &mdash; `EqualityCompares` |
| [MathTest.cpp](../OloEngine/tests/MathTest.cpp) | 11 | **MathBitwiseEqualTest** &mdash; `EqualScalarsCompareEqual`, `DifferentScalarsCompareUnequal`, `PositiveAndNegativeZeroAreDistinct`, `NaNIsBitwiseEqualToItself`, `DifferentNaNPayloadsAreUnequal`, `GlmVecComparison`, `GlmMat4Comparison`, `IntegerTypesAlsoWork`<br/>**MathIsFiniteTest** &mdash; `FiniteScalarsVectorsAndMatricesPass`, `NaNIsRejectedInEveryComponentSlot`, `PositiveAndNegativeInfinityAreRejected` |
| [MeshCookingFactoryCacheTest.cpp](../OloEngine/tests/MeshCookingFactoryCacheTest.cpp) | 5 | **MeshCookingFactoryCache** &mdash; `ReturnsTrueWhenCacheNewerThanSource`, `ReturnsFalseWhenSourceNewerThanCache`, `ReturnsFalseWhenCacheMissing`, `ReturnsFalseWhenSourceMissing`, `ReturnsTrueWhenTimestampsAreEqual` |
| [ModelImporterTest.cpp](../OloEngine/tests/ModelImporterTest.cpp) | 7 | **ModelImporterTest** &mdash; `AnimatedModelWiresAllComponents`, `StaticModelOnlyAddsMeshComponent`, `DeserializePreservesPlaybackScalars`, `OutOfRangeClipIndexClampsToZero`, `ShaderGraphMaterialIsNotClobbered`, `ReimportUpdatesDataWithoutReAddingComponents`, `NullAnimatedModelIsNoOp` |
| [NavMeshTest.cpp](../OloEngine/tests/NavMeshTest.cpp) | 31 | **NavMeshSettingsTest** &mdash; `DefaultValues`<br/>**NavMeshAssetTest** &mdash; `StaticType`, `DefaultConstructorIsInvalid`, `AssetTypeConsistency`, `SettingsRoundTrip`, `SerializeEmptyMeshFails`, `DeserializeGarbageDataFails`, `MoveConstructor`<br/>**NavMeshQueryTest** &mdash; `DefaultConstructorIsInvalid`, `FindPathWithoutNavMeshReturnsFalse`, `FindNearestPointWithoutNavMeshReturnsFalse`, `RaycastWithoutNavMeshReturnsError`, `IsPointOnNavMeshWithoutNavMeshReturnsFalse`<br/>**CrowdManagerTest** &mdash; `DefaultConstructorIsInvalid`, `AddAgentWithoutNavMeshReturnsNegative`<br/>**NavMeshBoundsComponentTest** &mdash; `DefaultValues`<br/>**NavAgentComponentTest** &mdash; `DefaultValues`, `CopyOnlySerializedFields`, `CopyAssignmentResetsRuntime`, `MoveConstructorResetsRuntime`<br/>**AssetTypeTest** &mdash; `NavMeshToString`, `NavMeshFromString`<br/>**NavMeshRoundTripTest** &mdash; `SerializeDeserializePreservesPathfinding`<br/>**NavMeshQueryPositiveTest** &mdash; `FindNearestPointOnPlane`, `RaycastUnobstructed`, `IsPointOnNavMeshCenter`, `IsPointOffNavMeshFarAway`, `CustomQueryBudget`<br/>**CrowdManagerPositiveTest** &mdash; `AddAgentWithValidNavMesh`, `GetAgentPositionAfterAdd`, `RemoveAgentDecrementsCount` |
| [PerformanceProfilerTest.cpp](../OloEngine/tests/PerformanceProfilerTest.cpp) | 9 | **PerformanceProfilerTest** &mdash; `InitiallyEmpty`, `SetPerFrameTimingAccumulates`, `EndFrameResetsCurrent`, `ClearBothMaps`, `MultipleFrames`, `ConcurrentWriters`<br/>**ScopedPerformanceTimerTest** &mdash; `RecordsTiming`, `NullProfilerSafe`<br/>**PerFrameDataTest** &mdash; `DefaultValues` |
| [PreCommitToolingSmokeTest.cpp](../OloEngine/tests/PreCommitToolingSmokeTest.cpp) | 1 | **PreCommitToolingSmoke** &mdash; `EveryToolingPythonScriptCompiles` |
| [PrecipitationEmitterTest.cpp](../OloEngine/tests/PrecipitationEmitterTest.cpp) | 14 | **PrecipitationEmitter** &mdash; `EmissionRateAtZeroIntensityIsZero`, `EmissionRateAtFullIntensityEqualsBaseRate`, `EmissionRateQuadraticAtHalf`, `EmissionRateNeverNegative`, `SpawnVolumeContainsCamera`, `FarFieldVolumeIsLargerThanNearField`, `WindBiasShiftsVolumeUpwind`, `GeneratesZeroParticlesAtZeroIntensity`, `GeneratesParticlesAtFullIntensity`, `ParticlesHaveFiniteValues`, `ParticlesHaveDownwardVelocity`, `ParticlesSpawnDespiteCameraMotion`, `GPUParticleSizeIs96Bytes`, `AllTypesGenerateValidParticles` |
| [PrecipitationSettingsTest.cpp](../OloEngine/tests/PrecipitationSettingsTest.cpp) | 8 | **PrecipitationSettings** &mdash; `TypeEnumValues`, `IntensityEnumValues`<br/>**PrecipitationUBOData** &mdash; `SizeIs80Bytes`, `FieldOffsetsMatchStd140`<br/>**PrecipitationBindings** &mdash; `UBOBindingIs18`, `TextureBindingIs31`, `UBOBindingIsKnown`, `TextureBindingIsKnown` |
| [PrecipitationSystemTest.cpp](../OloEngine/tests/PrecipitationSystemTest.cpp) | 3 | **PrecipitationStats** &mdash; `DefaultsAreZero`<br/>**PrecipitationSystem** &mdash; `IntensityClampLogic`<br/>**PrecipitationSettings** &mdash; `CopyPreservesValues` |
| [PrefabOverrideTest.cpp](../OloEngine/tests/PrefabOverrideTest.cpp) | 31 | **PrefabOverrideTest** &mdash; `PrefabComponent_DefaultHasNoOverrides`, `PrefabComponent_MarkAndQueryOverride`, `PrefabComponent_ClearSingleOverride`, `PrefabComponent_ClearAllOverrides`, `PrefabComponent_AddedAndRemoved`, `Prefab_CreateFromEntity`, `Prefab_InstantiateCreatesNewEntity`, `Prefab_CreatePreservesHierarchy`, `Prefab_InstantiatePreservesHierarchy`, `Prefab_DetectOverrides_Added`, `Prefab_DetectOverrides_Removed`, `Prefab_RevertComponent`, `Prefab_RevertComponent_UnknownReturnsFalse`, `Prefab_ApplyComponentToPrefab`, `Prefab_UpdateInstanceFromPrefab_NonOverridden`, `Prefab_CycleDetection_SelfReference`, `Prefab_HasNestedPrefabs_EmptyPrefab`, `Scene_MarkPrefabComponentOverridden`, `Scene_MarkPrefabOverridden_NoPrefabComponent`, `Prefab_UpdateInstance_SkipsRemovedComponents`, `Prefab_ApplyRemovedComponent_RemovesFromPrefab`, `Prefab_RevertRemovedComponent_RestoresFromPrefab`, `Prefab_RevertAddedComponent_RemovesFromInstance`, `Prefab_RevertComponent_ChildEntity`, `Prefab_UpdateInstance_ChildEntity`, `Prefab_DetectOverrides_PopulatesOverridden`, `Prefab_UpdateInstance_RemovesStaleComponent`, `PrefabComponent_ClearComponentOverride_ClearsAllSets`, `Scene_RevertPrefabComponent_EndToEnd`, `Scene_ApplyPrefabComponent_EndToEnd`, `Scene_UpdateAllPrefabInstances_MultipleInstances` |
| [QualityTieringTest.cpp](../OloEngine/tests/QualityTieringTest.cpp) | 5 | **QualityTiering** &mdash; `PresetsAreOrderedByQuality`, `ApplyWritesShadowSettings`, `ApplyWritesPostProcessSettings`, `StringRoundTrip`, `UnknownStringDefaultsToHigh` |
| [QuestSystemTest.cpp](../OloEngine/tests/QuestSystemTest.cpp) | 55 | **QuestSystemTest** &mdash; `QuestLifecycle_AcceptToComplete`, `QuestStatus_Unavailable`, `AcceptQuest_AlreadyActive`, `AbandonQuest`, `ObjectiveTracking_Increment`, `ObjectiveTracking_Overshoot`, `NotifyKill_UpdatesCorrectObjective`, `NotifyCollect_UpdatesCorrectObjective`, `TimedQuest_ExpiresOnTimeout`, `FailOnTag`, `BranchingCompletion`, `CompletionRewards_GrantsTags`, `NonRepeatable_CantReaccept`, `Repeatable_CanReaccept`, `OptionalObjectives_DontBlockStage`, `AnyObjective_CompletesStage`, `QuestDatabase_RegisterAndGet`, `QuestDatabase_GetByCategory`, `GetActiveAndCompletedQuests`, `SetObjectiveComplete`, `TagManagement`, `ObjectiveTypeStringConversion`, `QuestStatusStringConversion`, `AcceptQuest_RequiresPrerequisiteQuests`, `AcceptQuest_RequiresTags`, `AcceptQuest_MismatchedQuestID`, `CompleteQuest_NotReady`, `Requirement_Level`, `Requirement_Reputation`, `Requirement_ReputationLessThan`, `Requirement_HasItem`, `Requirement_Stat`, `Requirement_IsClass`, `Requirement_IsFaction`, `Requirement_QuestCompleted`, `Requirement_QuestActive`, `Requirement_QuestFailed`, `Requirement_QuestNotStarted`, `Requirement_DoesNotHaveTag`, `Requirement_HasTag`, `Requirement_All_Combinator`, `Requirement_Any_Combinator`, `Requirement_Not_Combinator`, `Requirement_NestedCombinators`, `Requirement_MultipleTopLevel`, `GetUnmetRequirements`, `CheckRequirement_Direct`, `ComparisonOperators`, `RequirementTypeStringConversion`, `ComparisonOpStringConversion`, `PlayerState_SettersGetters`, `Requirement_EmptyAll`, `Requirement_EmptyAny`, `Requirement_EmptyNot`, `RequirementsCombo_QuestAndTag` |
| [RuntimeAssetManagerTest.cpp](../OloEngine/tests/RuntimeAssetManagerTest.cpp) | 5 | **RuntimeAssetManagerTest** &mdash; `LoadedPackAssetsAreDiscoverable`, `UnknownHandleIsNotValid`, `GetAllAssetsWithTypeReturnsMatchingHandles`, `UnloadingPackRemovesItsAssets`, `MetadataSurvivesUntilExplicitUnload` |
| [RuntimeAssetPackTest.cpp](../OloEngine/tests/RuntimeAssetPackTest.cpp) | 4 | **RuntimeAssetPackTest** &mdash; `SceneSerializerPackRoundTrip`, `SceneRoutingUsesSceneInfoOffset`, `OffThreadCapabilityContract`, `AsyncLoadIntegratesThroughManager` |
| [SRGBTextureSupportTest.cpp](../OloEngine/tests/SRGBTextureSupportTest.cpp) | 8 | **SRGBTextureSupport** &mdash; `TextureSpecificationDefaultsToLinear`, `TextureSpecificationCarriesSrgbFlag`, `RawTextureDataKeepsLinearDefault`, `FilenameHeuristic_ClassifiesAlbedoAsSrgb`, `FilenameHeuristic_ClassifiesDataTexturesAsLinear`, `FilenameHeuristic_DataKeywordsOverrideColorKeywords`, `FilenameHeuristic_AmbiguousFilesDefaultLinear`, `FilenameHeuristic_IsCaseInsensitive` |
| [SceneMeshRaycastTest.cpp](../OloEngine/tests/SceneMeshRaycastTest.cpp) | 15 | **SceneMeshRaycastTest** &mdash; `HitsTranslatedCube`, `MissReturnsFalseAndResetsHit`, `RotatedEntityMapsNormalBack`, `NonUniformScaleNormalMatchesGeometricGroundTruth`, `ClosestOfTwoEntitiesWins`, `TMaxClampRejectsHitsBeyondIt`, `SubmeshComponentOnlyHitsItsOwnRange`, `InvisibleSubmeshComponentIsSkipped`, `SkeletonEntityIsSkipped`, `CacheReusesOneEntryPerMeshRange`, `CacheRebuildsWhenGeometryFingerprintChanges`, `CachePrunesDeadMeshSources`, `ClearCacheDropsEverything`, `NonUnitDirectionStillReportsWorldDistances`, `DegenerateRaysMiss` |
| [SceneSerializerFuzzRegressionTest.cpp](../OloEngine/tests/SceneSerializerFuzzRegressionTest.cpp) | 19 | **SceneSerializerFuzzRegression** &mdash; `CrashInput_OriginalSixBytes`, `EmptyInput`, `RootIsScalar`, `RootIsSequence`, `RootIsEmptyMap`, `SceneKeyMissing`, `SceneIsMap`, `SceneIsSequence`, `SceneIsNull`, `PostProcessSettingsIsScalar`, `StreamingSettingsIsSequence`, `EntitiesIsScalar`, `EntitiesContainsScalar`, `EntityIdIsMap`, `TagComponentIsScalar`, `BinaryGarbage`, `UnterminatedFlowMap`, `ParserThrowsOnAnchorOnlyDoc`, `SinglePrintableChar` |
| [SnowSettingsTest.cpp](../OloEngine/tests/SnowSettingsTest.cpp) | 7 | **SnowUBOData** &mdash; `SizeIs80Bytes`, `FieldOffsets_Std140Compatible`, `DefaultsMatchSettings`<br/>**SSSUBOData** &mdash; `SizeIs32Bytes`, `FieldOffsets_Std140Compatible`, `DefaultsMatchSettings`<br/>**ShaderBindingLayout** &mdash; `SnowAndSSSBindingsExist` |
| [SoundGraphBasicTest.cpp](../OloEngine/tests/SoundGraphBasicTest.cpp) | 8 | **SoundGraphBasicTest** &mdash; `SoundGraphAssetBasicOperations`, `SoundGraphConnections`, `SoundGraphRemoveConnection`, `CircularBufferSingleChannel`, `CircularBufferMultiChannel`, `SampleBufferOperationsInterleaving`, `SampleBufferOperationsGain`, `SoundGraphValidation` |
| [SoundGraphInstantiationTest.cpp](../OloEngine/tests/SoundGraphInstantiationTest.cpp) | 2 | **SoundGraphInstantiation** &mdash; `NoiseToGraphOutputCompilesAndInstantiates`, `WavePlayerWithUnconfiguredAssetDoesNotCrash` |
| [SoundGraphSampleAccurateTriggerTest.cpp](../OloEngine/tests/SoundGraphSampleAccurateTriggerTest.cpp) | 16 | **SoundGraphSampleAccurateTriggerTest** &mdash; `TriggerStartsUnfired`, `TriggerFireRecordsOffsetAndConsumeClears`, `TriggerFirstFireWinsWithinABlock`, `TriggerNegativeOffsetClampsToBlockStart`, `OutputEventForwardsSampleOffsetToInputEvent`, `LegacySingleArgFireDeliversBlockBoundaryOffset`, `ADEnvelopeTriggerOffsetShiftsAttackByExactlyThatManyFrames`, `ADEnvelopeOffsetZeroStartsAtFrameZero`, `ADEnvelopeWithoutTriggerStaysSilent`, `ADSREnvelopeReleaseTakesEffectAtItsExactFrame`, `ADSREnvelopeTriggerOffsetShiftsAttackByExactlyThatManyFrames`, `ProducerFiringMidBlockRetriggersWiredEnvelopeAtThatFrame`, `RepeatTriggerEmitsSampleAccurateOffsets`, `WavePlayerStopTriggerFiresOnStopAtItsExactFrame`, `WavePlayerPlayWithoutAssetIsConsumedAtItsExactFrame`, `WavePlayerWithoutTriggersFiresNoEvents` |
| [SoundGraphTypedConnectionTest.cpp](../OloEngine/tests/SoundGraphTypedConnectionTest.cpp) | 9 | **SoundGraphTypedConnections** &mdash; `NodeToNodeAudioConnectionDeliversSamples`, `AssetDefaultPlugsReachNodeInputs`, `EachNodeIsProcessedOncePerBlock`, `TypeMismatchedConnectionIsRejected`, `ScalarConnectionFlowsBetweenNodes`, `GraphFloatParameterRampsPerSample`, `DebugBlockProcessingKeepsRealTimeHeadroom`<br/>**SoundGraphCompiledPlan** &mdash; `FactoryNodeThunkIsDevirtualizedAndInvokesProcess`, `NodeOutputBuffersArePooledContiguously` |
| [StateMachineTest.cpp](../OloEngine/tests/StateMachineTest.cpp) | 12 | **StateMachineTest** &mdash; `StartCallsOnEnter`, `UpdateCallsOnUpdate`, `TransitionChangesState`, `ForceTransition`, `ForceTransition_ToSameState_DoesNothing`, `ForceTransition_InvalidState_DoesNothing`, `UpdateBeforeStart_DoesNothing`, `MultipleTransitions_FirstMatchWins`<br/>**StateMachineAssetTest** &mdash; `HasCorrectAssetType`, `StoresStatesAndTransitions`<br/>**FSMStateRegistryTest** &mdash; `RegisterAndCreate`, `UnknownTypeReturnsNull` |
| [StaticMeshColliderGenerationTest.cpp](../OloEngine/tests/StaticMeshColliderGenerationTest.cpp) | 5 | **StaticMeshColliderGenerationTest** &mdash; `GenerateFlagWiresColliderAssetToMeshSource`, `GenerateFlagOffProducesNoCollider`, `EmptyMeshSourceSkipsGeneration`, `ReSetupKeepsColliderHandleStable`, `GeneratedColliderCooksAndIsHitByRaycast` |
| [TransformComponentTest.cpp](../OloEngine/tests/TransformComponentTest.cpp) | 9 | **TransformComponent** &mdash; `DefaultsAreIdentity`, `SetRotationEulerUpdatesQuaternion`, `SetRotationQuaternionUpdatesEuler`, `SetRotationPreventsFlips`, `SetRotationContinuousAcrossPiBoundary`, `GetTransformUsesQuaternion`, `SetTransformRoundTrip`, `CopyPreservesPrivateFields`<br/>**MathDecompose** &mdash; `TRSMatrixRoundTripsThroughDecompose` |
| [VideoPlaybackTest.cpp](../OloEngine/tests/VideoPlaybackTest.cpp) | 15 | **VideoDecoder** &mdash; `DefaultIsClosed`, `OpenEmptyPathFails`, `OpenMissingFileFails`, `DecodeWithoutOpenReturnsFalse`<br/>**VideoFrameInfo** &mdash; `DefaultsAreZero`<br/>**VideoPlayer** &mdash; `DefaultState`, `LoadMissingFileFails`, `PlayPauseStopTransitions`, `SeekClampsToNonNegative`, `PlaybackSpeedClamping`, `VolumeClamping`, `LoopingToggles`, `UpdateOnUnloadedPlayerIsInert`<br/>**VideoTexture** &mdash; `UninitializedContract`<br/>**VideoDecoderFixture** &mdash; `DecodesFrameWhenFixtureAvailable` |
| [WindSettingsTest.cpp](../OloEngine/tests/WindSettingsTest.cpp) | 6 | **WindUBOData** &mdash; `SizeIs64Bytes`, `FieldOffsets_Std140Compatible`, `DefaultsMatchSettings`<br/>**ShaderBindingLayout** &mdash; `WindBindingsExist`<br/>**SnowSettings** &mdash; `WindDriftFactorDefaultIsZero`, `WindDriftFactorInUBOFlags` |

#### AI (1 file)

| File | Tests | Cases |
|---|---:|---|
| [GoapTest.cpp](../OloEngine/tests/AI/GoapTest.cpp) | 26 | **GoapWorldState** &mdash; `SetGetHasRemove`, `GetOrFallsBackOnMissingOrWrongType`, `SatisfiesSemantics`, `SatisfiesDistinguishesBoolAndIntTypes`, `ApplyEffectsOverlays`, `UnsatisfiedCount`, `EqualityAndHashAreOrderIndependent`, `HashSeparatesBoolFalseFromIntZero`<br/>**GoapPlanner** &mdash; `GoalAlreadySatisfiedYieldsEmptyFoundPlan`, `SingleActionPlan`, `MultiStepChainIsOrderedCorrectly`, `UnreachableGoalIsNotFound`, `ChoosesCheaperOfCompetingPlans`, `DijkstraModeFindsMinimumCostMultiStepPlan`, `IsUsableGateExcludesAction`, `MaxPlanLengthBoundsTheSearch`, `IterationCapTerminatesAndReports`, `NoOpActionsDoNotCauseInfiniteLoop`, `ClassicWoodcutterScenario`<br/>**GoapAgent** &mdash; `PlansAndExecutesToGoalWithInstantActions`, `PicksHighestPriorityRelevantGoal`, `SkipsGoalWhoseRelevanceGateIsClosed`, `RunningActionSpansMultipleTicks`, `ReplansWhenAnActionFails`, `SensorRefreshesWorldStateBeforePlanning`, `NoSatisfiableGoalLeavesAgentIdle` |

#### Animation (7 files)

| File | Tests | Cases |
|---|---:|---|
| [AimIKSolverTest.cpp](../OloEngine/tests/Animation/AimIKSolverTest.cpp) | 8 | **AimIKSolverTest** &mdash; `SingleBoneAimAtTarget`, `ZeroWeightPassthrough`, `MultiBoneChainDistributesRotation`, `ZeroChainLengthIsNoOp`, `InvalidBoneIndexIsNoOp`, `TargetAtBonePositionDoesNotCrash`, `PartialWeightBlendsResult`, `ChainFactorZeroPutsAllRotationOnEndBone` |
| [BlendUtilsTest.cpp](../OloEngine/tests/Animation/BlendUtilsTest.cpp) | 17 | **BlendUtilsTest** &mdash; `TransformPointAppliesTRS`, `TransformVectorIgnoresTranslation`, `InverseTransformUndoesOriginal`, `MultiplyTransformsIsAssociative`, `ModelSpacePoseRootIsLocal`, `ModelSpacePoseChildChainsCorrectly`, `ComputeModelSpaceTransformMatchesFullPose`, `LerpPoseWeight0ReturnsA`, `LerpPoseWeight1ReturnsB`, `LerpPoseWeightHalfInterpolates`, `AdditiveWeight0ReturnsBase`, `AdditiveRestPoseChangesNothing`, `AdditiveTranslationAddsWeightedDelta`, `MaskedLerpRootBone0BlendsAll`, `MaskedLerpOnlyAffectsSubtree`, `MaskedLerpAlpha0LeavesAllUnchanged`, `AdditiveMaskOnlyAffectsSubtree` |
| [FABRIKSolverTest.cpp](../OloEngine/tests/Animation/FABRIKSolverTest.cpp) | 13 | **FABRIKSolverTest** &mdash; `ReachableTargetConverges`, `UnreachableTargetStraightensChain`, `BoneLengthsPreserved`, `PoleVectorBiasesBendDirection`, `TargetAtInteriorJointPreservesLengths`, `ZeroWeightPassthrough`, `PartialWeightBlendsResult`, `ChainLengthBelowTwoIsNoOp`, `InvalidBoneIndexIsNoOp`, `NonFiniteTargetIsNoOp`, `ZeroLengthChainHandledGracefully`, `PartialChainLeavesRootBonesUntouched`, `TargetAtCurrentPositionMinimalChange` |
| [LimbIKSolverTest.cpp](../OloEngine/tests/Animation/LimbIKSolverTest.cpp) | 8 | **LimbIKSolverTest** &mdash; `ReachableTargetConverges`, `UnreachableTargetExtendsChain`, `ZeroWeightPassthrough`, `ZeroChainLengthIsNoOp`, `InvalidBoneIndexIsNoOp`, `PartialWeightBlendsResult`, `SingleBoneChainHandledGracefully`, `TargetAtCurrentPositionMinimalChange` |
| [NoiseSolverTest.cpp](../OloEngine/tests/Animation/NoiseSolverTest.cpp) | 13 | **NoiseSolverTest** &mdash; `OffsetBoundedByAmplitude`, `WeightScalesBound`, `ZeroWeightIsZeroOffset`, `DeterministicGivenParamsAndTime`, `BonesAreDecorrelated`, `SeedChangesMotion`, `MotionIsSmoothOverTime`, `NonFiniteParamsProduceZeroOffset`, `ApplyTouchesOnlyChainBones`, `DegenerateConfigsAreNoOps`, `PostPassDisplacesChain`, `PostPassIsFrameRateIndependent`, `PostPassDisabledIsPassthrough` |
| [OneShotBlendTest.cpp](../OloEngine/tests/Animation/OneShotBlendTest.cpp) | 14 | **OneShotBlendTest** &mdash; `StartsIdle`, `TriggerWithoutClipStaysIdle`, `TriggerWithClipStartsBlendIn`, `CompleteLifecycle_BlendIn_Playing_BlendOut_Idle`, `ZeroBlendDurationsSkipBlendPhases`, `CancelJumpsToBlendOut`, `CancelWhileIdleDoesNothing`, `OnFinishedCallbackFires`, `OnFinishedCallbackNotCalledDuringPlayback`, `BlendInInterpolatesWeight`, `ZeroWeightDoesNotModifyPose`, `FullWeightAppliesClipPose`, `ReTriggerDuringPlaybackRestarts`, `IdleUpdateDoesNotModifyPose` |
| [SpringBoneSolverTest.cpp](../OloEngine/tests/Animation/SpringBoneSolverTest.cpp) | 12 | **SpringBoneSolverTest** &mdash; `FirstSolveInitializesStateWithoutModifyingPose`, `RestPoseIsStableOverManySteps`, `GravitySagsChainBelowRestPose`, `SegmentLengthsArePreserved`, `ConvergesBackToRestAfterDisturbance`, `DeterministicGivenPoseAndDeltaTime`, `NonFiniteParamsAreRejected`, `ZeroWeightPassthrough`, `ChainLengthOneIsNoOp`, `OutOfRangeEndBoneIsNoOp`, `StateReinitializesWhenChainShapeChanges`, `NonFiniteStateSelfHeals` |

#### Async (8 files)

| File | Tests | Cases |
|---|---:|---|
| [ExternalMutexTest.cpp](../OloEngine/tests/Async/ExternalMutexTest.cpp) | 5 | **ExternalMutexTest** &mdash; `IsLockedAndTryLock`, `WithUniqueLockSlowPath`, `MutualExclusion`, `StatePreservation`, `MultipleMutexesSameState` |
| [ManualResetEventTest.cpp](../OloEngine/tests/Async/ManualResetEventTest.cpp) | 7 | **ManualResetEventTest** &mdash; `NotifyAndWait`, `WaitForUnset`, `WaitForSet`, `Reset`, `MultipleWaiters`, `NotifyBeforeWait`, `ResetWhileWaiting` |
| [MutexLockRateTest.cpp](../OloEngine/tests/Async/MutexLockRateTest.cpp) | 6 | **MutexLockRateTest** &mdash; `DISABLED_MutexLockRate`, `DISABLED_RecursiveMutexLockRate`, `DISABLED_SharedMutexLockRate`, `DISABLED_SharedRecursiveMutexLockRate`, `DISABLED_ExternalMutexLockRate`, `SmokeTest` |
| [MutexTest.cpp](../OloEngine/tests/Async/MutexTest.cpp) | 8 | **MutexTest** &mdash; `TryLockWhenUnlocked`, `TryLockWhenLocked`, `IsLocked`, `UniqueLockBasic`, `DynamicUniqueLockMoveConstruction`, `DynamicUniqueLockMoveAssignment`, `MultiThreadedContention`, `SlowLockUnlock` |
| [ParkingLotTest.cpp](../OloEngine/tests/Async/ParkingLotTest.cpp) | 5 | **ParkingLotTest** &mdash; `CanWaitReturnsFalse`, `WaitForWithTimeout`, `WaitUntilWithTimeout`, `FIFOOrderingAndWakeToken`, `WakeAll` |
| [SemaphoreTest.cpp](../OloEngine/tests/Async/SemaphoreTest.cpp) | 8 | **SemaphoreTest** &mdash; `InitialCount`, `TryAcquireWhenEmpty`, `TryAcquireForWithTimeout`, `TryAcquireUntilWithTimeout`, `AcquireAndRelease`, `ReleaseMultiple`, `ProducerConsumer`, `MultipleProducersConsumers` |
| [SharedMutexTest.cpp](../OloEngine/tests/Async/SharedMutexTest.cpp) | 13 | **SharedMutexTest** &mdash; `SingleThreadExclusive`, `SingleThreadShared`, `MultipleReaders`, `TryLockWhenUnlocked`, `TryLockWhenExclusiveLocked`, `TryLockWhenSharedLocked`, `TryLockSharedWhenExclusiveLocked`, `ConcurrentReaders`, `ExclusiveBlocksReaders`, `ReaderWriterInterleaving`, `ScopedSharedLock`, `ScopedExclusiveLock`, `StressTest` |
| [SharedRecursiveMutexTest.cpp](../OloEngine/tests/Async/SharedRecursiveMutexTest.cpp) | 6 | **SharedRecursiveMutexTest** &mdash; `SingleThreadExclusiveLock`, `SingleThreadRecursiveSharedLock`, `SingleThreadDynamicSharedLock`, `SingleThreadRecursiveExclusiveLock`, `MultipleThreadsBasic`, `StressTest` |

#### Audio (4 files)

| File | Tests | Cases |
|---|---:|---|
| [AudioCommandRegistryTest.cpp](../OloEngine/tests/Audio/AudioCommandRegistryTest.cpp) | 13 | **AudioCommandRegistryTest** &mdash; `AddTrigger`, `RemoveTrigger`, `RemoveNonExistentTrigger`, `DuplicateNameReturnsSameID`, `AddAction`, `RemoveAction`, `RemoveActionOutOfRange`, `Clear`, `SerializeDeserializeRoundTrip`, `DeserializeNonExistentFile`, `MultipleTriggersRoundTrip`, `DeserializeSanitizesInvalidMultipliers`, `SerializeReturnsFalseOnBadPath` |
| [AudioEventsManagerTest.cpp](../OloEngine/tests/Audio/AudioEventsManagerTest.cpp) | 6 | **AudioEventsManagerTest** &mdash; `PostTriggerReturnsUniqueIDs`, `StopActionDoesNotCreateActiveEvent`, `PostTriggerWithUnknownCommandReturnsZero`, `StopAllOnEmptyActiveSetDoesNotCrash`, `IsEventActiveReturnsFalseForInvalidID`, `ShutdownOnProcessedEmptySetDoesNotCrash` |
| [AudioEventsSceneIntegrationTest.cpp](../OloEngine/tests/Audio/AudioEventsSceneIntegrationTest.cpp) | 16 | **AudioEventsSceneIntegrationTest** &mdash; `PositionResolverFindsEntityByUUID`, `PositionResolverReturnsFalseForMissingEntity`, `PositionResolverReflectsMovedEntity`, `AudioPlaybackPostTriggerDelegatesToManager`, `AudioPlaybackStopAllOnEmptyActiveSetDelegates`, `AudioPlaybackIsEventActiveReturnsFalseForStopOnlyTrigger`, `AudioPlaybackWithNullManagerReturnsDefaults`, `StopEventCancelsPendingBeforeUpdate`, `PostTriggerRejectsUnknownCommandID`, `FullLifecycleInitUpdateShutdown`, `ShutdownClearsPositionResolver`, `PostTriggerWithObjectIDQueuesEvent`, `MultipleEntitiesPostEventsIndependently`, `StopEventOnNonExistentIDDoesNotCrash`, `AudioSourceComponentEventSystemFlag`, `CommandIDFromStartEventMatchesRegistry` |
| [CommandIDTest.cpp](../OloEngine/tests/Audio/CommandIDTest.cpp) | 8 | **CommandIDTest** &mdash; `FromStringDeterministic`, `EmptyStringInvalid`, `CaseInsensitive`, `DifferentStringsDifferentIDs`, `ValidIDIsValid`, `DefaultConstructedInvalid`, `Comparison`, `ExplicitConstruction` |

#### Cinematic (4 files)

| File | Tests | Cases |
|---|---:|---|
| [CinematicCurveTest.cpp](../OloEngine/tests/Cinematic/CinematicCurveTest.cpp) | 13 | **CinematicCurveTest** &mdash; `EmptyFloatChannelReturnsFallback`, `FloatChannelClampsOutsideRange`, `FloatChannelLinearMidpoint`, `FloatChannelConstantHoldsLeftKey`, `FloatChannelEaseInOutIsSmoothstep`, `ApplyInterpContract`, `Vec3ChannelLinearMidpoint`, `Vec3ChannelDegenerateSegmentIsSafe`, `QuatChannelSlerpMidpointAndNormalization`, `QuatChannelDegenerateKeyDoesNotNaN`, `QuatChannelEmptyReturnsFallback`, `VisibilityTrackStepSemantics`, `VisibilityTrackBeforeFirstKeyUsesFallback` |
| [CinematicEditTest.cpp](../OloEngine/tests/Cinematic/CinematicEditTest.cpp) | 14 | **CinematicEditTest** &mdash; `InsertIntoEmptyReturnsZero`, `InsertKeepsAscendingOrder`, `InsertAtFrontAndBack`, `InsertTieGoesAfterExisting`, `MoveKeyForwardReindexesAndResorts`, `MoveKeyBackwardReindexes`, `MoveKeyClampsNegativeTimeToZero`, `MoveKeyRejectsNonFiniteTime`, `MoveKeyOutOfRangeIsNoOp`, `RemoveKeyErasesAndKeepsOrder`, `RemoveKeyOutOfRangeReturnsFalse`, `WorksOnVisibilityKeys`, `WorksOnEventKeys`, `WorksOnQuatKeys` |
| [CinematicPlayerTest.cpp](../OloEngine/tests/Cinematic/CinematicPlayerTest.cpp) | 14 | **CinematicPlayerTest** &mdash; `AdvanceTimeNormalStep`, `AdvanceTimeSpeedScales`, `AdvanceTimeNegativeSpeedHolds`, `AdvanceTimeClampsAtEndWhenNotLooping`, `AdvanceTimeWrapsWhenLooping`, `AdvanceTimeZeroDurationFinishesImmediately`, `CollectEventsHalfOpenWindow`, `CollectEventsOrdersByTimeAcrossTracks`, `TickFiresZeroTimeEventOnFirstStep`, `TickDoesNotDoubleFireAcrossConsecutiveSteps`, `TickClampFinishFiresFinalEvent`, `TickLoopWrapFiresTailThenHead`, `TickMultiLapFiresEachCompletedLapEvents`, `EffectiveDurationDerivesFromKeysWhenUnset` |
| [CinematicSerializerTest.cpp](../OloEngine/tests/Cinematic/CinematicSerializerTest.cpp) | 5 | **CinematicSerializerTest** &mdash; `RoundTripPreservesStructureAndValues`, `EmptySequenceRoundTrips`, `MalformedYamlReturnsNullWithoutCrashing`, `NullSequenceSerializesToEmptyString`, `MalformedKeysAreSkippedNotMaterialized` |

#### Containers (1 file)

| File | Tests | Cases |
|---|---:|---|
| [ConcurrentQueuesTest.cpp](../OloEngine/tests/Containers/ConcurrentQueuesTest.cpp) | 16 | **SpscQueueTest** &mdash; `BasicPushPop`, `EmptyQueue`, `SingleProducerSingleConsumer`, `MoveOnlyType`<br/>**MpscQueueTest** &mdash; `BasicPushPop`, `MultipleProducers`<br/>**ClosableMpscQueueTest** &mdash; `BasicOperations`, `CloseQueue`, `CloseEmptyQueue`, `MultipleProducersBeforeClose`<br/>**ConsumeAllMpmcQueueTest** &mdash; `BasicOperations`, `EmptyQueue`, `ConsumeAllLifo`, `MultipleProducersMultipleConsumers`<br/>**QueueStressTest** &mdash; `SpscHighThroughput`, `MpscHighContention` |

#### ContentBrowser (2 files)

| File | Tests | Cases |
|---|---:|---|
| [ContentBrowserItemTest.cpp](../OloEngine/tests/ContentBrowser/ContentBrowserItemTest.cpp) | 19 | **FileTypeMapping** &mdash; `ImageExtensions`, `ModelExtensions`, `SceneExtensions`, `ScriptExtensions`, `AudioExtensions`, `MaterialExtensions`, `ShaderExtensions`, `EngineSpecificExtensions`, `UnknownExtensionReturnsUnknown`, `CaseInsensitive`<br/>**DragDropPayload** &mdash; `ScenePayload`, `ModelPayload`, `GenericFallback`<br/>**SortContentBrowserItems** &mdash; `DirectoriesBeforeFiles`, `AlphabeticalWithinSameType`, `MixedDirsAndFilesSorted`, `EmptyVectorSafe`<br/>**ActionResultHelpers** &mdash; `HasActionWorks`, `SetActionWorks` |
| [DirectoryTreeTest.cpp](../OloEngine/tests/ContentBrowser/DirectoryTreeTest.cpp) | 16 | **DirectoryTree** &mdash; `BuildCreatesRoot`, `BuildScansSubdirectories`, `BuildScansFiles`, `BuildScansRecursively`, `ParentPointersAreSet`, `FindDirectoryRoot`, `FindDirectoryNested`, `FindDirectoryReturnsNullForMissing`, `SearchEmptyQueryReturnsNothing`, `SearchFindsFilesByName`, `SearchIsCaseInsensitive`, `SearchFindsPartialMatch`, `MarkDirtyFlagsNode`, `MarkDirtyFlagsAncestors`, `RefreshPicksUpNewFiles`, `RefreshClearsNeedsRefreshFlag` |

#### Debug (1 file)

| File | Tests | Cases |
|---|---:|---|
| [CrashReporterTest.cpp](../OloEngine/tests/Debug/CrashReporterTest.cpp) | 5 | **CrashReporterTestFixture** &mdash; `CollectSystemInfoIsNonEmpty`, `ReportFatalErrorWritesReadableFile`, `WriteMiniDumpReturnsFalseWithoutContext`, `EmitToDebugOutputDoesNotCrash`, `InstallUninstallRoundTrips` |

#### Gameplay (1 file)

| File | Tests | Cases |
|---|---:|---|
| [GameplayAbilityTest.cpp](../OloEngine/tests/Gameplay/GameplayAbilityTest.cpp) | 40 | **GameplayTagTest** &mdash; `ConstructAndQuery`, `EmptyTag`, `ExactMatch`, `PartialMatch`, `PartialMatchNotSubstring`, `HashEquality`<br/>**GameplayTagContainerTest** &mdash; `AddAndQuery`, `NoDuplicates`, `RemoveTag`, `HasAll`, `HasAny`, `PartialQuery`<br/>**AttributeSetTest** &mdash; `DefineAndGet`, `SetBaseValue`, `AdditiveModifier`, `MultiplicativeModifier`, `OverrideModifier`, `StackedModifiers`, `RemoveModifiersBySource`, `GetAttributeNames`<br/>**CooldownManagerTest** &mdash; `StartAndCheck`, `TickReducesCooldown`, `CooldownExpires`, `CooldownFraction`, `ResetCooldown`<br/>**ActiveEffectsContainerTest** &mdash; `InstantEffect`, `DurationEffect`, `PeriodicEffect`, `StackingEffect`, `RequiredTagsBlocked`, `BlockedTagsPrevents`, `RefCountedTags`<br/>**DamageCalculationTest** &mdash; `BasicDamage`, `CriticalHit`, `Resistance`, `MinimumZeroDamage`, `CustomFormula`, `ScopedFormula`<br/>**AbilityComponentTest** &mdash; `InitializeDefaultRPG`, `EqualityOperator` |

#### HAL (1 file)

| File | Tests | Cases |
|---|---:|---|
| [ThreadManagerTest.cpp](../OloEngine/tests/HAL/ThreadManagerTest.cpp) | 3 | **ThreadManagerTest** &mdash; `RegistersThreadOnCreateAndDeregistersOnDestroy`, `TracksMultipleConcurrentThreads`, `NamedThreadIdAgreesWithRegistryId` |

#### Lua (1 file)

| File | Tests | Cases |
|---|---:|---|
| [LuaBindingTest.cpp](../OloEngine/tests/Lua/LuaBindingTest.cpp) | 119 | **LuaBindingTest** &mdash; `Vec2_ConstructAndAccess`, `Vec3_ConstructAndAccess`, `Vec4_ConstructAndAccess`, `TransformComponent_TranslationRoundTrip`, `TransformComponent_ScaleRoundTrip`, `TransformComponent_RotationRoundTrip`, `Rigidbody2D_PropertyRoundTrip`, `BoxCollider2D_PropertyRoundTrip`, `BoxCollider2D_RejectsInvalidInputs`, `CircleCollider2D_PropertyRoundTrip`, `CircleCollider2D_RejectsInvalidInputs`, `CameraComponent_PrimaryRoundTrip`, `CameraComponent_SceneCameraProperties`, `SpriteRenderer_PropertyRoundTrip`, `CircleRenderer_PropertyRoundTrip`, `TextComponent_StringRoundTrip`, `TextComponent_PropertyRoundTrip`, `MeshComponent_PrimitiveRoundTrip`, `UICanvasComponent_PropertyRoundTrip`, `UIRectTransformComponent_PropertyRoundTrip`, `UIImageComponent_PropertyRoundTrip`, `UIPanelComponent_PropertyRoundTrip`, `UITextComponent_PropertyRoundTrip`, `UIButtonComponent_PropertyRoundTrip`, `UISliderComponent_PropertyRoundTrip`, `UICheckboxComponent_PropertyRoundTrip`, `UIProgressBarComponent_PropertyRoundTrip`, `UIInputFieldComponent_PropertyRoundTrip`, `UIScrollViewComponent_PropertyRoundTrip`, `UIDropdownComponent_PropertyRoundTrip`, `UIGridLayoutComponent_PropertyRoundTrip`, `UIToggleComponent_PropertyRoundTrip`, `ParticleSystem_PropertyRoundTrip`, `ParticleEmitter_PropertyRoundTrip`, `ParticleSystem_RejectsInvalidInputs`, `ParticleEmitter_RejectsInvalidInputs`, `ParticleSystemComponent_SystemAccess`, `LightProbeComponent_PropertyRoundTrip`, `LightProbeVolumeComponent_PropertyRoundTrip`, `UIWorldAnchorComponent_PropertyRoundTrip`, `NameplateComponent_PropertyRoundTrip`, `IKTargetComponent_AimProperties`, `IKTargetComponent_LimbProperties`, `IKTargetComponent_ChainProperties`, `IKTargetComponent_ChainPropertiesClamped`, `SpringBoneComponent_PropertyRoundTrip`, `WindSettings_PropertyRoundTrip`, `StreamingVolumeComponent_PropertyRoundTrip`, `StreamingSettings_PropertyRoundTrip`, `NetworkIdentityComponent_PropertyRoundTrip`, `AudioSourceComponent_BasicProperties`, `AudioSourceComponent_VolumeClamping`, `AudioSourceComponent_PitchClamping`, `AudioSourceComponent_SpatialProperties`, `AudioSourceComponent_NaNSafety`, `AudioListenerComponent_PropertyRoundTrip`, `DialogueComponent_PropertyRoundTrip`, `NavAgentComponent_PropertyRoundTrip`, `NavAgentComponent_RejectsInvalidInputs`, `ItemPickupComponent_PropertyRoundTrip`, `ItemContainerComponent_PropertyRoundTrip`, `QuestGiverComponent_PropertyRoundTrip`, `MaterialComponent_AlbedoColorRoundTrip`, `Rigidbody3DComponent_PropertyRoundTrip`, `BoxCollider3DComponent_PropertyRoundTrip`, `SphereCollider3DComponent_PropertyRoundTrip`, `CapsuleCollider3DComponent_PropertyRoundTrip`, `DirectionalLightComponent_PropertyRoundTrip`, `DirectionalLightComponent_RejectsInvalidInputs`, `PointLightComponent_PropertyRoundTrip`, `PointLightComponent_RejectsInvalidInputs`, `SpotLightComponent_PropertyRoundTrip`, `SpotLightComponent_RejectsInvalidInputs`, `SphereAreaLightComponent_PropertyRoundTrip`, `SphereAreaLightComponent_RejectsInvalidInputs`, `TagComponent_PropertyRoundTrip`, `ScriptComponent_PropertyRoundTrip`, `LuaScriptComponent_PropertyRoundTrip`, `ModelComponent_PropertyRoundTrip`, `ComponentRegistry_UsertypesExist`, `MaterialComponent_ShaderGraphHandleRoundTrip`, `QuestJournalComponent_ItemCountRoundTrip`, `QuestJournalComponent_StatRoundTrip`, `QuestJournalComponent_PlayerClassRoundTrip`, `QuestJournalComponent_PlayerFactionRoundTrip`, `QuestJournalComponent_TagRoundTrip`, `QuestJournalComponent_PlayerLevelRoundTrip`, `QuestJournalComponent_ReputationRoundTrip`, `LogTable_FunctionsExist`, `LogTable_CallsDoNotThrow`, `KeyCode_TableExists`, `KeyCode_LetterValues`, `KeyCode_FunctionKeyValues`, `KeyCode_ArrowKeys`, `KeyCode_ModifierKeys`, `MouseButton_TableExists`, `MouseButton_Values`, `EntityUtils_TableExists`, `EntityUtils_HasExpectedFunctions`, `Application_TableExists`, `Application_HasExpectedFunctions`, `Scene_TableExists`, `Damage_TableExists`, `Damage_HasExpectedFunctions`<br/>**LuaSceneTest** &mdash; `ComponentRegistry_SceneBacked`, `FindByName_ReturnsEntityID`, `FindByName_ReturnsNilForMissing`, `GetSetTranslation_RoundTrip`, `GetName_ReturnsTagName`, `GetComponent_ReturnsProxy`, `ProxyWrite_UpdatesComponent`, `ProxyRead_ReturnsCopy_NotReference`, `ProxyRead_NestedUsertype_ReturnsReference`, `ProxyRead_ColliderMaterial_ReturnsReference`, `HasComponent_ReturnsTrueForExisting`, `ProxyRead_ParticleSystem_ReturnsReference`, `SetTranslation_RejectsNonFinite`, `Proxy_MethodCallReResolvesComponent`, `Proxy_SafelyDegradeAfterEntityDestruction` |

#### MCP (9 files)

| File | Tests | Cases |
|---|---:|---|
| [McpDispatchTest.cpp](../OloEngine/tests/MCP/McpDispatchTest.cpp) | 44 | **McpDispatchTest** &mdash; `InitializeReturnsHandshakeShape`, `InitializeEchoesSupportedProtocolVersion`, `InitializeFallsBackToLatestForUnknownVersion`, `PingReturnsEmptyResult`, `ToolsListSurfacesRegisteredToolsWithSchema`, `ResourcesListSurfacesRegisteredResources`, `PromptsListSurfacesRegisteredPrompts`, `ToolsCallHappyPathReturnsContent`, `ToolsCallUnknownToolIsProtocolError`, `ToolsCallMissingNameIsInvalidParams`, `ToolsCallToolErrorIsNotProtocolError`, `ToolsCallHandlerThrowIsCaughtAsToolError`, `ResourcesReadHappyPathReturnsContents`, `ResourcesReadUnknownUriIsError`, `ResourcesReadMissingUriIsInvalidParams`, `PromptsGetHappyPathExpandsToUserMessage`, `PromptsGetUnknownNameIsError`, `UnknownMethodIsMethodNotFound`, `MissingMethodIsInvalidRequest`, `NonObjectMessageIsInvalidRequest`, `NonStringMethodIsInvalidRequest`, `NotificationProducesNoResponse`, `FramingMalformedBodyIsParseError`, `FramingSingleMessageReturnsResponse`, `FramingBatchPreservesOrder`, `FramingBatchDropsNotifications`, `FramingAllNotificationBatchReturnsNoBody`, `FramingEmptyBatchIsInvalidRequest`, `FramingSingleNotificationReturnsNoBody`, `FramingInitializeMintsSessionId`, `FramingNonInitializeMintsNoSessionId`, `FramingInitializeNotificationMintsNoSessionId`<br/>**McpDispatchSecurity** &mdash; `BearerAuthAcceptsExactToken`, `BearerAuthRejectsWrongToken`, `BearerAuthRejectsTokenPrefix`, `BearerAuthRejectsMissingOrWrongScheme`, `BearerAuthRejectsEverythingWhenServerHasNoToken`, `OriginAllowsAbsentOrNull`, `OriginAllowsLoopbackHosts`, `OriginRejectsRemoteHosts`, `OriginRejectsMalformed`<br/>**McpDiscoveryFile** &mdash; `OverrideEnvWinsVerbatimRegardlessOfPort`, `DefaultPortKeepsLegacyUnsuffixedName`, `NonDefaultPortNamespacesByPort` |
| [McpEventsTailTest.cpp](../OloEngine/tests/MCP/McpEventsTailTest.cpp) | 16 | **DiagnosticsEventLogTest** &mdash; `RecordAssignsMonotonicOneBasedIds`, `QueryReturnsOldestFirstNewestLast`, `RecordPreservesEntityAndContext`, `SinceIdReturnsOnlyStrictlyNewer`, `SinceIdAtLatestReturnsEmptyAndCursorIsStable`, `CategoryFilterKeepsOnlyRequested`, `CategoryFilterAcceptsMultiple`, `MaxCountKeepsNewest`, `MaxCountAndSinceIdCompose`, `RingBufferWrapsAndDropsOldestKeepingNewest`, `SuppressScopeMutesRecordingThenRestores`, `SuppressScopeNestsByDepth`, `ClearResetsBufferAndIdCounter`, `QueryWithCursorReportsBufferCursorNotFilteredMax`<br/>**DiagnosticsEventCategory** &mdash; `StringRoundTripsForEveryCategory`, `FromStringRejectsUnknownToken` |
| [McpFrameBreakdownTest.cpp](../OloEngine/tests/MCP/McpFrameBreakdownTest.cpp) | 8 | **McpFrameBreakdown** &mdash; `ParseViewModeMapsStringsAndDefaults`, `EmptyFrameProducesValidShape`, `PostSortCommandsAreListedInStageOrder`, `DrawKeyAndMetadataFieldsDecoded`, `GpuTimeIsReportedRounded`, `MaxCommandsTruncatesButReportsFullCountAndHistogram`, `ViewModeFallsBackToPreSortWhenLaterStagesEmpty`, `StatsAndStageCountsPassThrough` |
| [McpGoldenCompareTest.cpp](../OloEngine/tests/MCP/McpGoldenCompareTest.cpp) | 9 | **McpGoldenCompare** &mdash; `IdenticalImagesAreAPerfectMatch`, `DimensionMismatchShortCircuits`, `MalformedBufferIsRejected`, `TinyUniformShiftStillPasses`, `LargeKnownDeltaFails`, `ExplicitThresholdGatesOnSimilarity`, `ExplicitThresholdBoundaryIsInclusive`, `ThresholdModeReportedWithoutExplicitThreshold`, `WorstPixelLocationIsReported` |
| [McpPhysicsExplainTest.cpp](../OloEngine/tests/MCP/McpPhysicsExplainTest.cpp) | 19 | **McpPhysicsExplain** &mdash; `BodyTypeNameCoversAllTypes`, `FullyEligibleAndOverlappingWouldCollide`, `SameEntityIsRejectedFirst`, `PhysicsNotRunning`, `EntityAMissing`, `EntityBMissing`, `EntityANoRigidbody`, `EntityANoColliderReportedAfterRigidbody`, `EntityANoLiveBody`, `EntityASideCheckedBeforeEntityB`, `EntityBNoRigidbody`, `EntityBNoCollider`, `EntityBNoBody`, `BothStaticNeverCollide`, `StaticVsDynamicPassesTheStaticGate`, `LayersDontCollide`, `TriggerProducesNoSolidResponse`, `NotOverlappingCanStillCollide`, `ChecksAreOrderedAndPrefixed` |
| [McpRenderExplainTest.cpp](../OloEngine/tests/MCP/McpRenderExplainTest.cpp) | 17 | **McpRenderExplain** &mdash; `FullyConfiguredAndInViewShouldBeVisible`, `NoSceneIsRejectedFirst`, `EntityMissing`, `NotRenderable`, `GeometryMissingReportedAfterRenderable`, `GeometryNotRequiredSkipsTheGeometryGate`, `ComponentHidden`, `DegenerateScale`, `ShaderCompileError`, `UnresolvedMaterialShaderSkipsTheShaderGate`, `BehindCamera`, `OutsideFrustumReportedAfterBehindCamera`, `CameraChecksSkippedWhenCameraUnknown`, `CameraChecksSkippedWhenBoundsUnknown`, `GlobalShaderErrorsAreAHintNotABlocker`, `ConfigGatesPrecedeCameraGates`, `ChecksAreOrderedAndPrefixed` |
| [McpRenderOverridesTest.cpp](../OloEngine/tests/MCP/McpRenderOverridesTest.cpp) | 15 | **McpRenderOverrides** &mdash; `ParsesEveryCanonicalPassToken`, `ParsePassIsCaseAndSeparatorInsensitive`, `ParsePassResolvesAliases`, `ParsePassRejectsUnknownAndEmpty`, `PassTokensCoversEveryPassAndDescribePassesMatches`, `ToggleResultJsonShape`, `ToggleResultIncludesNoteWhenSet`, `ToggleResultUnchangedWhenStateMatches`, `ParsesEveryDebugViewMode`, `ParseDebugViewTreatsEmptyAndOffAsNone`, `ParseDebugViewRejectsUnknown`, `DebugViewResultJsonShapeActive`, `DebugViewResultNotePresentWhenPassDisabled`, `DebugViewModesCoversEveryModeAndDescribeMatches`, `JoinTokensProducesCommaSeparatedList` |
| [McpShaderReloadTest.cpp](../OloEngine/tests/MCP/McpShaderReloadTest.cpp) | 4 | **McpShaderReload** &mdash; `StatusTokenMapsEveryEnumToLowercase`, `CleanReloadIsReadyWithEmptyLog`, `FailedReloadCarriesStatusAndLog`, `NotFoundReportsFoundFalseAndNoLibraries` |
| [McpToolAnnotationsTest.cpp](../OloEngine/tests/MCP/McpToolAnnotationsTest.cpp) | 9 | **McpToolAnnotations** &mdash; `ToolsListEmitsTitleAndReadOnlyAnnotations`, `ToolsListOmitsTitleWhenEmpty`, `ToolsListOmitsAnnotationsWhenEmpty`, `ToolsListOmitsAnnotationsWhenEmptyObject`, `ReadOnlyAndMutatingClassificationRoundTrip`<br/>**McpToolNameValidation** &mdash; `AcceptsWellFormedNames`, `RejectsEmptyAndOverLong`, `RejectsDisallowedCharacters`, `RegisterToolAcceptsValidNameAndListsIt` |

#### Memory (2 files)

| File | Tests | Cases |
|---|---:|---|
| [LockFreeAllocatorConcurrencyTest.cpp](../OloEngine/tests/Memory/LockFreeAllocatorConcurrencyTest.cpp) | 1 | **LockFreeAllocatorConcurrencyTest** &mdash; `ConcurrentAllocFirstTouchesPagesWithoutRace` |
| [MemoryViewTest.cpp](../OloEngine/tests/Memory/MemoryViewTest.cpp) | 27 | **MemoryViewTest** &mdash; `DefaultConstruction`, `ConstructFromPointerAndSize`, `ConstructFromArray`, `ConstructFromStdArray`, `ConstructFromVector`, `LeftSlice`, `RightSlice`, `MidSlice`, `LeftChop`, `RightChop`, `Equality`, `CompareBytes`, `CompareBytesWithDifferentSizes`<br/>**MutableMemoryViewTest** &mdash; `DefaultConstruction`, `ConstructFromPointerAndSize`, `ModifyData`, `CopyFrom`, `CopyFromPartial`, `LeftSlice`, `RightSlice`, `MidSlice`, `ConversionToImmutable`<br/>**MemoryViewEdgeCasesTest** &mdash; `EmptySlices`, `SliceEntireView`, `EmptyViewOperations`, `SingleByteView`, `LargeView` |

#### Networking (25 files)

| File | Tests | Cases |
|---|---:|---|
| [AuthorityRejectionTest.cpp](../OloEngine/tests/Networking/AuthorityRejectionTest.cpp) | 8 | **AuthorityRejectionTest** &mdash; `AcceptsInputFromOwnerWithClientAuthority`, `RejectsInputFromNonOwner`, `RejectsInputForServerAuthoritativeEntity`, `AcceptsInputForSharedAuthorityEntity`, `RejectsInputForUnknownEntity`, `RejectsInputForEntityWithoutNetworkIdentity`, `TracksLastProcessedTickPerClient`, `RejectsTruncatedPayload` |
| [ChatTest.cpp](../OloEngine/tests/Networking/ChatTest.cpp) | 7 | **ChatChannel** &mdash; `JoinLeave`, `Properties`<br/>**ChatManager** &mdash; `CreateDestroyChannel`, `ZoneChatDelivery`, `MessageFilterBlocking`, `RemoveClientFromAllChannels`, `GetChannelsByType` |
| [ComponentReplicatorTest.cpp](../OloEngine/tests/Networking/ComponentReplicatorTest.cpp) | 8 | **ComponentReplicatorTest** &mdash; `TransformRoundtrip`, `ArIsNetArchiveFlag`<br/>**ComponentRegistryTest** &mdash; `RegisterDefaultsPopulatesThreeComponents`, `IsRegisteredReturnsFalseForUnknown`, `RegisterCustomComponent`, `GetSerializerReturnsNullForUnknown`, `ClearRegistryRemovesAll`, `RegisteredTransformSerializerWorks` |
| [DeltaSnapshotTest.cpp](../OloEngine/tests/Networking/DeltaSnapshotTest.cpp) | 4 | **DeltaSnapshotTest** &mdash; `IdenticalSnapshotsProduceEmptyDelta`, `DifferentTranslationProducesDifference`, `DeltaFormatSameAsFullFormat`, `MultiEntityDeltaFiltering` |
| [EntitySnapshotTest.cpp](../OloEngine/tests/Networking/EntitySnapshotTest.cpp) | 4 | **EntitySnapshotTest** &mdash; `ManualCaptureApplyRoundtrip`, `EmptyBufferProducesNoWork`, `MultipleEntitiesRoundtrip`, `OnlyReplicatedEntities` |
| [InputBufferTest.cpp](../OloEngine/tests/Networking/InputBufferTest.cpp) | 7 | **InputBufferTest** &mdash; `PushAndRetrieveByTick`, `GetByTickReturnsNullForMissing`, `GetUnconfirmedInputsReturnsAfterTick`, `DiscardUpToRemovesOldEntries`, `PushOverwritesOnCapacityOverflow`, `ClearRemovesAll`, `GetUnconfirmedInputsOrderedByTick` |
| [InstanceLayerTest.cpp](../OloEngine/tests/Networking/InstanceLayerTest.cpp) | 9 | **InstanceManager** &mdash; `CreateAndDestroyInstance`, `AddRemovePlayers`, `AutoDestroyEmptyInstance`, `OpenWorldNotAutoDestroyed`<br/>**LayerManager** &mdash; `CreateLayerAndAssignPlayer`, `AutoSpillToNewLayerNeeded`, `PartyLayerCohesion`, `MergeUnderPopulatedLayers`, `RemovePlayer` |
| [InterestManagerTest.cpp](../OloEngine/tests/Networking/InterestManagerTest.cpp) | 7 | **InterestManagerTest** &mdash; `EntitiesWithoutInterestComponentAreAlwaysRelevant`, `ZeroRadiusIsAlwaysRelevant`, `DistanceFilteringWorks`, `GroupFilteringWorks`, `DefaultGroupIsAlwaysIncluded`, `NonReplicatedEntitiesAreExcluded`, `IsEntityRelevantMatchesGetRelevantEntities` |
| [LagCompensatorTest.cpp](../OloEngine/tests/Networking/LagCompensatorTest.cpp) | 5 | **LagCompensatorTest** &mdash; `RewindAndRestoreWorks`, `RejectsFutureTick`, `RejectsExcessiveRewind`, `MaxRewindGetSet`, `RejectsWhenNoHistoryAvailable` |
| [LockstepTest.cpp](../OloEngine/tests/Networking/LockstepTest.cpp) | 9 | **LockstepManagerTest** &mdash; `AdvanceWaitsForAllPeers`, `AllInputsAppliedOnAdvance`, `InputDelaySchedulesFutureTick`, `TickRateGetSet`, `InputDelayGetSet`, `DesyncDetection`, `WaitForSlowPeer`<br/>**StateHashTest** &mdash; `DeterministicHash`, `DifferentDataDifferentHash` |
| [MMOOptimizationTest.cpp](../OloEngine/tests/Networking/MMOOptimizationTest.cpp) | 8 | **MMOOptimization** &mdash; `NetworkLODLevels`, `BandwidthCapEnforcement`, `MMONetworkModelExists`, `MMOMessageTypesExist`, `PhaseComponentDefaults`, `InstancePortalComponentDefaults`, `CrowdUpdateThreshold`, `NetworkLODFiltering` |
| [NetworkIdentityComponentTest.cpp](../OloEngine/tests/Networking/NetworkIdentityComponentTest.cpp) | 2 | **NetworkIdentityComponentTest** &mdash; `DefaultValues`, `CopySemantics` |
| [NetworkIntegrationTest.cpp](../OloEngine/tests/Networking/NetworkIntegrationTest.cpp) | 7 | **NetworkIntegrationTest** &mdash; `ServerStartStop`, `ClientConnectDisconnect`, `ServerTracksConnectedClient`, `ClientToServerMessage`, `ServerToClientMessage`, `PingPongBuiltin`, `StatsTrackSentAndReceived` |
| [NetworkMessageDispatcherTest.cpp](../OloEngine/tests/Networking/NetworkMessageDispatcherTest.cpp) | 10 | **NetworkMessageDispatcherTest** &mdash; `RegisterAndDispatch`, `HasHandler`, `UnregisteredTypeDoesNotCrash`, `MultipleHandlersForDifferentTypes`, `HandlerReplacesExisting`<br/>**NetworkStatsTest** &mdash; `RecordSendUpdatesCounters`, `RecordReceiveUpdatesCounters`, `UpdateRatesComputesCorrectly`, `UpdateRatesNotTriggeredBeforeOneSecond`, `ResetClearsEverything` |
| [NetworkMessageTest.cpp](../OloEngine/tests/Networking/NetworkMessageTest.cpp) | 4 | **NetworkMessageTest** &mdash; `HeaderSerializationRoundtrip`, `PrimitivePayloadRoundtrip`, `EmptyPayload`, `MaxSizePayload` |
| [NetworkThreadDispatchTest.cpp](../OloEngine/tests/Networking/NetworkThreadDispatchTest.cpp) | 2 | **NetworkThreadDispatchTest** &mdash; `EnqueueNetworkThreadTask`, `EnqueueGameThreadFromNetwork` |
| [P2PRollbackTest.cpp](../OloEngine/tests/Networking/P2PRollbackTest.cpp) | 17 | **RollbackManagerTest** &mdash; `SaveAndRestoreState`, `NoRollbackForCurrentTick`, `NoRollbackForFutureTick`, `ExcessiveRollbackDropped`, `InputPredictionRepeatsLastKnown`, `MaxRollbackGetSet`<br/>**PeerMeshTest** &mdash; `CreateSessionMakesHost`, `JoinSessionNotHost`, `LeaveSessionClearsState`, `HostMigrationSelectsLowestID`, `StartListeningFailsWithoutGNS`, `ConnectToPeerFailsWithoutGNS`, `SendToPeerNoTransportIsNoOp`, `BroadcastWithoutTransportIsNoOp`, `PollMessagesWithoutGNSIsNoOp`, `LeaveSessionCleansUpTransportState`, `MessageCallbackIsStored` |
| [PersistenceTest.cpp](../OloEngine/tests/Networking/PersistenceTest.cpp) | 9 | **WorldDatabase** &mdash; `SaveLoadPlayerRoundtrip`, `DeletePlayer`, `SaveLoadEntityState`, `WorldState`, `InitShutdown`<br/>**WorldPersistenceManager** &mdash; `DirtyEntityTracking`, `AutoSaveOnInterval`, `SaveAndLoadEntity`, `PlayerPersistence` |
| [PredictionReconciliationTest.cpp](../OloEngine/tests/Networking/PredictionReconciliationTest.cpp) | 6 | **PredictionReconciliationTest** &mdash; `RecordInputStoresInBuffer`, `ReconcileDiscardsConfirmedInputs`, `ReconcileResimuatesUnconfirmedInputs`, `ReconcileIgnoresAlreadyConfirmedTick`, `SmoothingRateGetSet`, `ReconcilePassesEntityUUIDToCallback` |
| [SessionLobbyTest.cpp](../OloEngine/tests/Networking/SessionLobbyTest.cpp) | 18 | **NetworkSession** &mdash; `CreateSetsModelAndState`, `ResetClearsEverything`, `TransitionToChangesState`, `AddRemovePlayer`, `AreAllPlayersReady`, `SessionLifecycle`<br/>**NetworkLobby** &mdash; `CreateLobby`, `CloseLobby`, `JoinLobbyFailsIfAlreadyInLobby`, `JoinLobbySucceeds`, `LeaveLobby`, `ReadyState`, `FindLobbiesReturnsEmptyWhenNoHosts`, `FindLobbiesNullCallbackDoesNotCrash`, `DiscoveryPortConstant`, `PollDiscoveryWithoutHostingIsNoOp`, `CreateLobbyAndPollDiscoveryDoesNotCrash`, `CloseLobbyClosesDiscoverySocket` |
| [SnapshotBufferTest.cpp](../OloEngine/tests/Networking/SnapshotBufferTest.cpp) | 8 | **SnapshotBufferTest** &mdash; `DefaultConstructionIsEmpty`, `PushAndRetrieveLatest`, `PushOverwritesOldest`, `GetByTickFindsCorrectEntry`, `GetBracketingEntriesNeedsTwoEntries`, `GetBracketingEntriesInterpolation`, `ClearResetsState`, `CustomCapacity` |
| [SnapshotInterpolatorTest.cpp](../OloEngine/tests/Networking/SnapshotInterpolatorTest.cpp) | 6 | **SnapshotInterpolatorTest** &mdash; `DefaultState`, `PushSnapshotsIntoBuffer`, `SetRenderDelay`, `SetServerTickRate`, `GetRenderTickComputation`, `InterpolateNeedsAtLeastTwoSnapshots` |
| [SpatialGridTest.cpp](../OloEngine/tests/Networking/SpatialGridTest.cpp) | 10 | **SpatialGrid** &mdash; `InsertAndQuery`, `RadiusAccuracy`, `UpdateMovesEntity`, `RemoveEntity`, `CellPopulation`<br/>**RelevanceTier** &mdash; `TierAssignment`, `UpdateFrequency`<br/>**NetworkPriorityQueue** &mdash; `PriorityOrdering`, `TopNCaps`, `RemoveEntity` |
| [ZoneHandoffTest.cpp](../OloEngine/tests/Networking/ZoneHandoffTest.cpp) | 7 | **PlayerStatePacket** &mdash; `SerializeDeserializeRoundtrip`, `EmptyGameStateBlob`, `RejectsNonFiniteTransform`<br/>**ZoneHandoff** &mdash; `ThreePhaseHandoffProtocol`, `HandoffRejectionOnFull`, `GhostEntityVisibility`, `PlayerTransitioningState` |
| [ZoneServerTest.cpp](../OloEngine/tests/Networking/ZoneServerTest.cpp) | 7 | **ZoneServer** &mdash; `StartStop`, `AddRemovePlayers`<br/>**ZoneManager** &mdash; `RegisterAndRoutePlayer`, `TransferPlayer`, `MaxPlayersEnforcement`<br/>**InterZoneMessageBus** &mdash; `PushAndDrainAll`, `DrainForZone` |

#### Rendering (2 files)

| File | Tests | Cases |
|---|---:|---|
| [MeshBVHRaycastTest.cpp](../OloEngine/tests/Rendering/MeshBVHRaycastTest.cpp) | 25 | **MeshBVHRayTriangle** &mdash; `HitsThroughInteriorWithCorrectBarycentrics`, `MissesOutsideTheTriangle`, `ParallelRayMisses`, `BackFaceReportsFrontFaceFalse`, `RespectsTMinAndTMax`, `TriangleBehindOriginMisses`<br/>**MeshBVHRayAABB** &mdash; `HitsBoxAndReportsEntryDistance`, `MissesWhenAxisIntervalsDoNotOverlap`, `OriginInsideBoxHitsAtTMin`, `TMaxPrunesAFarBox`, `AxisParallelRayOnSlabBoundaryHitsAndStaysFinite`, `AxisParallelRayOutsideParallelSlabMisses`, `AxisParallelRayThroughInteriorHits`<br/>**MeshBVHBuild** &mdash; `EmptyBVHReportsUnbuiltAndNeverHits`, `SkipsTrianglesWithOutOfRangeIndices`, `RootBoundsEncloseTheMesh`<br/>**MeshBVHQuery** &mdash; `SingleTriangleHitReportsIndexPointAndNormal`, `ReturnsClosestOfManyStackedTriangles`, `RayFromInsideClosedMeshHitsAWall`, `CastRayAnyAgreesWithCastRayHitMiss`, `BuildsFromMeshSourceOverload`<br/>**MeshBVHParity** &mdash; `MatchesBruteForceOverCube`, `MatchesBruteForceOverSphere`, `MatchesBruteForceForAxisAlignedRays`<br/>**MeshSourceHeapCopyRegression** &mdash; `RepeatedHeapCopyConstructionKeepsHeapIntact` |
| [PCSSShadowTest.cpp](../OloEngine/tests/Rendering/PCSSShadowTest.cpp) | 12 | **PCSSShadow** &mdash; `ShadowUBOSizeUnchangedBySoftShadowModeField`, `SoftShadowModeFollowsCascadeDebugInLayout`, `RawDepthBindingSlotsMatchShaders`, `SoftShadowsDefaultsOn`, `QualityTieringMapsSoftShadows`, `LowPresetUsesHardShadows`, `NoBlockerReportsFullyLit`, `BlockerSearchAveragesOnlyOccluders`, `ZeroGapGivesSharpMinimumRadius`, `PenumbraGrowsMonotonicallyWithGap`, `PenumbraClampsToMaxRadius`, `LargerLightSizeGivesSofterMaxPenumbra` |

#### SaveGame (5 files)

| File | Tests | Cases |
|---|---:|---|
| [SaveFileWorldDatabaseTest.cpp](../OloEngine/tests/SaveGame/SaveFileWorldDatabaseTest.cpp) | 3 | **SaveFileWorldDatabaseTest** &mdash; `ImplementsIWorldDatabase`, `DefaultIsNotInitialized`, `UninitializedOperationsFail` |
| [SaveGameComponentSerializerCoverageTest.cpp](../OloEngine/tests/SaveGame/SaveGameComponentSerializerCoverageTest.cpp) | 2 | **SaveGameComponentSerializerCoverage** &mdash; `EveryRegisteredComponentIsCapturedAndRestored`, `EveryCapturedAndRestoredComponentIsRegistered` |
| [SaveGameFileTest.cpp](../OloEngine/tests/SaveGame/SaveGameFileTest.cpp) | 15 | **SaveGameHeaderTest** &mdash; `DefaultValues`, `SizeIs128Bytes`, `CompressionFlags`, `InvalidMagic`<br/>**SaveGameMetadataTest** &mdash; `RoundTrip`<br/>**SaveGameCompressionTest** &mdash; `CompressDecompress`, `EmptyData`<br/>**SaveGameFileTest** &mdash; `WriteAndReadHeader`, `WriteAndReadMetadata`, `WriteAndReadThumbnail`, `WriteAndReadPayload`, `ChecksumValidation`, `ReadNonExistentFile`, `EmptyThumbnail`, `OverwriteInPlaceRetainsLatestData` |
| [SaveGameIntegrationTest.cpp](../OloEngine/tests/SaveGame/SaveGameIntegrationTest.cpp) | 5 | **SaveGameIntegrationTest** &mdash; `CaptureAndRestoreEmptyScene`, `CaptureAndRestoreWithEntities`, `FullFileRoundTrip`, `EmptyPayloadReturnsError`, `CorruptedDataReturnsError` |
| [SaveGameManagerTest.cpp](../OloEngine/tests/SaveGame/SaveGameManagerTest.cpp) | 4 | **SaveGameManagerTest** &mdash; `AutoSaveInterval`<br/>**SaveableRegistryTest** &mdash; `RegisterAndInvoke`, `InvokeNonExistent`, `RoundTripWithData` |

#### Serialization (7 files)

| File | Tests | Cases |
|---|---:|---|
| [ArchiveExtensionsTest.cpp](../OloEngine/tests/Serialization/ArchiveExtensionsTest.cpp) | 24 | **ArchiveExtensionsTest** &mdash; `Vec2Roundtrip`, `Vec3Roundtrip`, `Vec4Roundtrip`, `IVec3Roundtrip`, `Mat3Roundtrip`, `Mat4Roundtrip`, `QuatRoundtrip`, `UUIDRoundtrip`, `UUIDZeroRoundtrip`, `VectorOfFloatsRoundtrip`, `EmptyVectorRoundtrip`, `VectorOfVec3Roundtrip`, `VectorOfStringsRoundtrip`, `UnorderedMapRoundtrip`, `EmptyMapRoundtrip`, `VectorOfUUIDsRoundtrip`, `Vec3ProducesExpectedSize`, `Mat4ProducesExpectedSize`, `TruncatedVectorFailsKeepsDestination`, `OversizeCountFailsKeepsDestination`, `DuplicateKeyMapFailsKeepsDestination`, `TruncatedMapFailsKeepsDestination`, `TruncatedVectorCountFailsKeepsDestination`, `TruncatedMapCountFailsKeepsDestination` |
| [EnvironmentSerializerTest.cpp](../OloEngine/tests/Serialization/EnvironmentSerializerTest.cpp) | 2 | **EnvironmentSerializerTest** &mdash; `TryLoadDataPopulatesSpecFilePath`, `TryLoadDataFailsWhenSourceMissing` |
| [FontAssetPackSerializerTest.cpp](../OloEngine/tests/Serialization/FontAssetPackSerializerTest.cpp) | 3 | **FontMemoryLoadTest** &mdash; `MemoryLoadMatchesFileLoad`, `SanitizesOutOfRangeCodepoints`<br/>**FontAssetPackSerializerTest** &mdash; `AssetPackRoundTrip` |
| [MeshAssetSerializerTest.cpp](../OloEngine/tests/Serialization/MeshAssetSerializerTest.cpp) | 3 | **MeshAssetSerializerYAMLTest** &mdash; `RoundTripsMeshSourceHandleAndSubmeshIndex`, `FailsWhenMeshSourceHandleIsMissing`, `ClampsOutOfRangeSubmeshIndexInsteadOfAsserting` |
| [MeshBinarySerializerTest.cpp](../OloEngine/tests/Serialization/MeshBinarySerializerTest.cpp) | 8 | **MeshBinarySerializerTest** &mdash; `WriteAndReadStaticMesh`, `ReadTimestampWorks`, `ReadTimestampFailsOnMissingFile`, `WriteAndReadRiggedMesh`, `ReadRejectsCorruptFile`<br/>**AnimationBinarySerializerTest** &mdash; `WriteAndReadRoundTrip`, `ReadTimestampWorks`, `ReadReturnsEmptyOnCorruptFile` |
| [SoundConfigSerializerTest.cpp](../OloEngine/tests/Serialization/SoundConfigSerializerTest.cpp) | 6 | **SoundConfigSerializerTest** &mdash; `DiskRoundTripPreservesEveryField`, `TryLoadDataFailsWhenSourceMissing`<br/>**SoundConfigSerializerYAML** &mdash; `RoundTripPreservesEveryField`, `MissingRootNodeFails`, `NonFiniteFloatsFallBackToDefaults`, `OutOfRangeValuesAreClamped` |
| [SoundGraphSerializerTest.cpp](../OloEngine/tests/Serialization/SoundGraphSerializerTest.cpp) | 2 | **SoundGraphSerializer** &mdash; `RoundTripPreservesNodesConnectionsAndGraphIO`, `EmptyGraphRoundTripsWithoutData` |

#### Tasks (1 file)

| File | Tests | Cases |
|---|---:|---|
| [TaskSystemTest.cpp](../OloEngine/tests/Tasks/TaskSystemTest.cpp) | 58 | **TaskSystemTest** &mdash; `FireAndForgetTask`, `LaunchAndWait`, `TaskWithResult`, `TaskWithResultPostponed`, `WaitForCompletion`, `MutableLambda`, `FreeTaskMemory`, `WaitingForMultipleTasks`<br/>**TaskEventTest** &mdash; `BasicTrigger`, `MultipleTriggersAllowed`, `BlocksUntilTriggered`, `TaskAsPrerequisite`, `EmptyPrerequisite`<br/>**NestedTasksTest** &mdash; `SingleNestedTask`, `NestedTaskCompletedDuringParent`, `MultipleNestedTasks`<br/>**PipeTest** &mdash; `BasicPipeUsage`, `SequentialExecution`, `MultipleTasksAfterCompletion`, `PipeWithPrerequisites`, `WaitUntilEmpty`, `WaitUntilEmptyWithWork`, `WaitUntilEmptyWithPrereq`<br/>**TaskDependenciesTest** &mdash; `SinglePrerequisite`, `MultiplePrerequisites`, `PipedTaskWithPrerequisite`<br/>**TaskStressTest** &mdash; `ManyTasks`, `NestedSpawning`, `PipeStress`<br/>**MakeCompletedTaskTest** &mdash; `BasicCompletedTask`, `MoveOnlyResult`<br/>**IsAwaitableTest** &mdash; `BasicIsAwaitable`<br/>**WaitAnyTest** &mdash; `BlocksIfNoneCompleted`, `DoesNotWaitForAllTasks`<br/>**AnyTest** &mdash; `BlocksIfNoneCompleted`, `DoesNotWaitForAllTasks`<br/>**TaskConcurrencyLimiterTest** &mdash; `BasicConcurrencyLimit`, `MultipleProducers`, `SlotsDoNotOverlap`<br/>**DeepRetractionTest** &mdash; `TwoLevelsDeep`<br/>**InlineTaskTest** &mdash; `InlineExecution`<br/>**MoveOnlyResultTest** &mdash; `UniquePtr`, `MoveConstructableOnly`<br/>**TaskSelfAccessTest** &mdash; `AccessTaskDuringExecution`<br/>**NestedTaskStressTest** &mdash; `ManyNestedTasks`<br/>**ConcurrentEventTriggerTest** &mdash; `TriggerFromMultipleThreads`<br/>**LowLevelTaskUserDataTest** &mdash; `SetUserDataBeforeLaunch`, `SetUserDataWithSharedTask`, `SetUserDataWithQueuedTask`, `ConcurrencyLimiterSimulation`, `SimpleFTaskConcurrencyLimiterTest`, `MultipleFTaskConcurrencyLimiterTest`<br/>**CancellationTokenTest** &mdash; `BasicCancellation`, `MultipleTasks`<br/>**WorkerRestartTest** &mdash; `LoneStandbyWorker`, `RestartWorkersAndOversubscription`, `DISABLED_RestartWorkersAndExternalThreads`, `BackgroundWithNormalAsPrereq` |

#### Templates (2 files)

| File | Tests | Cases |
|---|---:|---|
| [FunctionWithContextTest.cpp](../OloEngine/tests/Templates/FunctionWithContextTest.cpp) | 4 | **FunctionWithContext** &mdash; `DefaultConstructedIsNullAndExposesNullSlots`, `LambdaBoundCallableInvokesCaptureBody`, `ReassignmentReplacesBoundCallable`, `RoundTripsThroughStatelessInvocationAPI` |
| [TypeTraitsTest.cpp](../OloEngine/tests/Templates/TypeTraitsTest.cpp) | 2 | **TypeTraitsTest** &mdash; `AllChecksAreCompileTime`, `AllNameOfsAreCorrect` |

#### Terrain (1 file)

| File | Tests | Cases |
|---|---:|---|
| [TerrainGeneratorTest.cpp](../OloEngine/tests/Terrain/TerrainGeneratorTest.cpp) | 15 | **TerrainGeneratorTest** &mdash; `HeightFieldIsDeterministic`, `HeightFieldIsNormalizedAndFinite`, `LargeSeedStillProducesVariedTerrain`, `DifferentSeedsProduceDifferentTerrain`, `RidgedAndWarpAndExponentStayValid`, `TerraceShapingStaysValidAndDeterministic`, `TerraceEndpointsAndIdentity`, `TerraceIsMonotonicAndBounded`, `TerraceProducesFlatPlateaus`, `RuleWeightPeaksInsideBandAndZeroOutside`, `SlopeBandSelectsRule`, `DefaultRulesAssignExpectedLayers`, `LayerWeightsAreNormalized`, `NoMatchingRuleFallsBackToLayerZero`, `PackLayerWeightsQuantizesToBothSplatmaps` |

**Totals.** 150 unit / subsystem test files, 1844 TEST / TEST_F declarations across all subsystems.

<!-- END: unit-catalogue -->

---

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
