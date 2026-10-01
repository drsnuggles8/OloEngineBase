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
*(2026-09-29 annotation, #1357; the measurements above are unchanged: the
whitish-water shading was fixed by PR #1517 (#1486) and the Forward+
unpublished-binding errors by PR #1517 (#1487).)* *(2026-10-01, #1526: the
resize VUID is gone. A Debug Vulkan editor with the validation layer and
synchronization validation on reran `integrated-forward.yaml` and
`integrated-deferred.yaml`, each capture resizing the 941-pixel-wide editor
viewport to 1920 × 1080 and back. Both logs hold no VUID and no error line;
Forward measured p50 374.8 ms, Deferred 424.4 ms, every frame over the
deadline, under concurrent sibling-worktree GPU load. PRs #1475 and #1515
(#1511) fixed the resize-path image lifetimes this run had recorded.)*

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
also completed 100/100 samples each, but their recorded internal and display
resolutions were both 1920 × 1080. *(2026-10-01, #1526: that record was wrong,
not the render. It read only the dynamic render scale; the same results' own
`SceneColorHDR` is 1280 × 720, the Quality band. See the sub-scale cells
below.)* Their raw files are in `gl-controls/` and are not used as independent
budget runs.

A separate Deferred probe requested `RenderScale: 0.67` with Quality upscale.
The manifest parser refused it before rendering:
`Output.RenderScale must be a finite 1.0 in schema v1 (sub-scale capture is not supported)`.
Its manifest and failure log are in `gl-nonnative-upscale/`. This is an
explicitly **unverified non-native cell** for all three GL paths; the parser
gate applies before path selection. Issue #1397 separately records cropped
non-native framing in the live editor, so a live editor A/B cannot substitute
for an aligned headless capture. *(2026-09-29 annotation, #1357: the crop was
fixed by PR #1443 (#1397).)* That probe also stacked a dynamic scale on an
upscaler preset, which #1526 now refuses: the preset sets the internal scale.

### Sub-scale cells (#1526, 2026-10-01)

#1526 made sub-scale capture valid (see
`docs/guides/benchmark-subscale-and-lighting-taps.md`). Each row is ONE
fresh-process run of a generated variation, 540 measured frames over the three
cameras, under concurrent sibling-worktree load: these are observations beside
the native rows, not budget runs. GL is the test host's wall metric (the native
GL rows' metric); Vulkan is the editor's frame interval (the native Vulkan
rows'). Every row's internal size is 1280 × 720 and display 1920 × 1080.

| Backend, path | technique (resolved) | p50 ms | p95 ms | p99 ms | max ms | misses |
|---|---|---:|---:|---:|---:|---:|
| GL Forward | FSR1 Quality (spatial) | 353.2 | 361.0 | 366.1 | 410.4 | 539/540 |
| GL Forward | FSR2 Quality (temporal) | 382.7 | 393.2 | 397.3 | 433.4 | 539/540 |
| GL Forward | dynamic scale 0.667 (native) | 368.6 | 381.6 | 392.7 | 516.6 | 539/540 |
| GL Forward+ | FSR1 Quality (spatial) | 362.5 | 378.6 | 384.3 | 475.3 | 539/540 |
| GL Forward+ | FSR2 Quality (temporal) | 413.7 | 430.0 | 437.4 | 480.0 | 540/540 |
| GL Forward+ | dynamic scale 0.667 (native) | 415.4 | 430.9 | 436.1 | 464.8 | 540/540 |
| GL Deferred | FSR1 Quality (spatial) | 409.9 | 422.9 | 1020.0 | 1022.6 | 540/540 |
| GL Deferred | FSR2 Quality (temporal) | 406.0 | 422.0 | 429.5 | 441.6 | 539/540 |
| Vulkan Forward | FSR1 Quality (spatial) | 351.3 | 356.6 | 358.5 | 360.4 | all |
| Vulkan Forward | FSR2 requested (spatial, `backendNotOpenGL`) | 386.9 | 463.0 | 668.2 | 723.5 | all |
| Vulkan Forward | dynamic scale 0.667 (native) | 463.8 | 606.1 | 657.7 | 1301.0 | all |
| Vulkan Deferred | FSR1 Quality (spatial) | 438.7 | 1721.0 | 2292.4 | 2360.1 | all |
| Vulkan Deferred | FSR2 requested (spatial, `backendNotOpenGL`) | 411.0 | 485.2 | 650.1 | 1482.5 | all |

**Rendering at two thirds of the resolution buys nothing on this workload.**
GL Forward at FSR1 Quality measures p50 353 ms against the native rows'
356–372 ms, and no other path improves either; every row still misses
33.333 ms on every frame. The frame is dominated by work that does not scale
with the scene band; the 960 × 540 probe below attributed 368 ms to ShadowPass.
Upscaling is not a lever for this budget until that work is.

A dynamic scale on Deferred is refused (#1537: the Deferred chain does not
honour it yet), so that cell has no row. The Vulkan Deferred upscale run that
timed out after 254 frames in #1338 now completes all of them. The raw
directories are under `OloEditor/assets/benchmark/captures/gl-subscale-1526/`
and `vk-integrated-*-1526/` on the measuring machine (git-ignored).

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
`docs/testing/evidence/integrated-renderer-1338/`; the per-path run summaries are
[GL Forward](../testing/evidence/integrated-renderer-1338/gl-forward-warm/summary.md),
[GL Forward+](../testing/evidence/integrated-renderer-1338/gl-forward-plus-warm/summary.md) and
[GL Deferred](../testing/evidence/integrated-renderer-1338/gl-deferred-warm/summary.md), and the
water parity note is [water-parity-1470](../testing/evidence/integrated-renderer-1338/water-parity-1470/README.md).
The dolly and rapid-turn
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
- ~~Rerun the Vulkan benchmark and confirm the resize image-lifetime VUID is gone~~
  Done (#1526, 2026-10-01): clean logs on Forward and Deferred. *(Updated 2026-09-29, #1357: the
  Forward+ storage-binding errors (#1487), the Vulkan Forward water shading
  (#1486) and the water geometry difference (#1470, `water-parity-1470/`) are
  fixed.)*
- ~~Enable valid sub-scale benchmark readback~~ Done (#1526): the sub-scale
  cells above. Upscaling does not reduce this workload's frame time; the
  dynamic scale on Deferred is #1537.
- Improve the integrated content fixture's grass/water boundary and bound
  groom assets before using it as a production visual quality gate (#1259,
  groom and flora owners).
- Add isolated GPU/pooled/history/AS retention telemetry before declaring a
  VRAM or post-release retention budget (#1338; *2026-09-29: #1338 is closed,
  and this is owned by #1342*).
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
