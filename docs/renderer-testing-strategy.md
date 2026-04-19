# Renderer Testing Strategy

A production-grade testing specification for OloEngine's rendering pipeline, modeled after the practices of AAA engines (Unreal, Frostbite, id Tech) and informed by Bart Wronski's analysis of what separates effective rendering tests from brittle screenshot-blessing workflows.

**Core philosophy:** The primary question a rendering test should answer is not *"did the output change?"* but *"does the output have the correct properties?"* Tests that verify mathematical invariants, physical constraints, and algorithmic contracts catch real bugs with actionable failure messages. Screenshot tests serve as a thin integration safety net on top.

---

## Testing Pyramid for Rendering

The testing strategy is organized as a pyramid. The base is fast, numerous, and deterministic. The top is slow, few, and integration-focused. Most test development effort targets the bottom layers.

```
                    ┌─────────────┐
                    │  Golden     │  ~10–15 integration smoke tests
                    │  Image      │  Slow. Catches "something broke somewhere."
                  ┌─┴─────────────┴─┐
                  │  Perf Regression │  Microbenchmarks + whole-frame budgets
                  │  + Cross-Vendor  │  Medium speed. Catches perf & vendor drift.
                ┌─┴─────────────────┴─┐
                │  GPU State + Command │  RAII guards, state mirrors, command linting
                │  Sequence Validation │  Fast. Catches state leaks & ordering bugs.
              ┌─┴─────────────────────┴─┐
              │  Property / Behavioral   │  Synthetic inputs, numerical verification
              │  Tests on GPU            │  Fast. THE PRIMARY TESTING LAYER.
            ┌─┴─────────────────────────┴─┐
            │  Shader Unit Tests + Data    │  Math correctness, serialization round-trips
            │  Round-Trip Tests            │  Fastest. No scene required.
            └─────────────────────────────┘
```

---

## Current Coverage Snapshot

Live status of the catalog (see [OloEngine/tests/Rendering/PropertyTests/](../OloEngine/tests/Rendering/PropertyTests/)). Numbers reflect the state of this branch after the renderer-testing-infrastructure PR.

**Layer 1 — Property tests (`PbrPropertyTests.cpp`, `PostProcessPropertyTests.cpp`):**
- PBR: Fresnel (normal / grazing / monotonicity / CPU reference), NDF (non-negative + finite / peak at H=N / roughness=1 gives 1/π), diffuse (metallic=1 kills diffuse / dielectric diffuse non-zero), BRDF (positive+finite / Helmholtz reciprocity), normal-map identity. *Missing:* furnace, white-environment irradiance, roughness=0 mirror.
- Post-process: tone-map monotonicity (all 3 operators) + black-to-black (all 3 operators) + extreme-HDR NaN/Inf safety (all 3 operators), vignette center-brighter-than-corners, chromatic aberration center untouched, FXAA uniform no-op + hard-edge flat-region preservation, motion-blur static, DOF at focus distance, bloom threshold/downsample/upsample/composite invariants, fog disabled early-out. *Missing:* full bloom energy conservation over mip chain.

**Layer 2 — GPU state validation (`GLStateGuardTest.cpp`):** 8 tests covering blend / depth / stencil / FBO / viewport / texture-unit / UBO-binding leak detection via RAII snapshot-and-assert.

**Layer 3 — Data round-trip (`DataRoundTripTests.cpp`):** RGBA32F GPU bit identity, RGBA8 GPU byte identity, **IBL cache cubemap round-trip preserves all mips** (targets the `IBLCache::LoadCubemapFromCache` historical bug directly).

**Layer 4 — Shader unit tests (`ShaderUnitTests.cpp`):** sRGB↔linear round-trip, tone-map reference values (Reinhard / ACES / Uncharted2), GGX NDF hemisphere integral, octahedral normal round-trip, fog endpoint invariants (zero-distance → 0, infinite-distance → 1, linear-mode clamps).

**Layer 5 — Render graph validation:** *Not yet implemented.* `CommandBucket` has Layer-0 coverage via `CommandBucketTest.cpp` / `CommandDispatchTest.cpp`; structural graph validation (pass ordering, resource hazards) is deferred.

**Layer 6 — Perf regression (`PerfRegressionTests.cpp`):** GPU timing infrastructure + 4 microbenchmarks (tone-map, bloom threshold / downsample / upsample at 512²). No checked-in baselines yet — thresholds are sanity bounds only.

**Layer 7 — Smoke/sanity readback (`RendererValidateTest.cpp`):** Helper + 5 tests for NaN / Inf / energy-bound scans after key passes.

**Layer 8 — Golden image:** *Deferred* — per-vendor reference management + cascaded RMSE/SSIM comparison pipeline not yet implemented.

**Layer 9 — Cross-vendor conformance:** *Deferred* — single-GPU CI today; SwiftShader CI container is a follow-up.

**Layer 10 — Automatic diagnostic escalation:** *Deferred* — Stage-0 failure reporting only today.

**Layer 11 — Sanitizers / fuzzing:** Build flags exist (`cmake/Sanitizers.cmake`); no dedicated fuzz targets yet.

**Headline numbers:** ~2070 tests across ~335 suites, full run ≈ 35–45 s on a developer box.

---

## 1. Property-Based / Behavioral Testing with Synthetic Data

**This is the primary testing layer.** Every rendering algorithm has mathematical and physical properties that can be verified numerically. These tests replace authored scenes with procedurally generated inputs, run production GPU code paths, and verify outputs against known invariants. They are fast, deterministic, decoupled from the content pipeline, and produce actionable failure messages.

### Core Principles (Wronski's "Approach B")

