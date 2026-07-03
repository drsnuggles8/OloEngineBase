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
| Play-mode automation | `OLO_EDITOR_AUTOPLAY=1` env var (EditorLayer, same opt-in spirit as `OLO_MCP_AUTOSTART`) | yes |

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

Per scene the driver: edits `Sandbox.oloproj` StartScene (restored afterwards),
launches the editor with `OLO_MCP_AUTOSTART=1` (+ `OLO_EDITOR_AUTOPLAY=1` for
play-mode scenes), measures launch→MCP-discovery wall time (startup + scene
load), sets the viewport to 1920×1080 (`olo_viewport_set_size` — do **not**
maximize the window, the override fights a maximized panel), settles, then
averages `olo_perf_snapshot` and `olo_perf_pass_timings` over N samples and
saves `olo_perf_frame_history`, `olo_memory_report`, shader/script error
probes, a screenshot, and the engine log per scene. For edit-mode scenes it
poses the **editor camera** from the manifest (`olo_camera_set_pose` — the
in-scene camera only drives Play mode); for scenes with a `probeTag` it
samples that entity's translation twice and reports a `probe` column — a
workload that silently isn't running otherwise reads as a great fps number.
First full-run findings: [docs/analysis/perf-stress-findings-2026-07.md](../analysis/perf-stress-findings-2026-07.md). Output: `results.md` +
`results.json` + PNG/log per scene, default under `%TEMP%\olo-perf-battery\<stamp>\`.

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
- **No `olo_scene_open` / play-mode MCP tool yet** (#306) — hence the
  StartScene-edit + relaunch loop and the `OLO_EDITOR_AUTOPLAY` env var.
- `scripts_swarm_cs` needs the VS-generator C# target: build
  `Sandbox-Scripting` (the scene loads with script errors otherwise).
- The editor truncates `OloEngine.log` per launch; the driver snapshots it per
  scene into the results dir.
- Scene-load wall time at high entity counts is itself a finding (tracked in
  the results table `load s` column), not a harness defect.
