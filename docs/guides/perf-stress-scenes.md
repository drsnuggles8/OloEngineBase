# Performance stress scenes — generator + measurement runbook

A systematic battery of `.olo` scenes that each isolate one axis of frame cost
that normally shows up in games (physics step, ECS bookkeeping, draw
submission, instancing, lighting, skinned animation, script interop, fill
rate), plus a repeatable MCP-driven measurement loop. Built to make the known
CPU hot spots measurable (`Ref<T>` registry churn, `Scene::UpdateSpatialIndex`
full rebuild, sleeping-body Jolt→ECS writeback, per-bone string-map lookups)
and to surface new ones.

## Pieces

| piece | path | committed |
|---|---|---|
| Scene generator | [OloEngine/tests/scripts/generate_perf_scenes.py](../../OloEngine/tests/scripts/generate_perf_scenes.py) | yes |
| Generated scenes + manifest | `OloEditor/SandboxProject/Assets/Scenes/PerfStress/` | **no** (git-ignored; 200k-instance scene is ~24 MB) |
| Measurement driver | [scripts/perf/run-perf-battery.ps1](../../scripts/perf/run-perf-battery.ps1) | yes |
| Trivial per-frame scripts | `SandboxProject/Assets/Scripts/LuaScripts/PerfBob.lua`, `Scripts/Source/PerfBob.cs` | yes |
| Headless write-consent opt-in | `OLO_MCP_ALLOW_WRITES=1` env var (EditorLayer, same opt-in spirit as `OLO_MCP_AUTOSTART`) | yes |

## Scene battery

`python OloEngine/tests/scripts/generate_perf_scenes.py --list`:

| scene | default N | play mode | isolates |
|---|---|---|---|
| `physics_pile` | 10000 | yes | Jolt step + Jolt→ECS writeback (falling bodies in an arena) |
| `physics_sleeping` | 10000 | yes | sleeping-body writeback (bodies settle → sleep) |
| `physics_stacks` | 5000 | yes | contact/solver load (towers of 10) |
| `ecs_static` | 100000 | yes | spatial-index rebuild + hierarchy-panel floor (transform-only) |
| `draws_unique` | 10000 | no | per-draw CPU submission; unique material per entity defeats batching |
| `draws_instanced` | 200000 | no | GPU instancing path (one `InstancedMeshComponent`; GPU cull ≥1024) |
| `lights_many` | 512 | no | Forward+ light binning/shading, unshadowed |
| `lights_shadowed` | 64 | no | shadowed point lights |
| `anim_crowd` | 200 | yes | AnimationSystem per-bone lookups + pose copies (Fox, all playing) |
| `scripts_swarm` | 5000 | yes | Lua interop crossing (one get/set translation per tick) |
| `scripts_swarm_cs` | 2000 | yes | Mono interop crossing (needs Sandbox-Scripting.dll built) |
| `overdraw_sheets` | 80 | yes | fill rate / blending (stacked full-screen alpha quads; world sprites only render on the runtime path) |
| `kitchen_sink` | 1000 | yes | combined "game level" of all axes, shadows ON |

Sizes are parameterized: `--count 50000` or `--counts 1000,10000,50000`.
Directional-light shadows are **off** in every isolation scene on purpose —
PCSS is a known dominator (~93 % of ScenePass) and would mask every axis;
`kitchen_sink` turns them back on.

## Runbook

```powershell
# 1. generate (seconds; re-run any time — output is git-ignored)
python OloEngine/tests/scripts/generate_perf_scenes.py --scene all

# 2. build the measurement config
cmake --build build --target OloEditor --config Dist --parallel

# 3. run the battery (interactive desktop session required — GL + PrintWindow)
pwsh -File scripts/perf/run-perf-battery.ps1
#    subset / knobs:
pwsh -File scripts/perf/run-perf-battery.ps1 -Scenes physics_pile_10000 -SettleSeconds 30 -Samples 5
```

