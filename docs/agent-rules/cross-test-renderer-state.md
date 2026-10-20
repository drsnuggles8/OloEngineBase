# Cross-test renderer state

Applies to: any test that shuts down a renderer singleton, selects its own device, changes renderer configuration, or must be diagnosed when it fails only in the full single-process run (issue #1074).

## Rules

1. **Never leave a process-wide renderer singleton dead.** `Renderer3D::Init` owns `CommandDispatch`, `GPUPassTimerPool`, `ParticleBatchRenderer` and `MeshPrimitives`. A test that tears one down leaves every later rendering test running against a dead one. This, not configuration leakage, caused #1074. [Details](cross-test-renderer-state-details.md#cross-test-renderer-state)
2. **Do not fix it by guarding every `Shutdown()` on whether you brought it up.** A test that creates its own Vulkan device must still release backend-owned resources before destroying it, or VMA aborts ("Some allocations were not freed before destruction of this memory block"); inheriting the renderer's timer pool also made `GraphRecordsSharedUnboundUniformAndTimesOrderedPasses` read 18 timings where it asserts 2. Before guarding a `Shutdown()`, ask what backend owns the memory it releases. [Details](cross-test-renderer-state-details.md#why-just-dont-shut-them-down-is-the-wrong-fix)
3. **Keep the teardown unconditional and rely on the lazy self-heal.** `Renderer3D::BeginScene` calls `ReclaimSharedRenderState()`, which runs `RepublishCommandDispatchBindings()` if `!CommandDispatch::HasUBOReferences()` and `Initialize()` on the `GPUPassTimerPool` if not initialized, on the renderer's own backend. Do not republish eagerly: it leaves GL-currency handles in the dispatcher across the Vulkan suites that follow (see `VulkanPassSuiteTest.cpp`). [Details](cross-test-renderer-state-details.md#why-just-dont-shut-them-down-is-the-wrong-fix)
4. **Free on the backend that created it.** The timer pool's queries are GL objects, so its `Shutdown()` belongs before the Vulkan selection, not inside it. [Details](cross-test-renderer-state-details.md#cross-test-renderer-state)
5. **Use the ownership test for shared singletons,** as for `FrameDataBufferManager`: `const bool ownsFrameData = !FrameDataBufferManager::IsInitialized();`, init if so, and only `Shutdown()` if you own it. [Details](cross-test-renderer-state-details.md#why-just-dont-shut-them-down-is-the-wrong-fix)
6. **Leave the process-global renderer configuration as you found it.** The rendering path, settings structs and culling toggles are one static. `RendererStateCheck` (a GoogleTest listener) restores them after every test and names leakers in a `[ RENDERER STATE ]` summary; `--olo-strict-renderer-state` fails the polluting test at its source location. A leak is still a bug in your test. [Details](cross-test-renderer-state-details.md#the-configuration-guard-rule-2)
7. **Add a new `Renderer3D` settings struct to `RendererState::Snapshot` and the `OLO_RENDERER_STATE_STRUCT_ENTRIES` X-macro.** One entry drives capture, comparison and restore. Snapshot inputs, not derived values (`s_Data.DepthPrepassEnabled` is recomputed by `ApplyRendererSettings`). The restore ends with `ApplyRendererSettings()` because `ActiveGraphPath` selects the pipeline. [Details](cross-test-renderer-state-details.md#the-configuration-guard-rule-2)
8. **Capture the working directory after `RendererAttachedTest::SetUp()`.** The first GPU fixture in a process moves into `OloEditor/`; capturing earlier restores the repo root and later GPU suites fail to open shaders (`FoliageGPUCullEvidenceTest` after `FloraLooseCookedParityEvidenceTest`, #1391). [Details](cross-test-renderer-state-details.md#cross-test-renderer-state)
9. **Remember CI cannot see this class.** `gtest_discover_tests` runs one process per test in CI; only the monolithic single-process run (~7,600 tests sharing one `Renderer3D::s_Data` and GL context) has it, and it is not in the PR gate. [Details](cross-test-renderer-state-details.md#why-this-class-exists-at-all)
10. **Recognise the signature:** a visual-evidence test whose feature-on and feature-off captures are identical (differential near 0, e.g. `LightmapBleedRoom` 0.0007 vs needing 3.0; `VirtualGeometryVisualEvidence` 107 red pixels vs 800), passing in isolation, with identical engine logs. [Details](cross-test-renderer-state-details.md#the-failure-signature-and-why-it-points-nowhere-near-the-cause)
11. **A bisection step must check that the victim fails the same way,** not merely that it fails. Pin a signature from the full run (10 failures, 0 SEH, red-pixel counts 107–118), reject non-matching arms, and print the mode every step; a reduced configuration reproduced SEH 0xc0000005 access violations, a different bug. [Details](cross-test-renderer-state-details.md#trap-1--a-reduced-configuration-reproduces-a-different-bug)
12. **Do not conclude "not reproducible" from a minimal pair.** Two suites (`FogVolumeSelfShadowVisualEvidenceTest`, `VulkanParallelRecordingDevice`) were each necessary and neither sufficient; the repro needed both plus ~150 other suites. [Details](cross-test-renderer-state-details.md#trap-2--one-poisoner-is-not-enough)
13. **Do not assume a configuration leak is the cause.** The guard found 212 leaking tests (scene-published per-frame state: cloudscape, underwater fog, three water fields) and restoring them changed the failure count by 0. [Details](cross-test-renderer-state-details.md#the-configuration-guard-rule-2)

## Commands and checks

```powershell
# The whole binary in ONE process — the only configuration where this exists.
.\build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe

# Same, but fail every test that leaks renderer configuration.
.\build-cached\OloEngine\tests\Debug\OloEngine-Tests.exe --olo-strict-renderer-state
```

About 20 minutes; it holds the test binary (a concurrent rebuild dies `LNK1104`) and runs `AppLaunchSmoke`, which launches the real editor.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[cross-test-renderer-state-details.md](cross-test-renderer-state-details.md).
