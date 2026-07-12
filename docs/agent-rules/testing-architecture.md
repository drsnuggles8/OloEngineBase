# Testing Architecture

Applies to: `OloEngine/tests/**`

Authoritative reference: **[../testing.md](../testing.md)** — philosophy, value heuristic, named anti-patterns, decision tree, full per-layer reference for the renderer pyramid (L1–L11) and the Functional / cross-subsystem axis. Read it before adding a new test.

This file captures the operational rules that must be followed every time a test is added, moved, renamed, or deleted.

---

## Two axes, not one

OloEngine tests are classified along **two independent axes** that share only the GoogleTest harness and the registration contract:

1. The **renderer testing pyramid** (L1–L11 + plumbing / cullinglod / shaderpipe / integration / meta) — pins contracts of the rendering pipeline.
2. The **Functional / cross-subsystem axis** (tag `"Functional"`) — pins contracts at the seams between subsystems via real `Scene::OnUpdateRuntime` ticks.

The pyramid does *not* extend to layer 12 / 13 / etc. for non-renderer tests — they live on the separate Functional axis. See `docs/adr/0001-functional-tests-as-separate-axis.md`.

---

## 1. Pick a classification before writing code

Every new test belongs to exactly one renderer Layer **or** carries the `"Functional"` tag. If you cannot identify a classification, you are probably writing a symptom check rather than a contract check — stop and design the contract first.

### Renderer pyramid layers

| Layer | Catches | Goes in |
|---|---|---|
| L1 Property / behavioural | math / physics contracts, monotonicity, energy bounds | `OloEngine/tests/Rendering/PropertyTests/*PropertyTests.cpp` |
| L2 Shader unit | GLSL helper correctness via compute-shader readback | `OloEngine/tests/Rendering/PropertyTests/ShaderUnitTests.cpp` + `OloEditor/assets/shaders/tests/` |
| L3 Data round-trip | CPU↔GPU bit/byte identity, cache round-trips | `OloEngine/tests/Rendering/PropertyTests/DataRoundTripTests.cpp` |
| L4 GPU state validation | blend / depth / stencil / FBO / UBO leaks across passes | `OloEngine/tests/Rendering/PropertyTests/GLStateGuardTest.cpp`, `OloEngine/tests/Rendering/RenderStateTest.cpp` |
| L5 Render graph / hazards | pass ordering, read-after-write, parallel-write | `OloEngine/tests/Rendering/RenderGraphTest.cpp`, `ResourceHazardValidationTests.cpp` |
| L6 Performance regression | per-pass timing vs. pinned baseline with anti-flake | `OloEngine/tests/Rendering/PropertyTests/PerfRegressionTests.cpp` + `perf_baselines.txt` |
| L7 Smoke / sanity readback | NaN / Inf detection, empty / clean framebuffers | `OloEngine/tests/Rendering/PropertyTests/RendererValidateTest.cpp` |
| L8 Golden image | SSIM cascade over scene fixtures | `OloEngine/tests/Rendering/PropertyTests/GoldenImageTests.cpp` + `OloEditor/assets/tests/golden/` |
| L9 Cross-vendor | llvmpipe conformance in CI (workflow only, no code) | `.github/workflows/cross-vendor.yml` |
| L10 Diagnostic escalation | auto-capture framebuffer + metadata on `ASSERT_` failure | `TestFailureCapture.{h,cpp}` (consumed automatically) |
| L11 Sanitizers & fuzzing | libFuzzer harnesses + ASan / UBSan in CI | `OloEngine/tests/Fuzzing/Fuzz*.cpp` + `.github/workflows/fuzz.yml` |

Files outside `PropertyTests/` are also classified — see the generated `docs/test-catalogue.renderer.md` (run `generate_test_catalogue.py`) for the full inventory.

### Functional / cross-subsystem axis (tag `"Functional"`)

