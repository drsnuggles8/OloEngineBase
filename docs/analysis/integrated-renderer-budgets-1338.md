# Integrated renderer measurements (#1338)

Measured on 2026-09-23 with the generated `IntegratedRenderer.olo` scene and the
manifests in `OloEditor/assets/benchmark/manifests/`. The scene combines the
AnimalPopulation fox herd and grooms, Meadow terrain and grass, four local
lights, directional shadows, and an OceanCoast water patch. One fox and one
shadowing light move on specified trajectories. Each manifest measures 180
frames at a stationary camera, 180 during a dolly, and 180 during a rapid turn,
after 64 or more warm-up frames per camera. Each OpenGL row below has three
independent fresh-process runs; the table gives the range of each run's
statistic rather than treating adjacent frames as independent samples.

## Hardware and measurement scope

Windows 11 10.0.22631, Intel Core i7-14700KF, 64 GiB RAM, NVIDIA RTX 4090
24 GiB with driver 616.64. Debug build with clang-cl. Actual render and display
sizes are 1920 × 1080 for native runs. The GL test host presents offscreen, so
VSync and display presentation cost do not apply. Its wall metric times
`Scene::OnUpdateEditor` and excludes capture readback. GPU query samples are
included only when marked `valid` and carrying distinct frame IDs. The shader
cache state, graphics clock, utilization, power, process elapsed time, and
concurrent build count are in each run's `host.json`. Other worktrees were
building and running GPU processes during the measurements. These are
**contended observations**, not uncontended hardware limits or player frame
times.

## Native OpenGL, warm shader cache

All three paths captured the same scene and 540 measured frames per run.
Every run supplied 540 distinct valid GPU samples. The goal is 33.333 ms,
which none of these configurations meets.

| Path | p50 ms, across runs | p95 ms | p99 ms | maximum ms | misses / 540 | tracked peak/live MiB | post-scene-release tracked MiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| Forward | 356.13–372.07 | 395.62–423.58 | 411.63–439.28 | 529.62–541.62 | 539 each | 822.5 | 505.0 |
| Forward+ | 339.73–427.45 | 363.06–471.98 | 376.75–500.31 | 406.36–658.13 | 539–540 | 822.5 | 505.0 |
| Deferred | 379.09–414.97 | 411.95–474.71 | 464.61–623.38 | 501.05–1713.33 | 539 each | 798.8 | 481.3 |

The Deferred cold shader-cache run produced 540 valid GPU frames and 540/540
deadline misses: p50 438.46 ms, p95 462.84 ms, p99 488.26 ms, maximum
555.93 ms. The cache grew from zero to 637 files. Its full process duration
was 19.4 minutes and includes shader compilation and scene setup; it is not
a sampled frame-time statistic. Warm fresh processes reloaded the scene each
time; no disk page-streaming transition is claimed. Forward and Forward+
used that populated cache. Their first runs may still compile shaders specific
to those paths; file counts in `host.json` show the actual transition.

The run-level spread is the sampling uncertainty currently available. It is
large relative to the 33.333 ms goal, and especially wide for Forward+ and
Deferred tails. A clean, controlled rerun is needed before setting a tighter
optimization regression threshold. For this machine, the empirical baseline
is the full p50/p95/p99 range above; **33.333 ms remains the project target**.
Treat every path as failing that target. No shadow, foliage, groom, or water
work was disabled to improve the number.

Tracked memory combines CPU and GPU allocations. The post-scene-release value
includes renderer and asset caches. Isolated VRAM, histories, acceleration
structures, and retained pool bytes are not available from this counter, so
the retained-memory breakdown remains open.

## Vulkan live editor

The same 1920 × 1080 scene and three camera trajectories were captured through
the real Vulkan editor, with 540 measured frames per completed run. These are
single runs under concurrent machine load; they do not establish a run-to-run
uncertainty interval. The Vulkan `completed editor frame interval` metric
includes editor scheduling and is not directly comparable with the GL test
host's `Scene::OnUpdateEditor` wall metric. All 540 GPU samples in each
completed run had distinct valid frame IDs.

