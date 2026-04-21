---
description: "Renderer & engine testing architecture for OloEngine. Apply when writing, editing, or reviewing test code, adding new engine features that need tests, or modifying test infrastructure. Covers the 11-layer pyramid, the auto-catalogue registration contract, and where new tests belong."
applyTo: "OloEngine/tests/**/*.cpp, OloEngine/tests/**/*.h, OloEngine/tests/scripts/**/*.py, OloEngine/tests/scripts/**/*.json"
---

# Testing Architecture

Authoritative reference: [docs/renderer-testing.md](../../docs/renderer-testing.md).
Read that doc for the "why" behind each layer. This file captures the
operational rules that must be followed every time a test is added, moved,
renamed, or deleted.

---

## 1. The 11-layer pyramid — pick a layer before writing code

Every new test belongs to exactly one layer. If you cannot identify a layer
for a proposed test, you are probably writing a symptom check rather than a
contract check — stop and design the contract first.

| Layer | Catches | Goes in |
|---|---|---|
| L1 Property / behavioural | math / physics contracts, monotonicity, energy bounds | `OloEngine/tests/Rendering/PropertyTests/*PropertyTests.cpp` |
| L2 Shader unit | GLSL helper correctness via compute-shader readback | `OloEngine/tests/Rendering/PropertyTests/ShaderUnitTests.cpp` + `OloEditor/assets/shaders/tests/` |
| L3 Data round-trip | CPU↔GPU bit/byte identity, cache round-trips | `OloEngine/tests/Rendering/PropertyTests/DataRoundTripTests.cpp` |
| L4 GPU state validation | blend/depth/stencil/FBO/UBO leaks across passes | `OloEngine/tests/Rendering/PropertyTests/GLStateGuardTest.cpp`, `RenderStateTest.cpp` |
| L5 Render graph / hazards | pass ordering, read-after-write, parallel-write | `OloEngine/tests/Rendering/RenderGraphTest.cpp`, `ResourceHazardValidationTests.cpp` |
| L6 Performance regression | per-pass timing vs. pinned baseline with anti-flake | `OloEngine/tests/Rendering/PropertyTests/PerfRegressionTests.cpp` + `perf_baselines.txt` |
| L7 Smoke / sanity readback | NaN/Inf detection, empty/clean framebuffers | `OloEngine/tests/Rendering/PropertyTests/RendererValidateTest.cpp` |
| L8 Golden image | SSIM cascade over scene fixtures | `OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp` + `OloEditor/assets/goldens/` |
| L9 Cross-vendor | llvmpipe conformance in CI (no code — workflow only) | `.github/workflows/cross-vendor.yml` |
| L10 Diagnostic escalation | auto-capture framebuffer + metadata on `ASSERT_` failure | `TestFailureCapture.{h,cpp}` (consumed automatically) |
| L11 Sanitizers & fuzzing | libFuzzer harnesses + ASan/UBSan in CI | `OloEngine/tests/Fuzzing/Fuzz*.cpp` + `.github/workflows/fuzz.yml` |

Files outside `PropertyTests/` are also classified — see the auto-catalogue
in `docs/renderer-testing.md` §3.12 for the full inventory.

---

## 2. The registration contract (NON-NEGOTIABLE)

Every `.cpp` under the scan roots in
[`OloEngine/tests/scripts/test_catalogue.json`](../../OloEngine/tests/scripts/test_catalogue.json)
must appear in `file_layer_map` (with a layer id) or `exclude` (helpers /
fixtures only). The `test-catalogue-in-sync` pre-commit hook will **fail
the commit** otherwise.

Scan roots (recursive):

- `OloEngine/tests/Rendering/`
- `OloEngine/tests/ShaderGraph/`
- `OloEngine/tests/Streaming/`

Plus any file explicitly listed under `extra_roots`.

**Workflow when adding a test file:**

1. Write the test(s). Use `TEST`, `TEST_F`, `TEST_P`, or `TYPED_TEST` — the
   scanner's regex relies on these macros.
2. Register the file in `test_catalogue.json` → `file_layer_map` with the
   correct layer id.
3. Run `python OloEngine/tests/scripts/generate_test_catalogue.py` to
   regenerate the auto-catalogue block in `docs/renderer-testing.md`.
