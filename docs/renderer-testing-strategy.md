# Renderer Testing Strategy

A specification of testing approaches for real-time rendering engines, covering what each approach tests, its core concepts, trade-offs, and how major engines use them.

---

## 1. Golden Image / Screenshot Comparison Testing

### Core Concept

Render a deterministic scene to an offscreen framebuffer, read back pixels to CPU memory, and compare the result against a previously approved reference image. Differences exceeding a threshold indicate a regression.

### What It Tests

- **End-to-end visual correctness** — the final pixel output is the ultimate truth. Any bug in the pipeline (shaders, state management, resource binding, post-processing, tone mapping) that changes what the user sees will be caught.
- **Regression detection** — new code that subtly changes lighting, shadow quality, anti-aliasing, or color grading is immediately flagged.
- **Multi-frame consistency** — loading a scene twice, or rendering frame N vs frame N+100, should produce identical (or near-identical) output.

### Key Design Decisions

| Decision | Options | Trade-offs |
|---|---|---|
| **Comparison metric** | Per-pixel RMSE, peak absolute delta, SSIM, perceptual diff (pdiff) | RMSE is simple but sensitive to single-pixel outliers. SSIM better matches human perception. Perceptual diff (Yee's algorithm) is best but slowest. |
| **Threshold strategy** | Global RMSE threshold, per-pixel max delta, percentage of pixels allowed to differ | Too tight → flaky tests from driver/platform variance. Too loose → misses real bugs. Typical: RMSE < 0.005 in [0,1] space, or < 1% of pixels differing by > 2/255. |
| **Reference image management** | Checked into VCS, stored in LFS, generated on-demand | Must be regenerated explicitly after intentional visual changes. Per-platform references handle driver differences. |
| **Determinism** | Fixed camera, fixed resolution, fixed seed for any randomness, disabled temporal effects | Temporal AA, jittered sampling, and animation must be disabled or seeded for reproducibility. |
| **Headless rendering** | Hidden window, EGL offscreen context, software rasterizer | Hidden window is simplest but needs a display server. EGL is truly headless. Software rasterizer (Mesa llvmpipe) works in Docker/CI but produces different results from hardware. |

### Variant: Multi-Stage Comparison Pipeline

A cascaded approach that uses cheap metrics as a fast-path filter and only escalates to expensive perceptual metrics when the result is ambiguous:

1. **RMSE (fast)** — compute per-pixel RMSE across the whole image.
   - Below tight threshold (e.g., < 0.002): **pass immediately**. No further analysis needed.
   - Above high threshold (e.g., > 0.02): **fail immediately**. Clearly broken.
   - Between the two: **ambiguous** — escalate to stage 2.
2. **SSIM or FLIP (expensive)** — run a perceptual metric on the ambiguous image.
   - SSIM > 0.98: **pass** — differences are imperceptible despite elevated RMSE (common cause: a single specular highlight shifting by 1 pixel).
   - SSIM ≤ 0.98: **fail** — perceptually visible regression.

**Why cascade?** RMSE and perceptual metrics fail in complementary ways:
- RMSE is **oversensitive** to isolated bright-pixel outliers (a specular highlight moving 1 pixel produces high RMSE despite being visually identical).
- RMSE is **undersensitive** to distributed low-contrast shifts (a subtle color cast across the whole image has low RMSE but is clearly visible).
- SSIM/FLIP handle both cases correctly but cost 5–10× more to compute.

In practice, ~90% of test runs resolve at stage 1 (clear pass or clear fail), so the expensive metric rarely runs.

### Variant: Differential Testing

Instead of comparing against a static reference, render the same scene through two code paths (e.g., cached vs fresh, old branch vs new branch) and compare the outputs. No reference images needed — the two renders are compared against each other.

### Industry Usage

- **Unreal Engine**: `FAutomationTestBase` + screenshot comparison framework, per-platform golden images, dedicated "Screenshot Comparison" tool.
- **Unity**: Separate Graphics Tests repository with hundreds of scene-based screenshot tests per render pipeline (URP, HDRP, Built-in).
- **Godot**: Screenshot-based tests in `tests/servers/rendering/`, integrated with CI.
- **Filament**: `test_Filament` renders to headless SwapChain → compares against committed PNGs.
- **Bevy**: `screenshot_test!` macro, runs across wgpu backends.

### Limitations

- **Brittle across GPU vendors** — AMD, NVIDIA, and Intel produce slightly different rounding for the same shader. Requires per-vendor references or generous thresholds.
- **Slow** — each test requires full scene load + render + readback. Typically 0.5–2s per test.
- **Binary pass/fail** — when a test fails, you see *that* something changed but not *why*. Needs complementary diagnostics (frame capture, state dumps).

---

## 2. GPU State Validation / Sanitization

### Core Concept

At the boundaries of rendering passes (shadow, scene, IBL, post-process), capture a snapshot of critical GPU state (depth test, depth mask, blend mode, bound textures, bound UBOs, viewport, scissor, stencil). On scope exit, assert that state was either properly restored or explicitly set to the expected values.

### What It Tests

- **State leakage between passes** — a shadow pass disabling depth writes and forgetting to re-enable them, a post-process pass leaving blend enabled, an IBL pass leaving its UBO bound.
- **Implicit assumptions** — code that assumes "depth test is enabled here" without checking.
- **Driver-silent failures** — some state combinations are technically valid but produce wrong results (e.g., clearing depth with depth mask disabled silently no-ops).

### Key Design Decisions

| Decision | Options |
|---|---|
| **Granularity** | Per-pass boundaries, per-draw-call, or entry/exit of the entire frame |
| **Tracked states** | Depth test/mask, blend mode/factors, stencil, viewport/scissor, bound FBO, bound textures per slot, bound UBOs per slot, active shader, face culling |
| **Enforcement mode** | Assert-and-crash (debug builds), log-and-continue, or silent-restore |
| **Debug-only vs always-on** | Typically debug/development builds only — `glGet*` queries are slow |

### Approaches

**Approach A: RAII State Guards**
An RAII object snapshots state on construction and asserts/restores on destruction. Place at pass boundaries. Lightweight, easy to adopt incrementally.

**Approach B: Explicit State Machine / Tracked State**
Maintain a CPU-side mirror of all GPU state. Every state-changing call goes through the mirror, which validates transitions. Like a "software driver" sitting above the real driver. More comprehensive but higher maintenance cost.

**Approach C: GL Debug Callbacks + Custom Validators**
Use `glDebugMessageCallback` (already standard in GL 4.3+) for driver-reported issues, augmented with custom validators that `glGet*` state at pass boundaries. Combines driver intelligence with application-specific rules.

### Industry Usage

- **Filament**: Internal `DriverBase` tracks all state, validates transitions, asserts on illegal combinations.
- **bgfx**: CPU-side state mirror (the "encoder") that validates before submission.
- **WebGPU/Dawn**: Validation layer that rejects invalid state transitions before they reach the driver.
- **Vulkan Validation Layers**: The gold standard — external layer that intercepts every API call and validates correctness. No GL equivalent exists, so engines must roll their own.

### Limitations

- **Performance overhead** — `glGet*` calls stall the pipeline. Must be debug-only.
- **Incomplete coverage** — you can only validate states you thought to track.
- **Not visual** — validates *correctness of state* but not *correctness of output*. A shader bug producing wrong colors won't be caught.

---

## 3. Data Round-Trip / Serialization Tests

### Core Concept

Generate rendering data (textures, cubemaps, meshes, shader binaries, material parameters), serialize to the engine's cache format, deserialize back, and compare with the original. Verifies that the cache pipeline preserves data integrity.

### What It Tests

- **Format correctness** — writing RGBA32F data but reading it back as RGB32F, wrong bytes-per-pixel calculations, endianness issues.
- **Mip chain integrity** — all mip levels are saved and restored, not just mip 0.
- **Lossy vs lossless encoding** — if the cache uses compression (meshopt, BC compression, etc.), verify the error stays within acceptable bounds.
- **Header/metadata correctness** — dimensions, format enums, mip counts, face counts match the actual data.
- **Compatibility across versions** — cache files from version N can be loaded by version N+1 (or are correctly rejected).

### Key Design Decisions

| Decision | Options |
|---|---|
| **Comparison type** | Byte-exact, floating-point epsilon, RMSE within tolerance |
| **Data generation** | Procedural (deterministic patterns like gradients, checkerboards), or real assets |
| **Scope** | Just the cache layer, or full GPU upload → readback → cache → reload → readback chain |

### Test Patterns

- **Identity round-trip**: `data → serialize → deserialize → compare`. Pure CPU test, fast, no GPU needed.
- **GPU round-trip**: `data → upload to GPU → readback → compare`. Tests the upload/readback path independently of caching.
- **Full pipeline round-trip**: `data → GPU render → cache save → cache load → GPU upload → readback → compare`. Tests everything.

### Industry Usage

- **Assimp**: Extensive import → export → re-import tests for mesh formats.
- **KTX (Khronos Texture)**: Round-trip tests for every texture format, including mip chains and cubemap faces.
- **USD (Pixar)**: Serialization conformance suite that verifies data survives write → read cycles.

### Limitations

- **Only tests the data path** — doesn't verify that the data is *used correctly* by the renderer.
- **Doesn't catch "renders differently"** — data can round-trip perfectly but be bound to the wrong texture slot.

---

## 4. Smoke / Sanity Readback Tests

### Core Concept

After key rendering operations, read back a small sample of pixels (or statistics like min/max/average) and assert basic invariants: non-zero, no NaN, no Inf, within expected HDR range, alpha = 1.0, etc. Not comparing against a reference — just checking that the output isn't catastrophically wrong.

### What It Tests

- **Catastrophic failures** — entire pass rendered black, NaN propagation through the pipeline, infinite values from division by zero, alpha channel corruption.
- **Energy conservation** — rougher mip levels of a prefilter map should have less energy than smoother ones. IBL irradiance should be within a reasonable range of the environment map.
- **Format correctness (coarse)** — if you expect HDR values > 1.0 but read back clamped [0,1], the texture is probably in the wrong format.

### Key Design Decisions

| Decision | Options |
|---|---|
| **Sample strategy** | Center pixel only, corners + center, full-face average, random sampling |
| **Invariants** | Non-zero, non-NaN, non-Inf, within [min, max] range, monotonicity across mip levels |
| **Runtime vs test-time** | Can run as both — lightweight enough for runtime diagnostics in debug builds |

### Industry Usage

- **Filament**: `test_Filament` includes basic "output is not black" checks alongside golden image tests.
- **Three.js**: WebGL test suite renders simple scenes and checks for non-zero output via `readPixels`.
- **Most engines**: Informal — developers add `assert(pixel != black)` during debugging and sometimes leave them in. Rarely formalized into a test suite.

### Limitations

- **Very coarse** — "not black and not NaN" is a very low bar. Many real bugs produce non-black, non-NaN output that is still visually wrong.
- **Not a substitute for golden images** — a complement, not a replacement. Best used as a fast first-pass filter.

---

## 5. Shader Unit Testing

### Core Concept

Test individual shader functions in isolation by running them on small synthetic inputs and verifying outputs. Either by compute shader dispatch (GPU-side) or by transpiling GLSL to C++ and running on CPU.

### What It Tests

- **Mathematical correctness** — PBR equations (GGX NDF, Fresnel-Schlick, Smith geometry), tone mapping operators (Reinhard, ACES), color space conversions (sRGB ↔ linear), normal mapping math.
- **Edge cases** — roughness = 0 or 1, metallic = 0 or 1, NdotL = 0, grazing angles, degenerate normals.
- **Numerical stability** — division by zero guards, clamping, avoiding log(0) or sqrt(negative).

### Approaches

**Approach A: Compute Shader Harness**
Write a compute shader that calls the function under test with known inputs, writes output to an SSBO, read back on CPU, and compare. Requires a GL context but tests the actual compiled shader code.

**Approach B: GLSL-to-C++ Transpilation**
Tools like `glslang` or custom preprocessor macros can make GLSL functions compilable as C++. Run on CPU with standard unit test frameworks. Fast but may miss driver-specific behavior.

**Approach C: Shared Include Files**
Write core math in `.glsl` include files. Test the CPU-translated version. Use `#ifdef __cplusplus` guards for dual-compilation.

### Industry Usage

- **Unreal**: Shader permutation tests validate that all material permutations compile successfully. Limited runtime verification.
- **Filament**: Core BRDF functions implemented in both C++ and GLSL with shared test vectors.
- **Unity HDRP**: Dedicated shader function tests in the Graphics Tests repo.
- **Shadertoy / IQ**: Influential practice of validating math functions visually using known test scenes (Cornell box, furnace test).

### Specialized Shader Tests

- **Furnace test**: Render a white sphere inside a uniform white environment. A correct energy-conserving BRDF should produce a perfectly white pixel at all angles and roughness values. Any deviation indicates energy loss or gain.
- **White environment test**: Similar to furnace, but tests the full IBL pipeline. Irradiance of a uniform white environment should be π (for Lambertian diffuse).
- **Normal map identity test**: A flat normal map (128, 128, 255) should produce identical output to no normal map at all.

### Limitations

- **GPU compilation differences** — shader behavior can differ between NVIDIA, AMD, Intel due to precision, instruction reordering, and fast-math optimizations.
- **Combinatorial explosion** — engines often have thousands of shader permutations. Testing all is impractical.

---

## 6. Render Graph / Command Sequence Validation

### Core Concept

Capture the sequence of rendering commands (draw calls, state changes, resource barriers, pass transitions) for a frame, and validate structural properties: correct ordering, no missing barriers, no redundant state changes, correct resource lifetimes.

### What It Tests

- **Pass ordering** — shadow passes execute before the main scene pass, post-processing after, IBL generation before anything that reads IBL.
- **Resource hazards** — a texture written by pass A must not be read by pass B without a barrier/sync in between.
- **Redundant state** — binding the same shader or texture twice in a row wastes GPU time.
- **Missing resources** — a draw command references a texture ID of 0 (null/uninitialized).
- **Command budget** — frame renders within expected draw call / state change budget.

### Approaches

**Approach A: Frame Capture + Offline Analysis**
Record an entire frame's commands to a structured log (JSON, binary). Analyze offline with custom validators. Can diff between two captures to find what changed.

**Approach B: Render Graph Static Analysis**
If the engine uses a render graph (Vulkan-style), validate the graph structure before execution: check for cycles, verify resource lifetimes, detect unused passes, validate format compatibility.

**Approach C: Runtime Command Stream Validation**
Intercept commands at submission time and validate in real-time. Like a "linter" for your command stream.

### Industry Usage

- **Vulkan Validation Layers**: Validates resource barriers, pipeline hazards, descriptor set bindings.
- **RenderDoc**: Records frame → allows inspection of every API call, resource state, and pipeline stage.
- **Unreal RDG (Render Dependency Graph)**: Static validation of the render graph — detects missing transitions, unused resources, and aliasing conflicts at build time.
- **bgfx**: Submission-time validation of draw state (e.g., "texture slot 3 is unbound but shader expects it").

### Limitations

- **Structural, not visual** — a perfectly valid command sequence can produce wrong pixels.
- **Complex to implement** — requires either a render graph abstraction or comprehensive command interception.

---

## 7. Performance Regression Testing

### Core Concept

Measure frame time, draw call count, state change count, GPU memory usage, and other metrics across known scenes. Compare against a baseline to detect unintentional performance regressions.

### What It Tests

- **Draw call count regression** — a change that disables batching might double draw calls.
- **State change explosion** — a sorting change that breaks material coherence.
- **GPU memory leaks** — textures or buffers not freed across scene loads.
- **Frame time regression** — detected via wall-clock or GPU timestamp queries.

### Key Design Decisions

| Decision | Options |
|---|---|
| **Metric collection** | CPU-side counters, GPU timestamp queries, driver performance counters (vendor-specific) |
| **Baseline management** | Checked-in JSON with expected ranges, or relative comparison (this run vs previous run) |
| **Noise handling** | Run N frames, discard first M (warmup), report median. Thermal throttling and background processes add noise. |
| **Threshold** | Typically 5–10% regression triggers a warning, >20% fails the test |

### Industry Usage

- **Unreal**: Stat system + automated Gauntlet tests that track GPU/CPU timings across maps.
- **Unity**: Performance test framework with statistical analysis and historical tracking.
- **id Tech (Doom)**: Internal benchmarks with fixed camera paths and strict frame time budgets.
- **Chromium (Skia/Dawn)**: `perf_tests` with historical tracking and bisection support.

### Limitations

- **Noisy** — GPU performance varies with thermal state, driver version, Windows updates, background load.
- **Platform-specific** — a regression on NVIDIA may not appear on AMD.
- **Doesn't catch visual bugs** — a renderer that skips half its draw calls will run faster and pass perf tests while looking completely wrong.

---

## 8. Cross-API / Cross-Platform Conformance

### Core Concept

Run the same test suite across multiple rendering backends (OpenGL, Vulkan, DirectX, Metal) and/or multiple GPU vendors, comparing results for consistency. Verifies that the abstraction layer (RHI) produces equivalent output everywhere.

### What It Tests

- **Backend parity** — same scene rendered by Vulkan and OpenGL backends should produce near-identical output.
- **Vendor differences** — NVIDIA's fast-math vs AMD's stricter IEEE compliance, Intel's different texture filtering.
- **Precision differences** — mediump vs highp on mobile, denormalized float handling.
- **Format support** — not all formats are available on all backends (e.g., RGB32F is optional in GL).

### Industry Usage

- **Unreal**: Per-platform golden images, cross-API parity tests.
- **Filament**: Tests run across Vulkan, OpenGL, Metal backends.
- **WebGPU CTS (Conformance Test Suite)**: Thousands of per-feature tests run across Dawn (Chrome), wgpu (Firefox), and all GPU vendors.
- **Vulkan CTS (dEQP)**: Khronos-maintained conformance suite with ~500k tests.

### Limitations

- **Expensive** — requires hardware for every target platform, or cloud GPU CI.
- **Not always achievable** — single-backend engines (like an OpenGL-only engine) can only test cross-vendor, not cross-API.

---

## 9. Automatic Diagnostic Escalation

### Core Concept

When a test fails, don't just report "FAIL" — automatically re-run the failing test with progressively more diagnostics enabled, collecting the information a developer would manually gather during debugging. The first run is fast and cheap (normal assertions). The second run is slow and expensive (full diagnostics). Only the failing tests pay the diagnostic cost.

### Escalation Stages

| Stage | Trigger | What it collects | Cost |
|---|---|---|---|
| **Stage 0: Normal run** | Always | Pass/fail result, basic assertion message | Baseline |
| **Stage 1: Readback diagnostics** | Stage 0 fails | Pixel readback at key pipeline stages (after scene pass, after post-process, after tone map), min/max/avg statistics, NaN/Inf scan | ~2× slower (GPU readback stalls) |
| **Stage 2: State snapshots** | Stage 0 fails | Full GL state dump at pass boundaries (bound FBO, UBOs, textures, depth/blend/stencil config, viewport), diff against expected state | ~3× slower (`glGet*` queries) |
| **Stage 3: Frame capture** | Stage 0 fails | Full command stream log (every draw call, state change, resource bind), diff against baseline capture if available | ~5× slower (command recording overhead) |
| **Stage 4: Visual diff** | Golden image fails | Side-by-side diff image (reference vs actual vs heatmap of differences), per-channel breakdown, histogram of deltas | ~1.5× slower (image processing) |

Stages 1–4 are independent and can run in parallel during a single diagnostic re-run.

### What It Solves

- **"Test failed, now what?"** — the most common developer experience with rendering tests is staring at a red test and having no idea what changed. Automatic diagnostics bridge the gap between "something is wrong" and "here's what's wrong."
- **Non-reproducible failures** — some failures only occur under specific conditions (warm cache, second scene load, specific entity order). Re-running with diagnostics captures the failure in context rather than asking the developer to reproduce it manually.
- **CI without RenderDoc** — on CI machines, interactive GPU debuggers aren't available. The diagnostic re-run acts as a lightweight substitute, collecting the same information a developer would get from a RenderDoc capture.

### Key Design Decisions

| Decision | Options | Trade-offs |
|---|---|---|
| **Re-run strategy** | Re-run only failing tests, re-run entire suite with diagnostics, or always collect diagnostics | Re-run failing only is fastest. Always-on diagnostics is simplest but wastes time on passing tests. |
| **Diagnostic output format** | Structured JSON (machine-parseable), human-readable log, or both | JSON enables tooling (auto-bisection, dashboards). Human-readable is essential for quick debugging. Emit both. |
| **Artifact storage** | CI artifact upload, local temp directory, or committed alongside test results | CI artifacts are ephemeral but cheap. Local temp for development. Never commit diagnostic output. |
| **Diagnostic depth** | Fixed (always run all stages) or adaptive (stop after first informative stage) | Adaptive is faster but may miss correlated issues. Fixed is simpler and more thorough. |
| **Diagnostic instrumentation** | Compile-time flags (`#ifdef DIAGNOSTIC_MODE`), runtime flags (env var or test fixture parameter), or always-present but gated by cost | Runtime flags are most flexible — same binary can run normal or diagnostic mode. |

### Diagnostic Report Structure

A failing test should produce a self-contained diagnostic bundle:

```text
test-output/
└── IBL_ReloadConsistency_FAIL/
    ├── summary.json              # Machine-readable: which stages ran, what they found
    ├── assertion.txt             # Original failure message
    ├── readback/
    │   ├── scene_pass_face0.exr  # HDR pixel data after scene pass
    │   ├── postprocess_out.exr   # After post-processing
    │   └── statistics.json       # min/max/avg/NaN count per stage
    ├── state/
    │   ├── pass_entry_state.json # GL state at each pass entry
    │   ├── pass_exit_state.json  # GL state at each pass exit
    │   └── state_diff.txt        # Unexpected state changes highlighted
    ├── capture/
    │   ├── commands.json         # Full command stream
    │   └── baseline_diff.txt     # Diff against last known-good capture
    └── visual/
        ├── reference.png
        ├── actual.png
        └── diff_heatmap.png      # Per-pixel absolute difference, amplified for visibility
```

### Industry Usage

- **Chromium (Dawn/Skia)**: On test failure, re-runs with `--enable-gpu-debugging` flag that activates validation layers, state dumps, and shader debug info. Diagnostic output uploaded as CI artifacts.
- **Unity Graphics Tests**: Failed screenshot tests automatically emit a "diff triptych" (reference | actual | diff) as a CI artifact. Internal builds also dump render graph state.
- **Unreal Gauntlet**: Test failures trigger "verbose mode" re-runs that collect additional telemetry (frame timing histograms, memory snapshots, GPU crash dumps).
- **Mesa CI (piglit)**: Failed GL tests re-run with `MESA_DEBUG=1` and `LIBGL_DEBUG=verbose`, capturing driver-level diagnostics automatically.
- **Valve Steam Deck QA**: Automated test failures trigger a RenderDoc capture on the next run via `RENDERDOC_HOOK_EGL=1`, uploading the `.rdc` file as a CI artifact.

### Limitations

- **Re-run assumes reproducibility** — if the failure is truly non-deterministic (race condition, uninitialized memory), the diagnostic re-run may pass, producing no useful data. Mitigation: collect lightweight diagnostics (statistics, state snapshots) on *every* run, not just re-runs.
- **Doubles wall-clock time for failures** — acceptable if failures are rare (<5% of runs). Problematic during large refactors where many tests fail simultaneously. Mitigation: cap the number of diagnostic re-runs per suite (e.g., first 5 failures get full diagnostics, rest get summary only).
- **Diagnostic code is code** — the instrumentation itself can have bugs, or worse, can *mask* the original bug by changing timing or state. Mitigation: diagnostics should be read-only (no state modification), and the pass/fail verdict always comes from the *first* run, never the diagnostic run.

---

## Comparison Matrix

| Approach | What it catches | Speed | Requires GPU | Catches visual bugs | Catches state bugs | Catches perf bugs |
|---|---|---|---|---|---|---|
| Golden Image | Everything visual | Slow | Yes | ✅ | Indirectly | ❌ |
| GPU State Validation | State leakage | Fast | Yes (debug) | ❌ | ✅ | ❌ |
| Data Round-Trip | Serialization bugs | Fast | Optional | ❌ | ❌ | ❌ |
| Smoke Readback | Catastrophic failures | Medium | Yes | Coarse | ❌ | ❌ |
| Shader Unit Tests | Math/logic bugs | Fast | Optional | ❌ | ❌ | ❌ |
| Command Sequence | Ordering/resource bugs | Fast | No | ❌ | ✅ | Indirectly |
| Performance Regression | Perf regressions | Slow | Yes | ❌ | ❌ | ✅ |
| Cross-API Conformance | Backend parity | Very slow | Multi-GPU | ✅ | ✅ | ❌ |

---

## Recommended Reading

- [Rendering Testing at Scale (GDC 2019)](https://www.gdcvault.com/) — Unity's talk on maintaining thousands of rendering tests.
- [Filament Test Infrastructure](https://github.com/google/filament/tree/main/test) — open-source example of golden image + headless rendering.
- [Mesa piglit](https://gitlab.freedesktop.org/mesa/piglit) — GL conformance test suite with extensive per-pixel comparison patterns.
- [Vulkan CTS (dEQP)](https://github.com/KhronosGroup/VK-GL-CTS) — the most comprehensive GPU test suite ever built, worth studying for methodology.
- [The Furnace Test (Karis 2013)](https://blog.selfshadow.com/) — Brian Karis on energy conservation validation for PBR.
- [Automated Visual Testing for Games (GDC 2023)](https://www.gdcvault.com/) — practical approaches to screenshot testing in game engines.