| Configuration | p50 ms | p95 ms | p99 ms | maximum ms | misses / 540 | result |
|---|---:|---:|---:|---:|---:|---|
| Forward, native, raster | 397.77 | 539.19 | 669.55 | 2011.70 | 540 | `vk-forward-live-01` |
| Forward+, native, raster | 315.60 | 376.48 | 383.97 | 414.86 | 540 | `vk-forward-plus-live-01` |
| Deferred, native, raster | 731.50 | 907.23 | 1016.02 | 1111.18 | 540 | `vk-deferred-live-01` (after #1437) |
| Deferred, native, hybrid RT shadows | 709.99 | 773.02 | 840.70 | 1275.48 | 540 | `vk-hybrid-live-01` (after #1437) |

The two completed captures include Beauty from all three camera poses and
stationary HDR/depth. The Forward screenshot shows the foxes, grass and water,
but its water reads as a raised flat sheet relative to the GL capture. That
geometry difference was the editor host's live clock plus a doubled water
displacement, both fixed in #1470; see `water-parity-1470/`. The remaining
Vulkan Forward difference is shading (whitish water, #1486). The
Forward+ path switch logged 15 unpublished storage-binding errors for
`PBR_MultiLight`, `PBR_MultiLight_Skinned` and `Terrain_PBR`. The Forward run
logged `VUID-vkDestroyImage-image-01000` during a 1920 × 1080 resize. Neither
log is clean, and this report does not infer visual parity from valid timing
samples. Full logs and raw measurements are retained with each result.

Vulkan's renderer tracker reported only 6 MiB peak/live for both completed
runs and no post-scene-release value. That clearly misses large GPU allocations
visible in the GL counter; it is a telemetry scope gap, not a 6 MiB renderer
memory budget. GPU clocks/load and interfering processes are captured in each
`host.json`.

### Deferred and hybrid after #1437

The Deferred and hybrid rows were first blocked by a device fault in
`RayTracingScenePass` (`READ of invalid address 0x10000000000`). The #1338
isolation showed it needed an animated fox, but not what read the address.
The cause (#1437) was the ray-traced vegetation deformer.
`VegetationSurfaceCache::Dispatch` wrote its uniform block without binding it.
The slot it uses (`UBO_RAY_TRACING`) is shared, and whenever an animated
surface deformed, `DeformedSurfaceCache` had bound its own smaller block there
one pass earlier. The vegetation shader then read addresses from past the end
of the fox's data. RT vegetation is enabled only on Deferred, which is why
Forward and Forward+ completed. The fix binds both blocks before every
dispatch; see
`docs/agent-rules/shared-uniform-binding-bind-before-dispatch.md`.

Both rows above are single evidence runs on the fixed build (commit
`c93bab8b8`), each with 540 distinct valid GPU frames, no device fault and no
validation VUID. Two more fresh-process repeats of each preset also completed.
They were not retained as budget runs. A sibling worktree's editor was using
the GPU during these runs, so these are contended observations like the rows
above. The hybrid row is not directly comparable with the raster row: each run
saw different concurrent load (see each `host.json`).

A live Vulkan Deferred session on the same scene confirmed that the hybrid
preset traces. With ray-traced shadows on, the RT shadow pass reported 3
ray-traced lights and 0 shadow-map fallbacks. `olo_rt_scene_stats` reported
166 traced TLAS instances, 124 BLAS refits per frame, and 41 animated surfaces
deformed with none refused. A screenshot A/B of the shadow toggle at the
editor's default camera was inconclusive: the difference between shadows on
and off (4,826 changed pixels) did not exceed the difference between two
shadows-off frames (4,331 pixels) caused by animation. The pass counters, not
the image, are the evidence that shadows were traced.

The fault reproduced 5 of 5 times on the reduced `IntegratedRendererOneAnimal`
scene before the fix, including with `OLO_VK_ASYNC_COMPUTE=0`. After it, the
same scene completed 3 of 3 with Deferred applied after loading, 3 of 3 with
Deferred selected before loading, and 2 of 2 with async compute off. The
no-groom variant completed as well. Its fault in #1338 is explained by the same
cause, since the groom was never involved. The isolation scenes, manifests and
pre-fix fault logs remain in `vk-deferred-isolation/`.

## MSAA and upscale controls

Short functional GL probes used the same scene at 1920 × 1080, one stationary
camera and 100 measured frames each. Deferred with four MSAA samples completed
100/100 valid GPU samples; the requested and selected settings both report
four samples. Forward, Forward+ and Deferred with the `Quality` upscale mode
also completed 100/100 samples each, but their actual internal and display
resolutions were both 1920 × 1080. These runs prove settings admission and
capture continuity at native resolution; `result.json` does not prove the
upscale pass produced and consumed an image. Their raw files are in
`gl-controls/` and are not used as independent budget runs.

A separate Deferred probe requested `RenderScale: 0.67` with Quality upscale.
The manifest parser refused it before rendering:
`Output.RenderScale must be a finite 1.0 in schema v1 (sub-scale capture is not supported)`.
Its manifest and failure log are in `gl-nonnative-upscale/`. This is an
explicitly **unverified non-native cell** for all three GL paths; the parser
gate applies before path selection. Issue #1397 separately records cropped
non-native framing in the live editor, so a live editor A/B cannot substitute
for an aligned headless capture.

On Vulkan after #1437, the `integrated-deferred-msaa4`,
`integrated-deferred-upscale-quality` and `integrated-hybrid-upscale-quality`
manifests each completed once, with no device fault. Their `result.json`
report the requested MSAA and upscale settings as selected. Like the GL probes,
these prove the variants no longer fault; they are not budget runs. The
Deferred upscale probe hit the editor host's per-frame wait deadline and
measured 254 of 540 frames (`warmupTimedOut: true`), so it gives no usable
timing.

## Image and technique evidence

The representative stationary Beauty captures visibly contain the fox herd,
grass, water, and sky. Matching per-camera `SceneColorHDR.hdr`, depth, and
where the path supplies them, albedo, normals and velocity, are under
`docs/testing/evidence/integrated-renderer-1338/`. The dolly and rapid-turn
captures provide different camera poses, and their velocity AOVs show motion.
For a fixed-camera visual motion check after rebasing, `gl-motion-pair/` holds
stationary GL Deferred captures at frames 100 and 190, both 960 × 540 with the
same seed and camera. Their Beauty images differ at 328,394/518,400 pixels
(maximum channel difference 201/255); the fox and moving water visibly change
position, and aligned Velocity AOVs are retained. This establishes visible
scene motion, not isolated character-motion error or temporal stability.
The water patch overlaps grass on its near edge, and the source grooms are
unbound to fox bodies; these are visible quality limitations of this workload,
not a claim of production groom or water/vegetation interaction quality.

Requested and applied path, MSAA, upscale and RT-shadow settings are in each
`result.json`. They are separate from proof that a particular pass produced a
resource and a downstream pass consumed it. `passTimingsMs` is one snapshot
after all scenarios, not a per-scenario pass distribution. A stationary
960 × 540 diagnostic probe measured ShadowPass at 368.16 ms and ScenePass
at about 24 ms on its final frame; that supports investigating shadow cost,
but does not establish a run-level per-pass attribution. The raw total-GPU,
CPU, fence-wait and present-wait distributions are retained in each summary.

## Open work and ownership

- The 33.333 ms target fails on all measured native GL paths. Investigate
  shadow work and GPU occupancy on an uncontended host before making an
  optimization claim. Feed this baseline to #1259.
- Every measured Vulkan path, including Deferred and hybrid after #1437,
  misses 33.333 ms on every sampled frame.
- Investigate the Vulkan resize image-lifetime VUID and the Forward+ storage
  binding errors (#1487) before claiming backend parity. The water geometry
  difference is resolved (#1470, `water-parity-1470/`); Vulkan Forward water
  shading is #1486.
- Enable valid sub-scale benchmark readback and resolve the live-editor crop
  (#1397) before evaluating non-native upscaling quality or performance.
- Improve the integrated content fixture's grass/water boundary and bound
  groom assets before using it as a production visual quality gate (#1259,
  groom and flora owners).
- Add isolated GPU/pooled/history/AS retention telemetry before declaring a
  VRAM or post-release retention budget (#1338).
- Streaming transitions await the relevant #434 capability; this fixture
  currently checks fresh scene load and warm reload only.

## Reproduction and raw files

The exact commands and manifest format are in
`docs/guides/integrated-renderer-benchmark.md`. The evidence directory keeps
all run-level `measurement.csv`, `result.json`, manifest echoes, and host
records. Each native GL path also has `summary.json` and `summary.md`.
Deferred has aligned HDR/component AOVs for all three camera trajectories;
Forward and Forward+ preserve Beauty for each trajectory and stationary HDR.
Deferred has one cold run and three warm runs. Capture
results' `provenance.commitSha` field records the branch base because the
binary was built while this task's source diff was uncommitted. Vulkan Forward+
records the task commit instead. The published PR diff plus the build and gate
logs identify the source changes; the early provenance limitation must be kept
when comparing later runs.