4. Commit the .cpp, the JSON entry, and the doc diff together.

**Do not hand-edit** the block between
`<!-- BEGIN: auto-catalogue -->` and `<!-- END: auto-catalogue -->` in
`docs/renderer-testing.md`. The generator overwrites it.

---

## 3. When adding a new engine feature, add tests in lockstep

Every feature PR should land with tests at the layers dictated by the
feature's surface area. Use this heuristic:

- **Has a mathematical / physical contract** (energy, monotonicity,
  identity, commutativity) → **L1 property test** is mandatory.
- **Adds a GLSL helper function** (`Srgb*`, `ToneMap*`, geometry math) →
  **L2 shader unit test** via `ShaderUnit_*.glsl` compute dispatch.
- **Introduces a new CPU↔GPU data type or serialized format** →
  **L3 round-trip test**.
- **Binds/unbinds GL state** (blend, depth, stencil, FBO, UBO, texture) →
  **L4 GLStateGuard region** wrapping the pass.
- **Adds a render-graph pass, resource, or dependency edge** →
  **L5 hazard-validation case**.
- **Is on the hot path** (per-frame, per-pass) → **L6 perf bench** with a
  baseline entry in `perf_baselines.txt`. Use the anti-flake retry helper.
- **Produces visible pixels** for a scene the user can point at →
  **L8 golden** with SSIM threshold. Only after L1–L4 already pin the
  contract; never use goldens as the primary correctness check.
- **Parses external data** (file, socket, YAML, SPIR-V, image) →
  **L11 fuzz harness** + seed corpus under `OloEngine/tests/Fuzzing/corpus/`.

If a feature touches multiple surfaces, write one test per surface.
Do not collapse them into a single mega-test — the layer taxonomy is how
failures get diagnosed.

---

## 4. Anti-patterns to reject

- **Golden-image-only coverage**: goldens catch *symptoms*. If the only
  test for a feature is an L8 golden, the PR is under-tested. Every
  feature needs at least one contract-level test (L1–L5).
- **Hand-written expected images / numeric constants with no citation**:
  every "expected" value in L1/L2 must either be derived in the test
  itself or carry a comment linking to the reference (paper, spec,
  Wikipedia formula, GLSL source line).
- **Flaky timing assertions**: L6 baselines must use the anti-flake
  retry helper and record the measurement on failure. Never use raw
  `EXPECT_LT(elapsed_ns, constant)` — go through the baseline path.
- **Tests that require a specific GPU vendor**: anything beyond llvmpipe
  belongs in CI matrix work, not in the default test binary.
- **Unregistered test files**: see §2 — the pre-commit hook enforces this.

---

## 5. Running tests locally

- **Full suite:** VS Code task `run-tests-debug` (builds then runs).
- **Single test:**
  `./build/OloEngine/tests/Debug/OloEngine-Tests.exe --gtest_filter=SuiteName.TestName`
- **Fuzz smoke:** build target `OloEngine-FuzzSmoke`, then run each
  harness with `-runs=N -max_total_time=T`.
- **Golden rebase** (only when a deliberate visual change lands):
  `OLOENGINE_GOLDEN_REBASE=1 <test binary> --gtest_filter=GoldenImage*`.
- **Perf rebase** (only when moving to new hardware or after an
  intentional optimisation):
  `OLOENGINE_PERF_REBASE=1 <test binary> --gtest_filter=PerfRegression*`.

Working directory matters: run from the repo root so asset paths resolve.

---

## 6. Relevant files you will touch

| Purpose | Path |
|---|---|
| Authoritative doc | `docs/renderer-testing.md` |
| Layer registration config | `OloEngine/tests/scripts/test_catalogue.json` |
| Auto-catalogue generator | `OloEngine/tests/scripts/generate_test_catalogue.py` |
| Perf baselines | `OloEngine/tests/Rendering/PropertyTests/perf_baselines.txt` |
| Perf history + trend tool | `OloEngine/tests/Rendering/PropertyTests/perf_history/`, `OloEngine/tests/scripts/perf_trend.py` |
| Fuzz corpus | `OloEngine/tests/Fuzzing/corpus/` |
| CI workflows | `.github/workflows/{cross-vendor,fuzz}.yml` |

If you are editing any of these, the pre-commit hook will revalidate.
