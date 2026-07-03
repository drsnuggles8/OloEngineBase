# Perf stress-scene battery — findings (2026-07-03)

First full run of the perf stress battery (see
[docs/guides/perf-stress-scenes.md](../guides/perf-stress-scenes.md) for the
generator + driver). Machine: Windows 11, RTX 4090, NVMe. Build: **Dist**,
viewport 1920×1080 (`olo_viewport_set_size`), 20 s settle, 3 samples averaged
over MCP (`olo_perf_snapshot`, `olo_perf_pass_timings`). Play-mode scenes
entered Play via `OLO_EDITOR_AUTOPLAY=1`; a per-scene **workload probe**
(translation sampled twice) verified the workload was actually running —
several axes initially measured "great" numbers because the workload silently
wasn't engaged (see Harness lessons).

## Results

`load s` = editor launch → MCP ready (includes scene deserialize; baseline ≈2 s).
`probe` = designated moving entity still moving at sample time.

| scene | entities | load s | fps | frame ms | gpu ms | ShadowPass ms | ScenePass ms | draws | probe |
|---|---|---|---|---|---|---|---|---|---|
| baseline_pinkcube | 2 | 1.9 | 596 | 1.9 | 0.6 | 0 | 0.1 | 18 | — |
| overdraw_sheets_80 | 82 | 3.5 | 275 | 3.7 | 1.5 | 0 | 0.1 | 19 | — |
| scripts_swarm_cs_2000 | 2 003 | 2.4–5.0 | 313–639 | 1.6–3.7 | 0.8 | 0 | 0.1 | 22 | moving |
| anim_crowd_128 | 131 | 3.2 | 85 | 12.2 | 4.1 | 1.8 | 1.3 | 362 | — |
| scripts_swarm_5000 | 5 003 | 3.5–13.7 | 65–113 | 8.9–18.2 | 2.0 | 0 | 0.2 | 22 | moving |
| ecs_static_100000 | 100 002 | 24.1 | 53 | 19.0 | 2.8 | 0 | 0.3 | 18 | — |
| anim_crowd_200 | 203 | 7.1 | 34 | 29.8 | 3.8 | 2.1 | 1.8 | 362 | — |
| draws_instanced_200000 | 3 | 14.3 | 28 | 36.0 | 18.4 | 0 | 1.4 | 19 | — |
| lights_many_512 | 1 415 | 2.1 | 23 | 44.0 | 41.8 | 13.8 | 27.1 | 3 622 | — |
| kitchen_sink_1000 | 7 863 | 4.9 | 16 | 63.4 | 52.0 | 4.3 | 45.6 | 1 024 | static (settled) |
| physics_pile_1000 | 1 007 | 2.3 | 18 | 98.8 | 20.5 | 19.3 | 0.5 | 4 038 | — |
| physics_stacks_5000 | 5 007 | 17.9 | 10 | 102.3 | 87.8 | 85.4 | 0.7 | 20 038 | — |
| lights_shadowed_64 | 967 | 2.1 | 7 | 142.6 | 139.6 | 96.0 | 42.4 | 25 246 | — |
| physics_sleeping_10000 | 10 007 | 12.3 | 4.7 | 214.1 | 188.4 | 188.8 | 0.9 | 40 038 | — |
| draws_unique_10000 | 10 003 | 7.8 | 4.4 | 226.7 | 169.7 | 161.4 | 7.7 | 40 022 | — |
| physics_pile_10000 | 10 007 | 12–42 | 0.6 | 1 616 | 168.5 | 165.5 | 1.5 | 40 038 | moving (**through the floor**) |
| physics_pile_50000 | 50 007 | 53–59 | 0.5 | 1 977 | 1 286 | 1 091 | 194 | 200 038 | — |