1. **Generate inputs procedurally in code.** A lighting buffer with one white pixel. A flat GBuffer. A uniform white environment cubemap. A linear gradient. Never depend on authored meshes, textures, or scenes — those couple your test to the content pipeline, material system, camera system, and gamma correction all at once. Change any of those systems and every test breaks, drowning real regressions in noise.
2. **Run production GPU code.** The actual shader code, the actual render pass, the actual framebuffer format. Not a CPU simulation. This is what distinguishes property tests from pure unit tests — they exercise the real code path including driver behavior.
3. **Verify outputs numerically.** Integrate pixel values. Compute statistics. Check monotonicity. Assert energy bounds. The failure message should say *"bloom pass gained 3.2% energy (expected ≤ 0.5%)"* not *"pixels differ."*
4. **One test, one property, one algorithm.** Isolation is the point. When a test fails, you know exactly what broke and where.

### OloEngine-Specific Test Catalog

#### PBR / Lighting

| Test | Synthetic Input | Verification | What It Catches |
|---|---|---|---|
| **Furnace test** | White sphere, uniform white environment, sweep roughness 0→1 | Every pixel ≈ 1.0 (±ε). Any deviation = energy loss/gain. | BRDF energy conservation bugs, Fresnel errors, missing multi-scatter compensation |
| **White environment irradiance** | Uniform white cubemap → irradiance convolution | Irradiance ≈ π for Lambertian diffuse | IBL convolution normalization errors |
| **GGX NDF normalization** | Sweep roughness, integrate NDF over hemisphere via compute shader | Integral ≈ 1.0 for all roughness values | NDF formula bugs, clamping errors |
| **Fresnel at normal incidence** | F0 = 0.04 (dielectric), viewing angle = 0° | Output ≈ F0 | Fresnel-Schlick implementation errors |
| **Fresnel at grazing incidence** | Any F0, viewing angle → 90° | Output → 1.0 | Missing Fresnel clamp, NaN at extreme angles |
| **Metallic kills diffuse** | Metallic = 1.0, diffuse albedo = (1,0,0) | Diffuse contribution = 0 | Incorrect metallic workflow |
| **Roughness = 0 is mirror** | Roughness = 0, single directional light | Specular highlight concentrated in ≤ 2×2 pixels | GGX singularity at roughness 0 |
| **Normal map identity** | Flat normal map (128, 128, 255) vs no normal map | Pixel-exact identical output | TBN construction errors, normal map range remapping bugs |

#### Post-Processing

| Test | Synthetic Input | Verification | What It Catches |
|---|---|---|---|
| **Bloom energy conservation** | Single bright pixel (10.0, 10.0, 10.0), black background | Sum of all pixels after bloom ≈ sum before bloom (±1%) | Bloom adding or losing energy |
| **Bloom black passthrough** | All-black input | Output = all black | Bloom shader producing values from nothing |
| **Tone mapping monotonicity** | Linear gradient 0→10 | Output is monotonically non-decreasing | Tone mapping curve inversion, clamping errors |
| **Tone mapping black-to-black** | All-zero input | Output = all zero | Tone mapping offset/bias bugs |
| **FXAA edge displacement** | Hard diagonal edge (white/black) | Edge pixels displaced by ≤ 1px from geometric edge | Overly aggressive AA, broken edge detection |
| **DOF CoC correctness** | Single point at known depth, known focal params | Blur radius matches thin-lens equation (±1px) | Circle-of-confusion formula errors |
| **Vignette center untouched** | Uniform white input, vignette enabled | Center pixel = 1.0 | Vignette applying darkening at center |
| **Chromatic aberration center untouched** | Uniform color input, CA enabled | Center pixel = input color | CA shifting colors at optical center |
| **Motion blur static** | Static camera, static scene | Output ≈ input (no blur applied) | Motion blur activating on zero-velocity pixels |
| **Fog at zero distance** | Fragment at camera position | Fog contribution = 0 | Fog applying at zero distance |
| **Fog at infinite distance** | Fragment at far plane | Fragment fully replaced by fog color | Fog not reaching full density |

#### Shadows

| Test | Synthetic Input | Verification | What It Catches |
|---|---|---|---|
| **No self-shadowing** | Flat plane facing light, at known distance | All lit pixels ≈ fully lit | Shadow bias too small |
| **No peter-panning** | Object on plane, shadow enabled | Shadow contacts object base (no gap) | Shadow bias too large |
| **Shadow outside frustum** | Object behind shadow camera frustum | No shadow cast | Shadow frustum calculation errors |

#### IBL Pipeline

| Test | Synthetic Input | Verification | What It Catches |
|---|---|---|---|
| **Prefilter mip energy monotonicity** | Environment cubemap → prefilter chain | Energy(mip N) ≤ Energy(mip N-1) for rougher mips | Mip generation bugs, wrong mip level writes |
| **Prefilter mip 0 ≈ source** | Compare prefilter mip 0 to source cubemap | RMSE < tight threshold | Prefilter at roughness=0 diverging from source |
| **IBL reload consistency** | Generate IBL, reload from cache, generate again | Cached vs fresh output identical | Cache key instability, mip loading bugs (a bug we actually had) |

#### Terrain

| Test | Synthetic Input | Verification | What It Catches |
|---|---|---|---|
| **Flat heightmap** | Heightmap of constant value | All terrain vertices at same Y | Heightmap sampling offset bugs |
| **Splatmap channel isolation** | Splatmap with only channel 0 active | Only texture layer 0 visible | Splatmap channel swizzle errors |

### Implementation Approach

Tests use GoogleTest fixtures with a shared headless GL context:

```cpp
class RenderPropertyTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Headless GL 4.6 context (EGL or hidden window)
        // Minimal framebuffer at test resolution (e.g. 128×128)
    }

    // Helper: create a procedural texture from a lambda
    Ref<Texture2D> MakeTexture(u32 width, u32 height,
        std::function<glm::vec4(u32 x, u32 y)> generator);

    // Helper: read back framebuffer and compute statistics
    struct PixelStats { f32 min, max, avg; u32 nanCount, infCount; };
    PixelStats ReadbackStats(const Ref<Framebuffer>& fbo, u32 attachment);

    // Helper: sum all pixel values (for energy tests)
    glm::vec4 ReadbackSum(const Ref<Framebuffer>& fbo, u32 attachment);
};

TEST_F(RenderPropertyTest, BloomPreservesEnergy)
{
    // 1. Procedural input: single bright pixel
    auto input = MakeTexture(128, 128, [](u32 x, u32 y) {
        return (x == 64 && y == 64) ? glm::vec4(10.0f) : glm::vec4(0.0f);
    });
    auto inputSum = glm::vec4(10.0f); // Known analytically

    // 2. Run production bloom pass
    auto output = RunBloomPass(input);

    // 3. Numerical verification
    auto outputSum = ReadbackSum(output, 0);
    f32 energyRatio = outputSum.r / inputSum.r;
    EXPECT_NEAR(energyRatio, 1.0f, 0.01f)
        << "Bloom energy ratio: " << energyRatio
        << " (expected 1.0 ± 1%)";
}
```

### Why This Layer Is Primary

Property tests embody every quality Wronski identifies as essential:
- **Verify correctness**, not just "looks okay" — they answer *"is the Gaussian blur brightening the image?"* not *"did the image change?"*
- **Trivial to add** — a new test is 10–30 lines of C++ in the same directory as the code it tests. No editor, no scenes, no golden files.
- **Serve as documentation** — reading the test catalog tells you the design assumptions: "roughness=0 is a mirror," "bloom conserves energy," "fog is zero at camera position."
- **Fail with actionable messages** — *"energy increased by 3.2%"* not *"RMSE 0.0127."*
- **Decoupled from content pipeline** — changing the asset importer, material format, or camera system breaks zero property tests.
- **Fast** — most run in <50ms. The entire catalog finishes in seconds.
- **Deterministic** — synthetic inputs have no randomness. No temporal effects. No authored data dependencies.

### Limitations

- **Doesn't test integration** — a bloom pass that conserves energy perfectly but reads from the wrong texture slot won't be caught. Golden image tests cover this.
- **Requires understanding the algorithm** — you must know *what properties to test.* This is a feature, not a bug — it forces the implementer to articulate their assumptions.
- **GPU precision variance** — identical synthetic inputs can produce slightly different outputs across vendors. Use tolerances derived from IEEE 754 analysis, not empirical tuning.

---

## 2. Shader Unit Testing

### Core Concept

Test individual shader functions in isolation on small synthetic inputs. Either by compute shader dispatch (GPU-side) or by making GLSL functions compilable as C++ via shared includes.

### What It Tests

- **Mathematical correctness** — PBR equations (GGX NDF, Fresnel-Schlick, Smith geometry), tone mapping operators (Reinhard, ACES, Uncharted2), color space conversions (sRGB ↔ linear), normal mapping math.
- **Edge cases** — roughness = 0 or 1, metallic = 0 or 1, NdotL = 0, grazing angles, degenerate normals, F0 = 0 or 1.
- **Numerical stability** — division by zero guards, clamping, avoiding log(0) or sqrt(negative).
- **GBuffer packing/unpacking** — 0 maps to 0, 1 maps to 1, 0.5 maps to 0.5, round-trip within quantization tolerance. Engines routinely ship with 127/128 or 255/256 instead of 1.0 from wrong packing logic.

### Approaches

**Approach A: Compute Shader Harness (preferred for OloEngine)**
Write a compute shader that calls the function under test with known inputs, writes output to an SSBO, read back on CPU, and compare. Tests the actual compiled shader code on the actual GPU.

```glsl
// test_brdf.comp
#version 460
#include "PBR/BRDF.glsl"

layout(std430, binding = 0) buffer TestOutput { vec4 results[]; };
layout(std430, binding = 1) buffer TestInput  { vec4 inputs[];  };

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    vec3 N = vec3(0, 1, 0);
    float roughness = inputs[idx].x;
    float NdotH = inputs[idx].y;
    results[idx] = vec4(DistributionGGX(N, N, roughness), 0, 0, 0);
}
```

**Approach B: GLSL-to-C++ Dual Compilation**
Core math in `.glsl` include files with `#ifdef __cplusplus` guards. Run on CPU with GoogleTest. Fast, no GPU context needed, but may miss driver-specific behavior.