The driver launches **ONE** OloEditor instance for the whole battery and
switches scenes over MCP with the consented-write scene-control tools
(`olo_scene_open` / `olo_scene_play` / `olo_scene_stop`, #316 Part 5) instead
of relaunching the editor per scene — a StartScene-edit + relaunch loop was
the original (2026-07-03) design, before scene control existed. Launch sets
`OLO_MCP_AUTOSTART=1` + `OLO_MCP_ALLOW_WRITES=1` (the session-level write
consent opt-in these tools need — off by default, so a headless launch would
otherwise refuse every mutation) + `OLO_EDITOR_AUTOSAVE_RECOVERY=original`
(pre-answers the recovery modal a headless session could never click). Per
scene: `olo_scene_open` (timed — this **is** the scene-load-time metric now,
scoped to just the load/switch, not a full editor cold-boot), `olo_scene_play`
if the scene needs Play mode (`olo_scene_open` already stops any previous
scene's Play mode, so no explicit `olo_scene_stop` between scenes), sets the
viewport to 1920×1080 (`olo_viewport_set_size` — do **not** maximize the
window, the override fights a maximized panel), poses the **editor camera**
from the manifest for edit-mode scenes (`olo_camera_set_pose` — the in-scene
camera only drives Play mode), settles, then averages `olo_perf_snapshot` and
`olo_perf_pass_timings` over N samples and saves `olo_perf_frame_history`,
`olo_memory_report`, shader/script error probes, a screenshot, and a
per-scene **log delta** (`OloEngine.log` now grows across the whole run since
there's no per-scene relaunch to truncate it; the driver tracks a byte offset
and saves only what each scene appended). For scenes with a `probeTag` it
samples that entity's translation twice and reports a `probe` column — a
workload that silently isn't running otherwise reads as a great fps number. A
scene that fails to open/play is recorded as an error row and the run
continues (no relaunch to fall back to). First full-run findings:
[docs/analysis/perf-stress-findings-2026-07.md](../analysis/perf-stress-findings-2026-07.md).
Output: `results.md` + `results.json` + PNG/log-delta per scene, default under
`%TEMP%\olo-perf-battery\<stamp>\`.

**Always eyeball the per-scene PNGs and logs in the results dir** — the probe
column catches non-moving workloads, but only the screenshot catches a
mis-framed camera, dropped meshes (capacity overflows), or material
corruption; and the engine log is where capacity-limit errors surface.

## Caveats / known limits

- **CPU-side per-system breakdown is not measurable over MCP in Dist**:
  `PerformanceProfiler` isn't exposed as a tool and `OLO_PERF_SCOPE` compiles
  out in Dist. Physics/ECS/script scenes therefore show up only as frame-total
  deltas; for a per-system view run `-Config Release` and read the Statistics
  panel (Performance tab) via `olo_screenshot`. Tracked on #316.
- `scripts_swarm_cs` needs the VS-generator C# target: build
  `Sandbox-Scripting` (the scene loads with script errors otherwise).
- Scene-load wall time at high entity counts is itself a finding (tracked in
  the results table `scene open s` column), not a harness defect.
- **`MarshalRead`'s tool-dispatch watchdog defaults to 5 s** — plenty for a
  query tool, nowhere near enough for a full scene load/copy at these entity
  counts. `Handle_SceneOpen` / `Handle_ScenePlayState` (McpTools.cpp) now pass
  an explicit 120 s `kSceneControlTimeout`; every other MCP tool keeps the 5 s
  default on purpose (a slow *query* tool should hang loudly, not silently
  block behind a growing single-threaded queue). Discovered by this driver:
  a naive design that treats a timed-out `olo_scene_open` as "failed" and
  immediately queues the next scene compounds a backlog that never
  recovers, because the timed-out job keeps running on the game thread
  regardless (nothing dequeues it) — every later call queues behind it. If
  you see a run-wide cascade of `"Timed out waiting for the editor main
  thread"` starting at one scene and never recovering, this is the failure
  mode; check `kSceneControlTimeout` is still wired to both handlers.
- **Session-cumulative physics degradation**: `physics_pile_50000` measured
  markedly worse (8.1 s vs. 2.0 s frame time, Jolt error code 7 vs. 4 — see
  `EPhysicsUpdateError`) run as the Nth scene in a long-lived session versus
  as the first/only scene in a fresh process. Plausible cause: Jolt state
  (or the fixed-timestep catch-up loop) compounding across several
  physics-heavy scenes in one session rather than starting cold each time.
  Not chased further here — the qualitative finding (#523, contact-constraint
  overflow at ~10k+ bodies) holds either way, but **absolute numbers for the
  heaviest physics scenes are session-order-sensitive** in the one-instance
  design; treat them as a lower bound, not an exact figure, and rerun a
  suspect scene standalone (`-Scenes physics_pile_50000 -SkipBaseline`) for
  a cleaner read.