Raw per-scene JSON (samples, pass timings, memory report, screenshots, engine
logs): `%TEMP%\olo-perf-battery\full2\` for this run — regenerate any time with
the driver.

## Findings, ranked

### 1. ShadowRenderPass runs at full cost with zero shadow-casting lights (#522)

Every battery scene sets the sun's `CastShadows: false`, yet ShadowPass is the
**largest GPU consumer in almost every mesh scene** (161 ms of the 170 ms GPU
frame at draws_unique_10000; 1.09 s of 1.29 s at physics_pile_50000) and
quadruples CPU submission (draw calls = 4 cascades × geometry). Cause: casters
are submitted unconditionally and `ShadowRenderPass::Execute` only checks the
global `ShadowMap::IsEnabled()` — `Scene::RenderScene3D` computes CSM matrices
only when a light casts shadows (Scene.cpp:4135), so the pass renders against
stale matrices. `InstancedMeshComponent.CastShadows: false` does bypass it
(draws_instanced shows ShadowPass 0) — the per-light flag is what's broken.
*Possible overlap with `feature/shadow-pcss-performance` (PCSS cost when
shadows are ON) — this is a distinct defect: cost when shadows are OFF.*

### 2. Jolt contact overflow at 10k bodies → objects fall through the world (#523)

`physics_pile_10000` spams `Jolt physics update error: 4`
(`ContactConstraintsFull`) and the probe body was at **y=−36, still falling,
through a valid static floor**. `JoltScene.h` hardcodes
`s_MaxContactConstraints = 10240` (`constexpr`, not configurable) even though
`PhysicsSettings::m_MaxContactConstraints` exists and `Physics3DSystem` honours
it — the two init paths disagree. At 1k bodies the same scene settles
correctly.

### 3. FrameDataBuffer per-frame caps overflow ungracefully + unthrottled log spam (#524)

- **4096 bone matrices** (~170 characters @ 24 bones): anim_crowd_200 drops
  meshes and logs 2 lines/mesh/frame — **126 410 error lines in one session**,
  stalling the main thread enough that MCP marshals timed out.
- **16384 per-instance Color/Custom/EntityID slots**: draws_instanced_200000
  logs `Requested 200000 … capacity 16384` per frame; picking IDs return −1.
- **1024 unique materials/frame**: draws_unique_10000 overflows the material
  table; out-of-range indices render the default material.

### 4. Scene YAML load time at scale (#525)

24 s for 100k transform-only entities; 53–59 s for the 50k physics pile
(30 MB YAML). Per-entity deserialize is the floor; anim scenes additionally do
`Ref<AnimatedModel>::Create` **per entity** (no sharing by source path). A
bulk/binary path or runtime spawner is the fix direction.

### 5. Per-axis costs that are real but un-attributable in Dist (evidence posted to #519)

- **ecs_static_100000**: 19 ms frame with 2.8 ms GPU and ~0 render CPU — the
  ~16 ms (spatial-index rebuild over all entities every tick, Scene.cpp:1288 +
  editor bookkeeping) is invisible to every MCP tool. Confirms the known
  static-review finding but can't be split without `olo_perf_cpu_scopes`.
- **anim_crowd_128** (below the bone cap): ~8 ms CPU/frame for 128 playing
  foxes (per-bone string-map lookups, AnimationSystem.cpp:158-166 suspected).
- **scripts_swarm_5000 (Lua)**: +7.5 to +16.3 ms over baseline across two
  verified runs for 5 000 trivial scripts ≈ **1.5–3.3 µs/script/frame** (one
  get+set translation round trip; notable run-to-run variance).
- **scripts_swarm_cs_2000 (Mono)**: +0.2 to +2.3 ms for 2 000 scripts ≈
  **≤ ~1 µs/script/frame** — the C# crossing measures cheaper than the Lua
  path per call, but the signal is close to frame-time noise; pinning it down
  needs `olo_perf_cpu_scopes` (#519). Both swarms verified actually running
  via the translation probe in every run.
- **physics_sleeping_10000**: after settling, frame is 214 ms of which ~189 ms
  is finding-1's stale ShadowPass; the sleeping-body writeback the scene
  targets is inside the remaining ~25 ms CPU but unmeasurable in Dist.

### 6. What is *not* a problem on this hardware

- **Overdraw/fill rate**: 80 full-screen alpha-blended quads = 1.5 ms GPU
  (Renderer2D batches them into one draw).
- **GPU instancing**: 200k instances render in 19 draws; the 36 ms frame is
  CPU-side instance-data handling + ~17 ms unattributed GPU (cull/upload
  outside timed passes), not raster.
- **Forward+ scaling**: 512 unshadowed point lights = 27 ms ScenePass on a
  900-receiver field — heavy but linear-ish; 64 *shadowed* point lights are the
  real cliff (96 ms ShadowPass + 42 ms ScenePass).

## Harness lessons (baked into the generator/driver)

- Workloads must be **verified running**, not assumed: sprites don't render at
  all in the Edit-mode 3D viewport (overdraw scene needs Play mode); script
  swarms initially measured near-baseline before ScriptCore/Sandbox dlls were
  in place. The driver's probe column + per-scene log snapshot catch this.
- `BoxCollider3DComponent.HalfExtents` are **mesh-local** (multiplied by
  transform scale); getting it wrong silently substitutes a default shape.
- Edit-mode scenes are viewed through the **editor camera** — the driver poses
  it from the manifest (`olo_camera_set_pose`).
- No MCP scene-open or play/stop control exists (both logged on #306) — the
  driver edits `Sandbox.oloproj` StartScene and relaunches per scene; the
  editor briefly holds the file on shutdown (writes are retried).
- MCP `MarshalRead` times out on heavily stalled main threads (log-spam or
  sub-1-fps scenes) — samples degrade to null rather than failing the run.

## Tooling gaps logged

- #306: `olo_scene_play`/`olo_scene_stop` (posted 2026-07-03); `olo_scene_open`
  (already listed — noted there that the battery driver should be reworked to
  keep one editor instance and switch scenes over MCP once it lands).
- #519: fresh evidence for `olo_perf_cpu_scopes` + request for read-only
  `olo_physics_stats` (bodies active/sleeping, step ms, update-error flags)
  (posted 2026-07-03).