Anything that drives a real `Scene::OnUpdateRuntime` to pin a contract at the seam between two or more subsystems — Animation × Physics × Scripting × Networking × Audio × Asset × Nav × Save-game × Gameplay × AI. Lives under `OloEngine/tests/Functional/<Subsystem>/`. Inherits `OloEngine::Functional::FunctionalTest`. Catalogued in the generated `docs/test-catalogue.functional.md`. See `docs/adr/0001-functional-tests-as-separate-axis.md` for rationale.

---

## 2. The registration contract (NON-NEGOTIABLE)

The generator scans the **entire** `OloEngine/tests/` tree (recursively) — there is no allowlist and no exclude list. Every `.cpp` that declares at least one `TEST` / `TEST_F` / `TEST_P` / `TYPED_TEST` macro must be classified — by **either** an in-file `// OLO_TEST_LAYER: <id>` comment near the top of the `.cpp` (**preferred**: lives in the test file, so adding a test touches no shared file and two branches can't collide on it) **or** an entry in `file_layer_map` in [`OloEngine/tests/scripts/test_catalogue.json`](../../OloEngine/tests/scripts/test_catalogue.json) (a file uses one or the other, not both; the marker wins). The classification is one of three axes:

- a renderer-pyramid layer id (`L1`–`L11`, `plumbing`, `cullinglod`, `shaderpipe`, `integration`, `meta`) — rendering-scope tests;
- `"Functional"` — cross-subsystem tests driven via `Scene::OnUpdateRuntime`;
- `"unit"` — plain per-subsystem unit tests (everything else), grouped by directory in the doc.

The `test-catalogue-classified` pre-commit hook **fails the commit** otherwise. Files with no test macros (the gtest `main`, libFuzzer targets, fixtures/helpers) are not tests and need no entry — that is the definition of a non-test, not a configurable exclusion.

**Workflow when adding a test file:**

1. Write the test(s). Use `TEST`, `TEST_F`, `TEST_P`, or `TYPED_TEST` — the scanner's regex relies on these macros.
2. Classify the file. **Preferred:** add a `// OLO_TEST_LAYER: <id>` comment near the top of the `.cpp` — a renderer-pyramid layer id (`L1`–`L11`/`plumbing`/…) for rendering-scope tests, `Functional` for cross-subsystem tests, or `unit` for plain per-subsystem unit tests. Or, as a fallback, add the file to `file_layer_map` in `test_catalogue.json` with the same classification.
3. (Optional) Run `python OloEngine/tests/scripts/generate_test_catalogue.py` to (re)build the three per-axis catalogue documents (`docs/test-catalogue.{renderer,functional,unit}.md`). These are git-ignored, not committed — regenerate only if you want a fresh local copy to browse.
4. Commit just the .cpp (with its marker) — the catalogue docs are git-ignored, and no shared JSON edit is needed when you use a marker.

**Do not hand-edit** the generated catalogue documents (`docs/test-catalogue.renderer.md`, `docs/test-catalogue.functional.md`, `docs/test-catalogue.unit.md`) — they are git-ignored and overwritten on every regenerate. Classify via the in-file `// OLO_TEST_LAYER` marker or `test_catalogue.json` instead.

---

## 3. When adding a new engine feature, add tests in lockstep

Every feature PR should land with tests at the layers dictated by the feature's surface area. Use this heuristic:

- **Has a mathematical / physical contract** (energy, monotonicity, identity, commutativity) → **L1 property test** is mandatory.
- **Adds a GLSL helper function** (`Srgb*`, `ToneMap*`, geometry math) → **L2 shader unit test** via `ShaderUnit_*.glsl` compute dispatch.
- **Introduces a new CPU↔GPU data type or serialized format** → **L3 round-trip test**.
- **Binds/unbinds GL state** (blend, depth, stencil, FBO, UBO, texture) → **L4 GLStateGuard region** wrapping the pass.
- **Adds a render-graph pass, resource, or dependency edge** → **L5 hazard-validation case**.
- **Is on the hot path** (per-frame, per-pass) → **L6 perf bench** with a baseline entry in `perf_baselines.txt`. Use the anti-flake retry helper.
- **Produces visible pixels** for a scene the user can point at → **L8 golden** with SSIM threshold. Only after L1–L4 already pin the contract; never use goldens as the primary correctness check.
- **Parses external data** (file, socket, YAML, SPIR-V, image) → **L11 fuzz harness** + seed corpus under `OloEngine/tests/Fuzzing/corpus/`.
- **Crosses subsystem boundaries** (a script writes a component Physics reads next tick; a save-game round-trips a body's state; a network snapshot reconciles transforms; an asset hot-reload mid-tick) → **Functional test** under `OloEngine/tests/Functional/<Subsystem>/`, tagged `"Functional"`. Pin one seam per file.

If a feature touches multiple surfaces, write one test per surface — and write Functional tests **in addition to** renderer tests when the surface crosses subsystem boundaries. Do not collapse them into a single mega-test; the taxonomy is how failures get diagnosed.

---

## 4. Anti-patterns to reject

- **Golden-image-only coverage.** Goldens catch *symptoms*. If the only test for a feature is an L8 golden, the PR is under-tested. Every feature needs at least one contract-level test (L1–L5).
- **Hand-written expected images / numeric constants with no citation.** Every "expected" value in L1 / L2 must either be derived in the test itself or carry a comment linking to the reference (paper, spec, Wikipedia formula, GLSL source line).
- **Flaky timing assertions.** L6 baselines must use the anti-flake retry helper and record the measurement on failure. Never use raw `EXPECT_LT(elapsed_ns, constant)` — go through the baseline path.
- **Tests that require a specific GPU vendor.** Anything beyond llvmpipe belongs in CI matrix work, not the default test binary.
- **Unregistered test files** — see §2; the pre-commit hook enforces this.

---

## 5. Running tests locally

- **Full suite:** VS Code task `run-tests-debug` (builds then runs).
- **Single test:** `./build/OloEngine/tests/Debug/OloEngine-Tests.exe --gtest_filter=SuiteName.TestName`.
- **Fuzz smoke:** build target `OloEngine-FuzzSmoke`, then run each harness with `-runs=N -max_total_time=T`.
- **Golden rebase** (only when a deliberate visual change lands): `OLOENGINE_GOLDEN_REBASE=1 <test binary> --gtest_filter=GoldenImage*`.
- **Perf rebase** (only when moving to new hardware or after an intentional optimisation): `OLOENGINE_PERF_REBASE=1 <test binary> --gtest_filter=PerfRegression*`.

Working directory matters: run from the repo root so asset paths resolve.

### Reproducing flaky CI-only core-starvation races

A race that passes 100/100 locally but flakes on CI is usually starved into existence by CI's **few, slow cores**. To recreate that pressure on a fat dev box, use [`scripts/repro-flaky-test.ps1`](../../scripts/repro-flaky-test.ps1): it loops a gtest binary + `--gtest_filter` under a restricted **CPU-affinity mask** (default 2 cores), with stall-based hang detection (the #281 deadlock signature) and `cdb` stack/dump capture on the first crash/hang. Two non-obvious levers, **both** needed for a faithful repro:

- **Affinity alone is not enough.** `FScheduler` sizes its worker pool from `std::thread::hardware_concurrency()`, which **ignores process affinity** — a 28-core box pinned to 2 cores still spawns ~28 workers, a different concurrency regime than a 2-core runner's ~2. Set env `OLO_TASK_GRAPH_NUM_WORKERS=N` (Scheduler.cpp) to pin the pool size to CI's count (verify: peak process thread count drops, e.g. ~63 → ~11). The CI-faithful combo is `OLO_TASK_GRAPH_NUM_WORKERS=2` + affinity `0x3`.
- **Density beats cold restarts.** Process startup (~2 s) dwarfs a single physics test (~6–25 ms), so add `--gtest_repeat=N` to pile up step density per launch instead of relying on fresh processes.

See the script header for parameters. Issue #281 (flaky Jolt physics-step crash) is the worked example.

---

## 6. Parallel-safety contract (`ctest --parallel`)

CI runs the suite with `ctest --parallel` ([`.github/workflows/Windows.yml`](../../.github/workflows/Windows.yml)). Because `gtest_discover_tests` registers **every** `TEST` / `TEST_F` case as its own ctest entry, each case runs in its **own `OloEngine-Tests.exe` process** — and under `-j`, *different cases of the same fixture run concurrently in different processes*. Two rules follow for every test you write:

### 6.1 Never share a mutable OS resource between test cases

A fixed filesystem path, a fixed network port, or a named OS object that a **second** test case also touches will race across processes. The classic bug is a fixture whose `SetUp` / `TearDown` / `TearDownTestSuite` does `remove_all()` (or `InvalidateCache`) on a **fixed shared** location: one process's teardown deletes a concurrent process's files mid-test.

**Fix — key any path you write by the process or the case**, mirroring the existing patterns:

- **By PID** — `temp_directory_path() / ("olo_..._" + std::to_string(_getpid()))`. See [ShaderBinaryCacheRoundTripTest.cpp](../../OloEngine/tests/Rendering/ShaderBinaryCacheRoundTripTest.cpp), [MeshBinarySerializerTest.cpp](../../OloEngine/tests/Serialization/MeshBinarySerializerTest.cpp), [ShaderPackTest.cpp](../../OloEngine/tests/Rendering/ShaderPackTest.cpp), [InputActionTest.cpp](../../OloEngine/tests/InputActionTest.cpp).
- **By gtest case name** — `temp_directory_path() / (test_info()->test_suite_name() + "." + test_info()->name())`. See [MeshAssetSerializerTest.cpp](../../OloEngine/tests/Serialization/MeshAssetSerializerTest.cpp), [AutoSaveTest.cpp](../../OloEngine/tests/AutoSaveTest.cpp), and the Functional Lua tests.

A fixed path used by **exactly one** case is fine — only one process ever touches it. Fixed ports are fine here only because the offenders either never bind (GNS uninitialised → `StartListening` returns false) or are in the workflow's `--exclude-regex` (`NetworkIntegrationTest`).

When you add a test that writes files, grep your new file for `temp_directory_path` and confirm the path is PID- or case-keyed.

### 6.2 Shared resources that can't be process-keyed → serialize in CMake

Some resources are **not** test-controlled and can't be PID-keyed — chiefly the engine's repo-relative **mesh cache** (`OloEditor/assets/cache/mesh/`, content-hash keyed). Tests that load the *same* model and call `MeshCache::InvalidateCache` race on the same `.omesh` file. These are serialized **against each other** with a shared ctest `RESOURCE_LOCK` in [OloEngine/tests/CMakeLists.txt](../../OloEngine/tests/CMakeLists.txt) (`OLO_MESH_CACHE_TESTS` → `RESOURCE_LOCK "olo_mesh_cache"`), applied via a filtered second `gtest_discover_tests` call. They still overlap freely with unrelated tests. **If you add a test that loads + invalidates a shared model cache, add its `Suite.Case` name to that filter.**

### 6.3 GPU and CPU oversubscription are tolerated, not isolated

- **GL-context tests** (`RendererAttachedTest` subclasses; the `*VisualEvidence*` / `*Bake*` / GPU-contract families) **SKIP** on CI (no GL 4.6 context), so they are no-ops under `-j` there. Locally they *run* and contend for the single GPU — expect them to be slow or to hit the per-test `--timeout` at high `-j`; that is a local artifact, **not** a CI failure. Verify GPU tests at a low `-j`.
- **Functional / physics tests** each start an `FScheduler` worker pool sized to `hardware_concurrency` ([FunctionalTest.cpp](../../OloEngine/tests/Functional/FunctionalTest.cpp)), so concurrent physics processes oversubscribe the cores. We deliberately do **not** serialize them: the workflow's `--repeat until-pass:3` nets the rare #281 Jolt race, and the suite runs stable under heavy local oversubscription (verified at `-j16` on a 28-core box ≈ 16×, well above CI's 4-core `-j4` ≈ 4×). If a future change makes physics flakier under `-j`, the lever is a ctest `RESOURCE_LOCK` / `PROCESSORS` property on those cases — not lowering `-j` globally.
- **Task / Async suites** ([TaskSystemTest.cpp](../../OloEngine/tests/Tasks/TaskSystemTest.cpp)) likewise each spin an `FScheduler` worker pool, so under `-j` you get *N* test processes × `hardware_concurrency` workers competing for the cores. They tolerate this and run on `Windows.yml`. **Lesson (don't repeat it):** this whole family was once excluded *wholesale* from the Windows test step on a vague "Timeouts + SEGFAULTs" note — a ~2-month coverage hole over the entire concurrency core. Re-classifying each suite under a **faithful runner squeeze** (4-core affinity + `OLO_TASK_GRAPH_NUM_WORKERS=4` to match the 4-vCPU runner's small worker fleet + `ctest --parallel 4`; see §5 "Reproducing flaky CI-only core-starvation races") showed **exactly one** genuinely-flaky case — everything else held up at 30× faithful + 20× a harsher 2-core squeeze. When a concurrency test flakes on CI, exclude the **specific failing case** with a tight regex and file an issue; never blanket-exclude the family — the rest is your only coverage. There are now **no** residual Task/Async exclusions. `CancellationTokenTest.MultipleTasks` (issue #359) used to be excluded for an intermittent **deadlock** under `--parallel` oversubscription; it is now fixed and re-enabled. The deadlock was *not* in the test body (the test passed — `[ OK ]` printed) but in scheduler teardown: `FWaitingQueue::StartShutdown` drained the standby-worker stack by walking `Node->Next` *in place* while triggering each node's event, and a woken `ConditionalStandby` worker — relying on its waker having mutated `m_StandbyState` (which the normal waker `TryStartNewThread` does by popping, but the in-place drain did not) — re-pushed itself onto a head it still owned, forming a `Node->Next == self` cycle that spun the drain (a tight `SetEvent` loop) forever with every worker already gone. Fix: drain via an atomic `m_StandbyState.exchange(StackMask)` (clear the head up front so re-push CASes fail) and read `Node->Next` *before* `Trigger()` (the ordering `Unpark` already uses). **Lesson for this scheduler family (UE5 `LowLevelTasks` port):** a node-stack waker that signals without changing the shared state head breaks the EventCount invariant that woken parkers re-validate against a changed state — always pop/clear atomically and capture `Next` before signaling.

### 6.4 GL error-state hygiene — leave `glGetError()` clean (issue #485)

Every GPU test in the binary shares **one** process-wide GL 4.6 context. A test that returns while `glGetError()` still has a pending flag silently corrupts a *later, unrelated* test: the victim's first GL readback drains the leaked error, misattributes it to its own call, and fails (or self-heals) for reasons unrelated to the victim. This is a real, hard-to-diagnose failure mode — the original case cost hours of bisecting (`SphereAreaLightShadowEvidenceTest` left `GL_INVALID_OPERATION` → `ProceduralSkyBakeTest` got a spurious all-black cubemap; production was fixed in 69aa9357 by draining stale errors in the readback helpers).

A **global gtest listener** ([`GLErrorStateCheck.{h,cpp}`](../../OloEngine/tests/Rendering/PropertyTests/GLErrorStateCheck.cpp), registered from `OloEngineTest.cpp::main`) now drains + asserts a clean error queue after **every** test's `OnTestEnd`. It runs for all tests regardless of fixture (most GPU tests are plain `TEST()` bodies gated on `OLO_ENSURE_GPU_OR_SKIP()`, not `TEST_F` subclasses, so a fixture `TearDown()` would miss them). It is a clean no-op when there is no live GL context (headless CI, where every GL test SKIPs), so it never fabricates a failure. On a leak it drains the queue (so the next test still starts clean) **and** fails the source test with `GL error state not clean after test: left <ENUM>`.

**The contract for any GPU test: leave `glGetError()` empty when you return.** Two failure shapes and their fixes, both already applied as worked examples:

- **Self-inflicted error** — a test that provokes a GL error must not leave it pending. Prefer removing the *cause*: `GLStateGuardTest.RestorePolicy_RestoresCoreStateOnDtor` used to bind an intentionally-**unlinked** program to give the guard a non-zero program to restore, but `glUseProgram` rejects an unlinked program with `GL_INVALID_OPERATION` *and leaves `GL_CURRENT_PROGRAM` at 0* — so the restore assertion passed vacuously AND leaked an error; it now binds a minimal **linked** program (`MakeMinimalLinkedProgram`), which both exercises the real non-zero restore path and raises no error. When an error genuinely can't be avoided, drain it before returning with `Utils::DrainGLErrors()` (`Platform/OpenGL/OpenGLUtilities.h`, a bounded drain).
- **Dangling binding left for a later test** — a test that binds a texture/buffer to a slot (`glBindTextureUnit`, `glBindBufferBase`) and then deletes the object, or a compute program via `glUseProgram`, must **unbind those slots before freeing the objects**. `GPUOcclusionCullGPUTest` bound its HZB to texture unit 0 + SSBOs to 15–19 + a UBO to 0 + the cull program, freed them, and left the slots dangling — which surfaced as a spurious `GL_INVALID_OPERATION` during the *next* full-scene render (`OccludedInstanceFieldScene`, `SceneRenders3D`, the EASU/AutoExposure/Nebula visual tests — all of which pass in isolation). The fix is `glBindTextureUnit(0, 0)` / `glBindBufferBase(..., 0)` / `glUseProgram(0)` before the `glDelete*` calls, not a downstream drain in the victim.

When you write a raw-GL test, the discipline is: **unbind what you bound and drain what you provoked, at the end of the test.** The listener will pin any slip to its source with a `[ FAILED ]` naming the leaked GL enum — fix the named test, don't drain in its victim.

### 6.5 Lazy-loaded GPU resources: a headless first-touch must not cache a non-renderable state (issue #520)

A process-global **lazy singleton that owns a GPU resource** is a second class of cross-test state leak — one that §6.4's GL-error listener and `GLStateGuard` cannot see, because the corruption is at the *asset* layer, not the GL-binding layer. The trap: a headless test (no GL context) *first touches* the singleton before any GPU test brings the context up. If the loader correctly skips GPU-resource creation when no context is bound (it must — `glCreate*` through a null glad pointer segfaults) **but caches the result as fully loaded**, every later GPU test reuses a permanently non-renderable resource. It passes in isolation (there the singleton is first loaded *with* a context) and fails only in a full-suite ordering — deterministically, which reads like a state leak rather than timing flake.

The worked case: `Font::GetDefault()` caches a `static Ref<Font>`; `SlugFontProcessor::Process` skips curve/band texture creation when `glad_glCreateTextures == nullptr`. `FontMeasureLineTest` (a plain metrics-only `TEST`) called `GetDefault()` first in the full-suite order, caching a **textureless** font; `RebindMenuScene.RendersRebindPanelAndProducesPng` then rendered its labels through that cached font (UI text falls back to `Font::GetDefault()`) and every glyph silently dropped — `maxLum 0.269`, the button-fill grey, no near-white text.

The fix is **not** a test-ordering workaround: make the resource *upgradeable*. Retain the CPU-side data when the GPU upload is deferred and upload it lazily the first time the resource is used with a context bound (`SlugFontProcessor::EnsureGpuTextures`, called from `Font::GetCurveTexture()`/`GetBandTexture()`), freeing the CPU copy afterward. **Guard rule for any new lazy GPU-resource loader: if you skip GPU creation because no context is bound, do not cache the result as final — either retain enough to finish the upload later, or don't cache until the upload succeeds.** Deterministic guard: `SlugDeferredUploadTest.EnsureGpuTexturesUploadsRetainedData` drives the upgrade path directly; the `RebindMenuScene` visual test remains the full-suite integration guard.

### 6.6 Deferred deletion of a still-*current* GL program is a landmine, not a leak (issue #625)

A third class of cross-test state corruption §6.4's listener does **not** catch at its true source: deleting a GL program while it is still the bound (`GL_CURRENT_PROGRAM`) object. Per spec, `glDeleteProgram` on a program that is currently in use only **flags** it for deletion — GL keeps the id valid and current until *something else* calls `glUseProgram` to switch away from it. If nothing does before the next test's render tick, the flagged-for-deletion program is still what `glGetIntegerv(GL_CURRENT_PROGRAM)` reports. The engine's shader teardown (`OpenGLShader`/`OpenGLComputeShader` destructors, and the hot-reload retire path) additionally routes `glDeleteProgram` through `FrameResourceManager::SubmitForDeletion` — a deferred queue drained a few frames later at `BeginFrame()` — so the flagged-for-deletion state can persist across several intervening tests, including headless ones that never touch GL at all.

The landmine springs when the deletion *finally* completes: `Utils::GLClearProgramGuard` (§ above, `docs/agent-rules/gl-clear-program-revalidation.md`) saves `GL_CURRENT_PROGRAM` around every clear and unbinds for the duration — and that unbind, if it's the first thing to touch the binding since the flagged program was left current, is what completes the deferred GL-side deletion. The guard's destructor then tries to *restore* the saved id, which is now genuinely gone → `GL_INVALID_VALUE ("Program handle does not refer to an object generated by OpenGL")`, raised inside `ShadowRenderPass::Execute`'s depth clear on whichever GPU test happens to render next. That test fails the §6.4 listener/`RendererAttachedTest` post-tick check — looking exactly like a leaked-error-queue flake, but no earlier test ever left a *pending GL error*; the corruption is a dangling **id**, several frames removed from whoever last used it. `UnderwaterCausticsVisualTest.CausticsBrightenAndTextureTheSeabed` failing only inside the `*AssetPack*:*Serializ*:*Texture*:*AssetSerial*` sweep (never standalone) was this exact chain — the true source was some earlier test's transient shader outliving its last bind with nothing rebinding before teardown, not a leaked line-width call or any single named "leaker" test.

Two-part fix, both defense-in-depth: (1) **`Utils::UnbindProgramIfCurrent(programId)`** (`Platform/OpenGL/OpenGLUtilities.h`) — call immediately before any `glDeleteProgram` that might still be current, including inside a `FrameResourceManager`-deferred lambda; it checks `GL_CURRENT_PROGRAM` and unbinds first so the deletion is immediate and deterministic instead of a dangling flagged-for-deletion id. Applied at all four program-deletion sites that can run while bound: `OpenGLShader`/`OpenGLComputeShader` destructors and their hot-reload retire paths. (2) **`GLClearProgramGuard`'s destructor now checks `glIsProgram()`** before restoring — defense in depth against *any* similarly-shaped bug reaching it, not just this one; skipping a restore of an invalid id just leaves `0` bound, which is safe since the next real draw binds its own program explicitly.

**Guard rule: never delete a GL object without first checking whether it is the currently-bound one on that binding point** — this applies to programs (`GL_CURRENT_PROGRAM`) here, but the same landmine shape exists for any object type with GL's use-then-defer deletion semantics (buffers bound to an indexed target, VAOs, textures bound to a unit) if something elsewhere ever tries to *restore* a saved binding rather than just re-set it unconditionally.

### 6.7 Headless `.comp` coverage in `ShaderHarness`: compute stages need a different shaderc target env (issue #627)

`ShaderHarness::EnumerateProductionShaders` walks both `.glsl` and `.comp` files. `.comp` files have no `#type` header — they're a single implicit compute stage, detected by `SplitStages` via a `local_size_x` substring match (added alongside `.comp` enumeration; every call site that used to hand-roll this "no `#type` found, but it declares a local_size" fallback now shares one implementation).

The harder problem: vertex/fragment/etc. stages compile under `shaderc_target_env_vulkan` because that's the real runtime path (`OpenGLShader::CompileOrGetVulkanBinaries`). Compute shaders do **not** go through that path — `OpenGLComputeShader` compiles GLSL natively via `glCompileShader`, no shaderc/SPIR-V step at runtime — and the engine's `.comp` files rely on two things the Vulkan SPIR-V target rejects outright:

- **Plain `uniform` declarations outside a block** (`uniform float u_Foo;`), legal core GLSL that the driver resolves per-program at link time. Vulkan target: `'non-opaque uniforms outside a block' : not allowed when using GLSL for Vulkan`.
- **No explicit `layout(location=L)`** on those loose uniforms — again resolved at link time via `glGetUniformLocation`, never authored by hand in this codebase's `.comp` files.

`shaderc_target_env_opengl` (SPIR-V under OpenGL/ARB_gl_spirv semantics) accepts the first but still demands the second explicitly; `options.SetAutoMapLocations(true)` closes that gap by having shaderc assign locations itself. So `ShaderHarness::CompileStageToSpv` branches on `kind == shaderc_glsl_compute_shader`: OpenGL target + auto-mapped locations for compute, Vulkan 1.2 (unchanged) for everything else. This is the *closest headless approximation* of the real compile path, not a byte-for-byte guarantee — but it's exact enough to catch real GLSL syntax errors and, critically, still produces SPIR-V for spirv-cross reflection (UBO sizes/offsets/member layout, binding indices), which is where the actual value is: a compute-side UBO redeclaration drifting from its C++ struct (the `FroxelFogScatter.comp`/`FroxelFogIntegrate.comp` acid test from #627 — `ShadowData`/`ForwardPlusParams`/`FroxelFogData` at bindings 6/25/46) is now caught by `ShaderUBOSizeConsistencyTest` exactly like a `.glsl` shader would be.

**Compute-dispatch texture/sampler bindings are inherently pass-local — don't require cross-shader type agreement.** Unlike a graphics material's texture set (which can persist bound across many draws), a compute dispatch explicitly (re)binds every texture it reads immediately before `glDispatchCompute`, matching the `TEX_USER_0/1/2` "engine rebinds between passes" rationale already documented in `ShaderBindingLayout.h`. `ShaderCrossConsistencyTest::SamplerBindingsHaveConsistentType` therefore skips every compute-stage sampler entirely (not just `TEX_USER_*`) — two different `.comp` files, or a `.comp` and a `.glsl`, legitimately declare the same low binding slot (0–9) with completely different sampler types (`sampler2D` vs `sampler3D`) with zero risk, since they're never simultaneously bound. `ShaderReflectionBindingTest`'s *name*-pattern check for those same low slots (`TEX_DIFFUSE`/`TEX_NORMAL`/etc. in `ShaderBindingLayout.h`) still applies to compute shaders — it's additive `||` name matching in the same style as every other documented pass-local reuse in that function, so a compute shader's texture name still needs a registered pattern (or exact-name entry) on whichever slot it reuses; only the *cross-shader type consistency* requirement is compute-exempt.

---

## 7. Relevant files

| Purpose | Path |
|---|---|
| Testing doc (philosophy + renderer pyramid L1–L11 + Functional axis) | `docs/testing.md` |
| Classification config (renderer + Functional) | `OloEngine/tests/scripts/test_catalogue.json` |
| Auto-catalogue generator | `OloEngine/tests/scripts/generate_test_catalogue.py` |
| Functional test harness | `OloEngine/tests/Functional/FunctionalTest.{h,cpp}` |
| GL error-state guard (§6.4) | `OloEngine/tests/Rendering/PropertyTests/GLErrorStateCheck.{h,cpp}` |
| Flaky-race repro harness (CPU-affinity + worker-count squeeze) | `scripts/repro-flaky-test.ps1` |
| Perf baselines | `OloEngine/tests/Rendering/PropertyTests/perf_baselines.txt` |
| Perf history + trend tool | `OloEngine/tests/Rendering/PropertyTests/perf_history/`, `OloEngine/tests/scripts/perf_trend.py` |
| Fuzz corpus | `OloEngine/tests/Fuzzing/corpus/` |
| ADRs | `docs/adr/0001-functional-tests-as-separate-axis.md`, `docs/adr/0002-headless-tick-default-for-functional-tests.md`, `docs/adr/0003-functional-tests-mount-editor-asset-root.md` |
| CI workflows | `.github/workflows/{cross-vendor,fuzz}.yml` |

If you are editing any of these, the pre-commit hook will revalidate.