**Approach C: Shared Test Vectors**
Maintain a set of known input/output pairs (Filament's approach). Both C++ and GPU paths must produce matching results for the same inputs.

### Specialized Tests

- **Furnace test** — white sphere, uniform white environment. Correct energy-conserving BRDF → perfectly white at all angles and roughness. Any deviation = energy leak. (See Section 1 for the property test version.)
- **White environment irradiance** — irradiance of uniform white = π for Lambertian.
- **Normal map identity** — flat normal (128, 128, 255) = no normal map.
- **Tone mapping reference values** — known HDR inputs through each operator (Reinhard, ACES, Uncharted2) compared against hand-computed reference.
- **sRGB ↔ linear round-trip** — convert to linear, convert back, verify < 1 LSB error.

### Industry Usage

- **Unreal**: Shader permutation tests validate compilation of all material permutations. Limited runtime verification of outputs.
- **Filament**: Core BRDF functions implemented in both C++ and GLSL with shared test vectors.
- **Unity HDRP**: Dedicated shader function tests in the Graphics Tests repo.
- **Khronos dEQP**: Per-function precision tests for every GLSL built-in (`sin`, `cos`, `pow`, etc.) across all GPU vendors.

### Limitations

- **GPU compilation differences** — behavior can differ between NVIDIA, AMD, Intel due to precision, instruction reordering, and fast-math.
- **Combinatorial explosion** — engines have thousands of shader permutations. Test the shared math functions, not every permutation.

---

## 3. Data Round-Trip / Serialization Tests

### Core Concept

Generate rendering data procedurally (textures, cubemaps, meshes, shader binaries, material parameters), serialize to the engine's cache format, deserialize back, and compare with the original. Verifies that the cache pipeline preserves data integrity.

### What It Tests

- **Format correctness** — writing RGBA32F but reading as RGB32F, wrong bytes-per-pixel, endianness issues.
- **Mip chain integrity** — all mip levels saved and restored, not just mip 0. (We have actually shipped a bug where `IBLCache::LoadCubemapFromCache` only loaded mip 0.)
- **Lossy vs lossless encoding** — if cache uses compression (meshopt, BC), verify error within tolerance.
- **Header/metadata correctness** — dimensions, format enums, mip counts, face counts match actual data.
- **Version compatibility** — cache v(N) loadable by v(N+1) or cleanly rejected.

### Test Patterns

All test data is **procedurally generated** — deterministic gradients, checkerboards, known bit patterns. No dependency on authored assets.

- **Identity round-trip**: `data → serialize → deserialize → compare`. Pure CPU, fast, no GPU.
- **GPU round-trip**: `data → upload to GPU → readback → compare`. Tests the upload/readback path.
- **Full pipeline round-trip**: `data → GPU render → cache save → cache load → GPU upload → readback → compare`. Tests everything.

### Industry Usage

- **Assimp**: Import → export → re-import tests for mesh formats.
- **KTX (Khronos Texture)**: Round-trip tests for every texture format including mip chains and cubemap faces.
- **USD (Pixar)**: Serialization conformance suite verifying write → read survival.

### Limitations

- **Only tests the data path** — doesn't verify data is *used correctly* by the renderer.
- **Doesn't catch "renders differently"** — data can round-trip perfectly but bind to the wrong texture slot.

---

## 4. GPU State Validation / Sanitization

### Core Concept

At the boundaries of rendering passes (shadow, scene, IBL, post-process), capture a snapshot of critical GPU state and assert correctness. This is the layer that catches the class of bugs OloEngine has historically suffered: `ResetState()` zeroing shadow texture IDs, `OpenGLFramebuffer::Bind()` unconditionally enabling blend, blend state leaking from InfiniteGrid into terrain.

### What It Tests

- **State leakage between passes** — shadow pass disables depth writes and forgets to re-enable. Post-process leaves blend on. IBL pass leaves its UBO bound.
- **Implicit assumptions** — code that assumes "depth test is enabled here" without verification.
- **Driver-silent failures** — clearing depth with depth mask disabled silently no-ops. Technically valid, produces wrong results.

### Tracked States

Depth test/mask, blend mode/factors, stencil op/mask, viewport/scissor, bound FBO, bound textures per slot, bound UBOs per slot, active shader program, face culling mode/enabled, polygon mode.

### Approaches

**Approach A: RAII State Guards (recommended first step)**
An RAII object snapshots state on construction and asserts/restores on destruction. Place at render pass boundaries. Lightweight, incremental adoption.

```cpp
class GLStateGuard
{
public:
    GLStateGuard(std::string_view passName);  // Snapshots current state
    ~GLStateGuard();                           // Asserts state restored or logs violations
private:
    GLStateSnapshot m_EntryState;
    std::string_view m_PassName;
};

// Usage in SceneRenderPass::Execute()
{
    GLStateGuard guard("ScenePass");
    // ... render ...
}   // ~GLStateGuard asserts depth/blend/stencil match entry state
```

**Approach B: CPU-Side State Mirror**
Maintain a CPU mirror of all GPU state. Every `glEnable`/`glDisable`/`glBindTexture` goes through the mirror, which validates transitions. Like Filament's `DriverBase` or bgfx's encoder.

**Approach C: GL Debug Callbacks + Custom Validators**
`glDebugMessageCallback` (GL 4.3+) for driver issues, plus custom `glGet*` validators at pass boundaries.

### Industry Usage

- **Filament**: `DriverBase` tracks all state, validates transitions, asserts on illegal combinations.
- **bgfx**: CPU-side state mirror validates before submission.
- **WebGPU/Dawn**: Validation layer rejects invalid state transitions before reaching driver.
- **Vulkan Validation Layers**: Gold standard — intercepts every API call, validates correctness. No GL equivalent, so engines must build their own.

### Limitations

- **Performance overhead** — `glGet*` calls stall the pipeline. Debug builds only.
- **Incomplete coverage** — only validates states you thought to track.
- **Not visual** — correct state doesn't mean correct output.

---

## 5. Render Graph / Command Sequence Validation

### Core Concept

Capture the sequence of rendering commands for a frame and validate structural properties: correct ordering, no missing barriers, no redundant state changes, correct resource lifetimes, no null resource references.

### What It Tests

- **Pass ordering** — shadow before scene, post-process after scene, IBL before any pass that reads IBL.
- **Resource hazards** — texture written by pass A read by pass B without barrier/sync.
- **Redundant state** — binding same shader or texture twice consecutively.
- **Missing resources** — draw command references texture ID 0 (null/uninitialized).
- **Command budget** — frame within expected draw call / state change count.

### Approaches

**Approach A: Frame Capture + Offline Analysis**
Record a frame's commands to structured JSON. Analyze offline with custom validators. Diff between captures to find what changed.

**Approach B: Render Graph Static Analysis**
Validate graph structure before execution: check for cycles, verify resource lifetimes, detect unused passes, validate format compatibility. Unreal's RDG does this.

**Approach C: Runtime Command Stream Validation**
Intercept commands at submission and validate in real-time. A "linter" for the command stream. Modeled after bgfx's submission-time validation.

### Industry Usage

- **Unreal RDG**: Static validation — detects missing transitions, unused resources, aliasing conflicts at graph build time.
- **Vulkan Validation Layers**: Resource barriers, pipeline hazards, descriptor set bindings.
- **RenderDoc**: Records frame for inspection of every API call, resource state, pipeline stage.
- **bgfx**: Submission-time validation of draw state.

### Limitations

- **Structural, not visual** — valid command sequence can produce wrong pixels.
- **Complex to implement** — requires render graph abstraction or comprehensive command interception.

---

## 6. Performance Regression Testing

### Two Tiers: Microbenchmarks and Whole-Frame Budgets

#### Tier 1: Microbenchmarks with Controlled Inputs (primary)

Isolate individual passes with procedurally generated inputs at controlled sizes. Eliminates scene complexity noise and content pipeline dependencies. Modeled after Wronski's recommendation and id Tech's internal benchmark methodology.

| Benchmark | Controlled Variables | Metric |
|---|---|---|
| **Bloom pass** | Resolution (720p, 1080p, 4K), % pixels above threshold (0%, 50%, 100%) | GPU time via timestamp queries |
| **Shadow map generation** | Triangle count (1K, 10K, 100K), cascade count (1–4), resolution (1024–4096) | GPU time |
| **FXAA pass** | Resolution, edge density (0%, 50%, 100%) | GPU time |
| **Tone mapping** | Resolution, operator (Reinhard, ACES, Uncharted2) | GPU time |
| **PBR lighting** | Light count (1, 8, 64, 256), material complexity | GPU time |
| **Terrain rendering** | LOD level, patch count, splatmap layers | GPU time, triangle count |

Run N frames, discard first M (warmup), report median and P95. Compare against checked-in baselines with ±10% tolerance.

#### Tier 2: Whole-Frame Budgets

Standard scenes with fixed camera paths and full pipeline active. Measures aggregate frame time, draw call count, state change count, GPU memory. Noisier than microbenchmarks but catches integration-level regressions (broken batching, sort order changes).

### Key Design Decisions

| Decision | Options |
|---|---|
| **GPU timing** | `GL_TIME_ELAPSED` queries per pass, `glQueryCounter` for start/end timestamps |
| **Baseline management** | Checked-in JSON with expected ranges per GPU family |
| **Noise mitigation** | Median of 100 frames after 20-frame warmup. Run on idle machine. |
| **Thresholds** | 5–10% warns, >20% fails. Per-pass and per-frame budgets. |

### Industry Usage

- **Unreal**: Stat system + Gauntlet tests tracking GPU/CPU timings across maps.
- **id Tech (Doom)**: Fixed camera paths, strict frame time budgets. Internal benchmarks are legendary for catching regressions.
- **Unity**: Performance test framework with statistical analysis and historical tracking.
- **Chromium (Skia/Dawn)**: `perf_tests` with historical tracking and bisection support.

### Limitations

- **Noisy** — thermal state, driver version, background load all add variance.
- **Platform-specific** — regression on NVIDIA may not appear on AMD.
- **Doesn't catch visual bugs** — skipping half the draw calls makes frames faster.

---

## 7. Smoke / Sanity Readback Tests

### Core Concept

After key rendering operations, read back pixel statistics and assert basic invariants: non-zero, no NaN, no Inf, within expected HDR range, alpha correctness. Not comparing against a reference — checking that output isn't catastrophically broken. These run on *every* frame in debug builds as a continuous safety net.

### What It Tests

- **Catastrophic failures** — entire pass rendered black, NaN propagation, infinite values, alpha corruption.
- **Energy bounds** — HDR output within expected range (not clamped to [0,1] when it shouldn't be, not exceeding reasonable maxima).
- **Format correctness** — expecting HDR but reading back clamped values → wrong texture format.

### OloEngine Integration

A `ValidateOutput` helper called after each pass in debug builds:

```cpp
void ValidatePassOutput(const Ref<Framebuffer>& fbo, std::string_view passName)
{
    auto stats = ReadbackStats(fbo, 0);
    OLO_CORE_ASSERT(stats.nanCount == 0,
        "{}: {} NaN pixels detected", passName, stats.nanCount);
    OLO_CORE_ASSERT(stats.infCount == 0,
        "{}: {} Inf pixels detected", passName, stats.infCount);
    OLO_CORE_ASSERT(stats.max < 65504.0f,  // fp16 max
        "{}: max value {} exceeds fp16 range", passName, stats.max);
}
```

### Limitations

- **Very coarse** — "not black and not NaN" is a low bar. Complement with property tests.
- **Debug-only** — readback stalls make this too expensive for release.

---

## 8. Golden Image / Screenshot Comparison Testing

### Role in the Strategy

Golden image tests sit at the **top of the testing pyramid** — few in number, integration-focused, and expected to change occasionally. Following Wronski's recommendation: **no more than 10–15 golden smoke tests**, each cramming many features into one scene, testing the pipeline end-to-end. These are *not* per-feature tests.

The per-feature correctness verification is handled by property tests (Section 1). Golden images answer a different question: *"does the fully composed frame, with all systems interacting, still produce acceptable output?"*

### Scene Design

Each golden test scene is designed to exercise multiple systems simultaneously:

| Scene | Systems Exercised |
|---|---|
| **PBR Material Gallery** | PBR shading, IBL, shadow mapping, normal mapping, tone mapping, gamma correction |
| **Post-Process Showcase** | Bloom, FXAA, DOF, motion blur, vignette, chromatic aberration, fog, color grading |
| **Terrain + Foliage** | Terrain LOD, splatmap blending, foliage instancing, shadow cascades |
| **Animated Characters** | Skeletal animation, bone transforms, skinning, shadow casting from animated meshes |
| **Transparency + Particles** | Alpha blending, particle rendering, OIT (if applicable), depth sorting |
| **Stress Test** | Maximum lights, maximum draw calls, full post-process chain, all passes active |

### Multi-Stage Comparison Pipeline

A cascaded approach using cheap metrics as fast-path filters:

1. **RMSE (fast)** — per-pixel RMSE across the whole image.
   - RMSE < 0.002: **pass immediately.**
   - RMSE > 0.02: **fail immediately.**
   - Between: **ambiguous** → escalate to stage 2.
2. **SSIM or FLIP (expensive)** — perceptual metric on ambiguous cases.
   - SSIM > 0.98: **pass** — differences imperceptible.
   - SSIM ≤ 0.98: **fail** — perceptually visible regression.

**Why cascade?** RMSE is oversensitive to isolated bright-pixel outliers (specular highlight shifts 1px → high RMSE, visually identical) and undersensitive to distributed low-contrast shifts (subtle color cast → low RMSE, clearly visible). Perceptual metrics handle both correctly but cost 5–10× more. ~90% of runs resolve at stage 1.

### Variant: Differential Testing

Render the same scene through two code paths (cached vs fresh, old branch vs new branch) and compare. No reference images needed.

### Key Design Decisions

| Decision | Options | Trade-offs |
|---|---|---|
| **Threshold strategy** | Global RMSE, per-pixel max delta, % pixels differing | Too tight → flaky. Too loose → misses bugs. |
| **Reference management** | Git LFS, per-platform references | Per-platform handles vendor variance. Regenerate explicitly after intentional changes. |
| **Determinism** | Fixed camera, fixed resolution, fixed RNG seed, disabled temporal effects | Temporal AA/jitter/animation must be disabled or seeded. |
| **Headless rendering** | Hidden window, EGL offscreen, SwiftShader | Hidden window simplest. SwiftShader for CI without GPU (property tests only — golden images need real GPU). |

### Process for Golden Test Failures

When a golden test fails, the developer workflow is:

1. **Check property tests first.** If property tests also fail, fix the property-level bug — the golden test failure is a symptom.
2. **If property tests pass**, the failure is an integration issue (system interaction, state ordering, resource binding). Use automatic diagnostic escalation (Section 10) output.
3. **Investigate the diff.** Examine the heatmap, per-channel breakdown, and statistics.
4. **If the change is intentional**, update the golden reference in the **same commit** as the code change. (Wronski: test updates in the same CL as code changes, not separated.)
5. **Never blind-bless.** Every reference update requires understanding *why* pixels changed.

### Industry Usage

- **Unreal Engine**: `FAutomationTestBase` + screenshot comparison, per-platform golden images, "Screenshot Comparison" tool.
- **Unity**: Graphics Tests repository with hundreds of scene-based tests per render pipeline.
- **Filament**: `test_Filament` renders to headless SwapChain, compares against committed PNGs.
- **Godot**: Screenshot tests in `tests/servers/rendering/`, CI-integrated.

### Limitations

- **Brittle across GPU vendors** — requires per-vendor references or generous thresholds.
- **Slow** — full scene load + render + readback. 0.5–2s per test.
- **Binary pass/fail** — shows *that* something changed, not *why*. Needs diagnostic escalation.

---

## 9. Cross-Vendor Conformance

### Core Concept

Run the same test suite across different GPU vendors (NVIDIA, AMD, Intel) comparing results for consistency. Since OloEngine currently targets OpenGL 4.6, this is cross-vendor rather than cross-API.

### What It Tests

- **Vendor differences** — NVIDIA's fast-math vs AMD's stricter IEEE compliance, Intel's different texture filtering.
- **Precision differences** — denormalized float handling, rounding mode differences.
- **Format support gaps** — not all formats behave identically (e.g., RGB32F filtering behavior).
- **Driver regressions** — a new driver version changes behavior. Tracked over time.

### Software Rasterizer Strategy

For property tests (Section 1) and shader unit tests (Section 2), consider **SwiftShader** as a deterministic reference implementation:
- Property tests check *invariants* (energy = input, monotonicity, etc.) — small precision differences between vendors don't matter.
- Running property tests through SwiftShader in Docker enables fully deterministic CI with no GPU hardware.
- Golden image tests (Section 8) still require real GPU hardware — SwiftShader's output differs too much for pixel comparison.

### Industry Usage

- **Unreal**: Per-platform golden images, cross-API parity tests.
- **Filament**: Tests across Vulkan, OpenGL, Metal backends.
- **WebGPU CTS**: Thousands of per-feature tests across Dawn, wgpu, all GPU vendors.
- **Vulkan CTS (dEQP)**: Khronos-maintained, ~500k tests.

### Limitations

- **Requires hardware** — or cloud GPU CI for each target vendor.
- **Single API limits scope** — can test cross-vendor but not cross-API until a Vulkan backend exists.

---

## 10. Automatic Diagnostic Escalation

### Core Concept

When a test fails, don't just report "FAIL" — automatically re-run with progressively more diagnostics. The first run is fast. The second run collects the data a developer would manually gather. Only failing tests pay the diagnostic cost.

### Escalation Stages

| Stage | Trigger | What It Collects | Cost |
|---|---|---|---|
| **Stage 0: Normal run** | Always | Pass/fail, assertion message | Baseline |
| **Stage 1: Readback diagnostics** | Stage 0 fails | Pixel readback at key pipeline stages, min/max/avg statistics, NaN/Inf scan, energy totals | ~2× (GPU readback stalls) |
| **Stage 2: State snapshots** | Stage 0 fails | Full GL state dump at pass boundaries, diff against expected state | ~3× (`glGet*` queries) |
| **Stage 3: Frame capture** | Stage 0 fails | Full command stream log, diff against baseline capture | ~5× (command recording) |
| **Stage 4: Visual diff** | Golden image fails | Side-by-side (reference \| actual \| heatmap), per-channel breakdown, histogram of deltas | ~1.5× (image processing) |

Stages 1–4 are independent and run in parallel during a single diagnostic re-run.

### Diagnostic Report Structure

```text
test-output/
└── IBL_ReloadConsistency_FAIL/
    ├── summary.json              # Machine-readable: which stages ran, what they found
    ├── assertion.txt             # Original failure message with numerical context
    ├── readback/
    │   ├── scene_pass_face0.exr  # HDR pixel data after scene pass
    │   ├── postprocess_out.exr   # After post-processing
    │   └── statistics.json       # min/max/avg/NaN count/energy total per stage
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
        └── diff_heatmap.png      # Per-pixel absolute difference, amplified
```

### Key Design Decisions

| Decision | Recommendation |
|---|---|
| **Re-run strategy** | Re-run only failing tests (cap: first 5 failures get full diagnostics, rest summary only) |
| **Output format** | Both structured JSON (for tooling/dashboards) and human-readable log |
| **Artifact storage** | CI artifact upload. Local temp for development. Never committed. |
| **Instrumentation** | Runtime flags (env var `OLO_DIAGNOSTIC_MODE=1`). Same binary, different mode. |

### Industry Usage

- **Chromium (Dawn/Skia)**: `--enable-gpu-debugging` on failure activates validation layers, state dumps, shader debug info. CI artifacts.
- **Unity Graphics Tests**: Failed screenshots emit diff triptych as CI artifact. Internal builds dump render graph state.
- **Unreal Gauntlet**: Failures trigger verbose re-runs with telemetry (frame timing histograms, memory snapshots, GPU crash dumps).
- **Mesa CI (piglit)**: Failed tests re-run with `MESA_DEBUG=1` and `LIBGL_DEBUG=verbose`.
- **Valve Steam Deck QA**: Failures trigger RenderDoc capture via `RENDERDOC_HOOK_EGL=1`, uploading `.rdc` as CI artifact.

### Limitations

- **Assumes reproducibility** — non-deterministic failures may not reproduce. Mitigation: collect lightweight stats on *every* run.
- **Doubles wall-clock for failures** — acceptable if failures < 5% of runs.
- **Diagnostic code is code** — instrumentation can mask bugs by changing timing. Diagnostics must be read-only. Pass/fail verdict always comes from first run.

---

## 11. Sanitizers, Fuzzing, and Memory Safety

### Core Concept

Compiler-provided sanitizers and fuzz testing catch entire categories of bugs that no pixel comparison ever will: buffer overruns, data races, undefined behavior, use-after-free. Bart Wronski calls these "amazing ROI" and dedicates a section to them despite being "off-topic" for graphics.

These are not rendering-specific — they protect the engine's foundation. A corrupted vertex buffer or a race condition in command queue submission will eventually manifest as a visual bug, but the root cause is invisible to rendering tests.

### Sanitizers

| Sanitizer | What It Catches | OloEngine Relevance |
|---|---|---|
| **AddressSanitizer (ASan)** | Buffer overruns, use-after-free, stack overflow, memory leaks | Vertex/index buffer construction, texture readback, asset loading, custom allocators |
| **ThreadSanitizer (TSan)** | Data races, lock-order inversions, deadlocks | Command queue submission (Molecular Matters-inspired), asset hot-reload, multithreaded scene loading |
| **UndefinedBehaviorSanitizer (UBSan)** | Signed overflow, null deref, alignment, shift errors | Fixed-point math, bitfield packing, serialization, enum casting |

### CMake Integration

```cmake
option(OLO_ENABLE_ASAN  "Enable AddressSanitizer"          OFF)
option(OLO_ENABLE_TSAN  "Enable ThreadSanitizer"           OFF)
option(OLO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(OLO_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
# (analogous for TSan, UBSan)
```

On MSVC: `/fsanitize=address` is supported since VS 2019 16.9. TSan and UBSan are not natively available on MSVC — use Clang/LLVM toolchain for those. Consider a Clang-cl CI configuration specifically for sanitizer runs.

### Fuzz Testing

Feed random or semi-random data into parsers and loaders to find crashes, hangs, and undefined behavior:

| Fuzz Target | Input | What It Catches |
|---|---|---|
| **Shader source compilation** | Mutated GLSL source strings | Shader compiler crashes, infinite loops in preprocessor |
| **Asset deserialization** | Mutated `.olo` scene files, `.oar` asset registries | Parser crashes, buffer overruns, OOM from malformed headers |
| **Texture loading** | Corrupted PNG/HDR/KTX data | Image decoder crashes, integer overflow in dimension calculations |
| **Mesh import** | Mutated glTF/FBX data via Assimp | Import pipeline crashes, degenerate geometry handling |

Use libFuzzer (Clang) or similar. Run under ASan for maximum bug detection.

### Industry Usage

- **Chromium**: ASan/TSan/MSan/UBSan are part of standard CI. libFuzzer targets for every parser.
- **Mesa**: Continuous fuzzing of shader compilers via OSS-Fuzz.
- **Unreal**: Internal ASan configurations for development builds.
- **Blender**: OSS-Fuzz integration for file format parsers.

### Limitations

- **Performance overhead** — ASan: ~2× slowdown, TSan: ~5–15×, UBSan: ~1.2×. Not for release builds.
- **MSVC ecosystem gaps** — TSan and UBSan require Clang-cl. Worth the CI complexity.
- **False positives** — rare with ASan/TSan, but driver code in vendor DLLs can trigger spurious reports. Maintain a suppression file.

---

## 12. Test Infrastructure and Developer Experience

### Tests as Documentation

Well-named tests teach the next developer (or your future self) the assumptions of every system. Reading the property test catalog should reveal design contracts:

- *"Roughness=0 produces a mirror reflection"*
- *"Bloom conserves energy"*
- *"Fog contribution is zero at camera position"*
- *"Metallic=1 kills the diffuse term"*

This is especially valuable during onboarding and during refactors where you need to understand what a system *should* do, not just what it currently does.

### Test Proximity

Tests live **adjacent to the code they test**, not in a distant repository or editor-only workflow:

```
OloEngine/src/Renderer/PostProcess/
    BloomPass.cpp
    BloomPass.h
OloEngine/tests/Renderer/PostProcess/
    BloomPropertyTests.cpp       ← property tests
    BloomShaderUnitTests.cpp     ← shader math tests
```

Adding a test requires no editor, no authored scenes, no golden file management — just C++ in the same build tree. Wronski's rule: *"make valued behaviors easy to do."*

### Minimal Friction Test Template

A new rendering property test should be ~15 lines:

```cpp
TEST_F(RenderPropertyTest, FogZeroAtCamera)
{
    auto depthBuffer = MakeDepthTexture(128, 128, 0.0f);  // depth = 0 → at camera
    auto colorBuffer = MakeTexture(128, 128, glm::vec4(1.0f));
    auto output = RunFogPass(colorBuffer, depthBuffer);
    auto stats = ReadbackStats(output, 0);
    EXPECT_NEAR(stats.avg, 1.0f, 0.001f) << "Fog should not affect fragments at camera position";
}
```

### CI Integration

| CI Stage | Tests Run | Expected Duration |
|---|---|---|
| **Pre-commit (local)** | Shader unit tests, data round-trip tests | < 5 seconds |
| **PR gate** | All property tests, state validation, command sequence, smoke readbacks | < 60 seconds |
| **Nightly** | Full suite including golden images, performance benchmarks, cross-vendor | < 15 minutes |
| **Weekly** | Sanitizer builds (ASan, TSan, UBSan), fuzz campaign | < 2 hours |

### Headless Rendering for CI

| Option | Pros | Cons | Use For |
|---|---|---|---|
| **Hidden window** | Simplest, uses real GPU | Needs display server | All tests on GPU-equipped CI |
| **EGL offscreen context** | Truly headless, real GPU | Linux-only for most drivers | Linux CI |
| **SwiftShader** | Deterministic, runs in Docker, no GPU | Different results from hardware | Property tests, shader unit tests |
| **Mesa llvmpipe** | CPU-only, runs anywhere | Slow, different from hardware | Fallback when no GPU available |

For property tests, SwiftShader is the recommended CI path — invariant checks don't require hardware-accurate pixels. Golden image tests require real GPU hardware.

---

## Comparison Matrix

| Approach | What It Catches | Speed | GPU Required | Visual Bugs | State Bugs | Perf Bugs | Actionable Failures |
|---|---|---|---|---|---|---|---|
| **Property / Behavioral** | Algorithm correctness | Fast | Yes | Indirectly | ❌ | ❌ | ✅ Excellent |
| **Shader Unit** | Math/logic bugs | Fastest | Optional | ❌ | ❌ | ❌ | ✅ Excellent |
| **Data Round-Trip** | Serialization bugs | Fastest | Optional | ❌ | ❌ | ❌ | ✅ Good |
| **GPU State Validation** | State leakage | Fast | Yes (debug) | ❌ | ✅ | ❌ | ✅ Good |
| **Command Sequence** | Ordering/resource bugs | Fast | No | ❌ | ✅ | Indirectly | ✅ Good |
| **Smoke Readback** | Catastrophic failures | Medium | Yes | Coarse | ❌ | ❌ | ⚠️ Coarse |
| **Performance** | Perf regressions | Slow | Yes | ❌ | ❌ | ✅ | ✅ Good |
| **Golden Image** | Integration visual regressions | Slow | Yes | ✅ | Indirectly | ❌ | ⚠️ Needs escalation |
| **Cross-Vendor** | Vendor parity | Very slow | Multi-GPU | ✅ | ✅ | ❌ | ⚠️ Moderate |
| **Sanitizers / Fuzzing** | Memory safety, UB, races | Slow | No | ❌ | ❌ | ❌ | ✅ Excellent |

---

## Recommended Reading

- [**How (not) to test graphics algorithms** — Bart Wronski (2019)](https://bartwronski.com/2019/08/14/how-not-to-test-graphics-algorithms/) — The foundational reference for this document. Approach B (property testing with synthetic data) is our primary strategy.
- [**The Furnace Test** — Brian Karis (SIGGRAPH 2013)](https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf) — Energy conservation validation for PBR BRDFs.
- [**Filament Test Infrastructure**](https://github.com/google/filament/tree/main/test) — Open-source golden image + headless rendering.
- [**Mesa piglit**](https://gitlab.freedesktop.org/mesa/piglit) — GL conformance test suite with extensive per-pixel comparison patterns.
- [**Vulkan CTS (dEQP)**](https://github.com/KhronosGroup/VK-GL-CTS) — Most comprehensive GPU test suite, worth studying for methodology.
- [**SwiftShader**](https://github.com/nichmack/prebuilt-swiftshader) — CPU-based Vulkan/GL implementation for deterministic CI.
- **Rendering Testing at Scale (GDC 2019)** — Unity's talk on maintaining thousands of rendering tests.
- **Automated Visual Testing for Games (GDC 2023)** — Practical screenshot testing in game engines.
- [**Google Sanitizers**](https://github.com/google/sanitizers/wiki) — ASan, TSan, MSan, UBSan documentation.
- [**libFuzzer**](https://llvm.org/docs/LibFuzzer.html) — Coverage-guided fuzz testing for C/C++.
