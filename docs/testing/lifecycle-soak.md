# Lifecycle soak (#1796)

One long-lived process walks real user workflows over two production scenes, again and again, on
the real renderer, and checks that nothing accumulates and nothing stale survives. It is the
application-level complement to the renderer state-machine harness (#1349), which drives
renderer-level operations over a scene built in code.

- **Per-PR smoke:** `LifecycleSoak.*` in the ordinary suite (one warm-up cycle, two measured
  cycles per leg), [LifecycleSoakTest.cpp](../../OloEngine/tests/Rendering/Lifecycle/LifecycleSoakTest.cpp).
  Every local `ctest` runs it on both backends: the GL pass under the `olo_sandbox_registry` lock,
  and, in a tree configured with `OLO_WITH_VULKAN` (the default on Windows), its own
  `OloEngine-Tests_VulkanLifecycleSoak` entry (`--olo-rhi=vulkan`). Hosted PR runners
  have no GPU, so there it skips and only `LifecycleSoakStats.*` runs; the AMD nightly's default
  filter includes it. It is not in `vulkan_scene_filter.txt`: on lavapipe every pipeline compiles
  cold at its first draw, the soak draws three render paths and two tiers every cycle, and one
  path's cold render of a Sandbox scene already took 26 minutes there (`EnginePipelineSeedTest`).
- **Long soak:** [scripts/lifecycle-soak.ps1](../../scripts/lifecycle-soak.ps1), a reproducible
  invocation, not a scheduled job (the owner declined a scheduled run on the RTX dev box on
  2026-10-18).
- **Owner:** drsnuggles8 (repository owner). Run the long soak after a change to scene activation,
  renderer shutdown/init, memory ownership or the render-path switch, and before a packaged KESTREL
  build (#1759).

## The sequence

Scene A is `SandboxProject/Assets/Scenes/ForwardPlusTest.olo` (authored Forward+, 25 entities, 16
local lights). Scene B is `ContactShadowTest.olo` (authored Deferred, HDR environment). Each
step is followed by three rendered frames.

| step | editor leg (loose assets) | runtime leg (cooked assets) |
|---|---|---|
| open A | `EditorLayer::OpenScene`: scene-swap reset, then `ApplySceneRenderSettings` | `ActivateScene`: load, stop old, `ApplySceneRenderSettings`, resize, `OnRuntimeStart` |
| play / stop | `Scene::Copy` + `OnRuntimeStart`, then `OnRuntimeStop` + scene-swap reset | skipped: the runtime has no edit mode |
| mutate entities | spawn eight mesh entities, then destroy them | same, in the running scene |
| resize | 480x270 to 640x360 and back | same |
| quality tier | Low, then Ultra, as `RendererSettingsPanel` applies a preset | same |
| render path | Forward, Deferred, Forward+ | same |
| reload an asset | `ShaderLibrary::ReloadShaders` | same |
| switch to B, return to A | open B, open A | activate B, activate A |
| **checkpoint** | A against A's control (image + metadata), memory sample | same |

Once at the end of a run: unload, `Renderer3D::Shutdown`, an owned-state census, `Init`, open A
and compare again. Skipped transitions, with their reason, are in each report's `skipped` list:
MSAA and upscale belong to the state-machine harness, and play/stop does not exist in the runtime.

Cells the soak does not run, and why:

- **Editor with cooked assets:** the editor never loads a cooked game; cooked content is the
  runtime's by definition.
- **OloRuntime as a real process, restarted:** the executable's only flag is `--smoke-test`, which
  skips the `RuntimeLayer` and the renderer. It takes no scene, frame count or exit request, so a
  process-level load/unload/restart cycle cannot be driven. The runtime leg's restart is a renderer
  `Shutdown`/`Init` in one process.
- **OloServer:** it has no renderer; `AppLaunchSmoke.OloServerStopsAHeadlessSceneCleanly` covers its
  only lifecycle step.

The editor leg uses the Sandbox project with an `EditorAssetManager`. The runtime leg uses the game
directory `GameBuildPipeline::StageRuntimeContent` lays out and a `RuntimeAssetManager`, as
OloRuntime's `MountGameProject` does. OloRuntime's `RuntimeLayer` is compiled into the executable
only, so the test mirrors its `ActivateScene` call for call instead of calling it.

## What is checked

- **A after every return matches A's control**: the per-tile mean shift and the fraction of
  changed pixels, each within four times the control's own noise floor (A rendered twice), with
  absolute minimums of 1/255 and 0.2 %. The renderer's path, bloom, FXAA and shadow resolution
  must equal A's too. Pinned by `LifecycleSoakStats.*`.
- **GPU resident memory does not grow** across the measured cycles: the renderer's own ledger
  (`RendererMemoryTracker`), read after the deletion queues are drained, within 1 MiB. Retiring
  bytes before the drain are recorded too.
- **Process resident memory** is recorded and judged within 64 MiB, because the heap, the driver
  and the shader cache keep pages a process does not give back.
- **The restarted renderer draws A as A**, and its `Shutdown` leaves no history, pose records,
  hand-offs or GPU-owning statics.
- **No Vulkan validation error** (the fixture's tick already fails on one).
- **A fresh-process control** (long soak only): A after the soak against A written by a process
  that only opened it.
- **The negative control**, `LifecycleSoak.MissingActivationResetIsCaught`: the
  `OLO_FAULT_SKIP_SCENE_RENDER_SETTINGS_PUBLISH` lever makes `ApplySceneRenderSettings` publish
  nothing (the missing activation reset of #1563 and #1601). The image and metadata comparisons
  must both catch it.

## The report

`--olo-lifecycle-soak-report=<path>` writes `<stem>_<leg>_<backend>.json` per leg. The fields
(schema `olo-lifecycle-soak 1`): the build, the sequence's operations and skips, executed counts
per operation, every cycle's frame index, GPU resident and retiring bytes, process resident bytes,
its A comparison and frame-time distribution, the run's p50/p95/p99, the growth verdicts, the
after-restart and fresh-process comparisons, and the Vulkan validation count. The sequence is fixed
(`seed` 0), so the report and the binary's commit are enough to replay a run.

Frame times are wall-clock time per tick on the CPU, including any GPU wait the tick makes. They
describe this sequence's cost, not a frame budget.

## Coverage manifest

What existed before this soak, and what it adds. "One process, repeated" means the step runs
several times in one process with the real renderer.

| lifecycle step | existing coverage (before #1796) | added by the soak |
|---|---|---|
| load a scene file | evidence tests that load one scene once (`AcceptanceSceneEvidenceTest`, `TemporalLongSequenceEvidenceTest`, `FloraLooseCookedParityEvidenceTest`); live `olo_scene_open` | A and B through both hosts' activation, every cycle, both backends |
| play / stop | headless only (`PhysicsRestartIsCleanTest`); live scripts; no rendered test calls `OnRuntimeStart`/`Stop` | editor Play on a copy and Stop, every cycle, rendered |
| mutate entities | harness `EntityChurn`, `SceneReload` (#1349, GL) | spawn and destroy in a loaded scene, both backends |
| resize | harness `Resize` (GL); `MultiViewLifetime.*`, `RendererMemoryAdmission*` (both) | every cycle, both backends |
| quality tier change | none (`QualityTieringTest` is CPU only) | Low and Ultra every cycle, restored by the next activation |
| render path switch | harness `Path` (GL); `RenderGraphPathSwitchTests` (CPU) | Forward, Deferred, Forward+ every cycle, both backends |
| asset reload | harness `ShaderReload` (GL); one texture reload (`TextureInPlaceReloadTest`) | shader reload every cycle, both backends |
| switch to B, return to A | harness `SceneSwap` (one frame of a code-built scene, GL) | file-loaded B then A, compared against A's control |
| unload, shutdown | `RendererOwnership.RestartStartsFromAFreshViewSceneFrameAndClock` (both); `AppLaunchSmoke` (no renderer) | after a soak: unload, Shutdown, census, Init, A again |
| cooked runtime load/unload/restart | parity of one cooked scene (`FloraLooseCookedParityEvidenceTest`); pack round trips without rendering | `ActivateScene` order over the staged game, every cycle, plus the restart |
| repeated cycles with memory and frame times | none per cycle; `--olo-rss-report` per test on the AMD nightly | GPU ledger and process memory per cycle, growth verdicts, p50/p95/p99 |
| A against a fresh control | harness `fresh-vs-sequence.gl` (in-process reset); live `renderer-state-machine-live.py` | in-process control every cycle, after restart, and a fresh-process control |
| #1492 | the harness's comparison guards (rejection reasons, non-finite values), not a replay feature | uses its rule that a missing measurement is never a tolerance |
| OloServer | `AppLaunchSmoke.OloServerStopsAHeadlessSceneCleanly`; it has no renderer | no lifecycle step adds coverage beyond that |

#1538 and the existing synchronization bugs keep their own owners; the soak does not allow them.
