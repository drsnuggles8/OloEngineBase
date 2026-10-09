# Foliage cost baseline (#1391)

What the flora costs on one named machine, as **measured baselines** for later flora work to
regress against. Nothing here is a budget the scene was tuned to meet, and nothing was thinned:
the subjects are the committed scenes as authored.

**Hardware.** NVIDIA GeForce RTX 4090 (24 GiB, driver 617.14), Intel Core i7-14700KF, 64 GiB,
Windows 11 10.0.22631. OpenGL 4.6 headless test host and a Vulkan editor, both Release builds
(clang-cl, `dev-cached`) from `feature/foliage-cost-baseline-1391`: OpenGL runs 1–3 and the Vulkan
editor at `13ad0f9a7`, runs 4–5 at `2fd61ce7a`. The commits between them change no draw or dispatch
(only the readback path, the timer budget and the measurement scripts).

## Results in one paragraph

With cascaded shadow maps, foliage costs **7.3–19.5 ms of GPU time per 1080p frame** on the
traversal and 6.6–17.8 ms on the two fixtures, 73–92% of the frame. On the traversal the largest
part is the **shadow casters (4.3–10.4 ms)**; on the dense fixtures it is the visible draw. With
virtual shadow maps foliage costs **26–111 ms**, 86–97% of the frame, because the VSM redraws every
layer into all 16 clip levels every frame (#1580). The GPU cull is worth **116–949 ms** a frame:
without it every plant is drawn into the main view and every shadow view. The density LOD changes
the frame by **−0.5 to +1.7 ms** in every cell on either backend. A whole-layer rebuild is a **1–2 s** frame
(#1582), and the foliage floor holds **371 MB** of GPU memory, **320 MB** of it cull streams
(#1581).

## OpenGL: GPU time per frame, by stage

Milliseconds of GPU time at 1920x1080, no MSAA. Each value is a cell's median over its
interleaved blocks; a range is over the subject's poses (nine traversal steps, three fixture
cameras). "Foliage share of the frame" is the whole-frame GPU time minus the NoFoliage arm's; the
stages beside it are foliage's own brackets, except the G-buffer share, which is
`ScenePass(Shipped) − ScenePass(NoFoliage)` because Deferred draws foliage among everything else.
Every cell, with its raw samples, is in [foliage-cost-baseline-1391/summary.md](foliage-cost-baseline-1391/summary.md)
and the per-run `foliage-cost.json.gz`.

| subject | path | shadows | poses | frame GPU p50 | foliage share of the frame | main-view cull | shadow-view culls | shadow casters | forward draw (+prepass) | G-buffer share (ScenePass Δ) | GPU culling off: frame Δ | density LOD off: frame Δ |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Traversal | Forward | CSM | 9 | 9.97–20.75 | 7.31–18.48 | 0.09–0.15 | 0.39–0.48 | 4.33–10.05 | 1.84–9.37 | -0.00–0.00 | 273.85–282.74 | -0.53–0.58 |
| Traversal | Forward | VSM | 9 | 32.52–64.69 | 28.28–61.38 | 0.14–0.14 | 2.02–2.28 | 19.82–48.65 | 2.14–14.78 | -0.01–0.01 | 839.08–869.34 | -0.01–0.61 |
| Traversal | ForwardPlus | CSM | 9 | 10.12–21.32 | 7.45–19.05 | 0.14 | 0.48–0.49 | 4.29–10.02 | 1.89–9.55 | -0.01–0.00 | 271.48–278.57 | 0.01–0.54 |
| Traversal | ForwardPlus | VSM | 9 | 30.94–65.08 | 26.73–61.76 | 0.14 | 1.99–2.28 | 19.85–49.57 | 2.28–14.78 | -0.00–0.01 | 838.41–876.41 | -0.33–1.34 |
| Traversal | Deferred | CSM | 9 | 9.98–21.13 | 8.42–19.45 | 0.14 | 0.47–0.48 | 4.28–10.40 | 0.00 | 2.67–8.32 | 354.19–364.52 | 0.08–0.36 |
| Traversal | Deferred | VSM | 9 | 29.08–66.43 | 26.28–63.55 | 0.14 | 2.01–2.09 | 19.46–51.60 | 0.00 | 2.70–7.84 | 916.89–948.74 | -0.02–0.91 |
| Meadow | Forward | CSM | 3 | 16.35–20.24 | 14.51–17.84 | 0.06 | 0.24 | 3.66–4.36 | 9.65–13.53 | -0.00 | 142.80–150.95 | -0.06–0.02 |
| Meadow | Forward | VSM | 3 | 69.87–74.87 | 65.89–70.99 | 0.06–0.06 | 1.14–1.15 | 40.50–47.71 | 17.89–23.26 | -0.00–0.00 | 486.80–498.25 | -0.16–0.20 |
| Meadow | ForwardPlus | CSM | 3 | 16.32–20.13 | 14.49–17.82 | 0.06–0.06 | 0.24 | 3.67–3.69 | 10.06–13.53 | -0.00 | 142.67–151.01 | -0.14–0.09 |
| Meadow | ForwardPlus | VSM | 3 | 69.65–74.96 | 65.71–71.08 | 0.06 | 1.14–1.16 | 40.47–47.79 | 17.88–23.10 | 0.00 | 487.67–499.38 | -0.18–0.05 |
| Meadow | Deferred | CSM | 3 | 8.23–8.97 | 6.57–6.84 | 0.06–0.06 | 0.24 | 3.68–3.70 | 0.00 | 1.78–2.10 | 174.03–174.36 | -0.18–-0.10 |
| Meadow | Deferred | VSM | 3 | 64.32–68.91 | 60.77–65.14 | 0.06 | 1.15–1.16 | 55.23–59.79 | 0.00 | 1.79–2.14 | 515.51–517.85 | -0.26–0.23 |
| Woodland | Forward | CSM | 3 | 16.58–17.68 | 14.40–15.48 | 0.06 | 0.22 | 5.98–6.75 | 7.48–8.59 | -0.00 | 116.74–120.01 | -0.00–0.14 |
| Woodland | Forward | VSM | 3 | 78.71–97.11 | 74.99–93.85 | 0.06 | 0.89–0.92 | 58.21–76.92 | 13.17–14.41 | -0.00 | 394.04–400.35 | -0.44–-0.10 |
| Woodland | ForwardPlus | CSM | 3 | 16.49–17.71 | 14.30–15.51 | 0.06 | 0.22 | 5.97–6.48 | 7.70–8.26 | -0.00–0.00 | 116.13–119.94 | -0.05–0.14 |
| Woodland | ForwardPlus | VSM | 3 | 79.15–97.24 | 75.37–93.93 | 0.06 | 0.88–0.94 | 58.19–77.33 | 12.98–14.53 | 0.00–0.01 | 395.59–400.63 | -0.40–0.28 |
| Woodland | Deferred | CSM | 3 | 11.71–12.37 | 9.63–10.35 | 0.06 | 0.21–0.22 | 6.14–6.52 | 0.00 | 2.09–2.64 | 141.24–141.80 | -0.07–0.11 |
| Woodland | Deferred | VSM | 3 | 99.63–114.55 | 96.09–111.37 | 0.06 | 0.95–1.01 | 88.18–103.49 | 0.00 | 2.11–2.69 | 417.56–418.78 | 0.24–0.57 |

What the table says:

* **On the traversal, shadows cost more than the visible foliage; on the fixtures, the other way
  round.** With CSM the casters exceed the forward draw or the G-buffer share in 16 of the
  traversal's 20 measured cells, but in only 3 of 9 on each fixture, whose dense near-field grass
  makes the forward draw 7.5–13.5 ms. The CSM region draws every cascade's survivors through the same
  authored-mesh and card draws the main view uses.
* **VSM multiplies the shadow cost by 4.4–16x**, pose for pose. The foliage footprint is submitted
  to the VSM as deforming every frame (`ShadowRenderPass::SubmitFamilyDynamicInvalidations`), so its
  pages never stay cached and all 16 clip levels cull and redraw every layer every frame (16
  `ShadowPass/FoliageCull` brackets a frame against CSM's one).
* **The culls are cheap; switching them off is not.** The main-view cull is 0.06–0.15 ms and the
  shadow-view culls 0.2–0.5 ms (CSM) or 0.9–2.3 ms (VSM). Without them every plant is drawn into
  every view: +116–365 ms with CSM, +394–949 ms with VSM.
* **Density LOD buys almost no GPU time here.** At the far pose of the lever's contract test it
  removes 36% of the density-LOD layers' survivors (126 903 against 197 411 without it), but those
  are distant cards, and the frame moves by −0.5 to +1.3 ms in every OpenGL cell.
* **Resolution barely moves it** (traversal, Deferred CSM, Step0/4/8): the foliage share is
  9.7/13.5/7.4 ms at 1280x720, 13.2/15.4/8.4 at 1920x1080 and 13.6/15.9/9.0 at 2560x1440. The
  shadow casters, a large part, do not scale with the screen at all. **MSAA 4x** (Deferred, the only
  path with MSAA) adds 0.8–1.4 ms to the frame's foliage share, 0.1–0.7 ms of it in the G-buffer.

## Vulkan, live editor

A Release editor launched with `--rhi=vulkan` (`[RHI] Backend: Vulkan (source: --rhi flag)` in the
log), with a 1920x1080 viewport override and the edit-mode throttle off, driven over MCP by
`scripts/perf/foliage-cost-live.py`: the same four arms, interleaved the same way, 8 distinct
resolved frames per block from `olo_perf_pass_timings`. The poses are a subset (traversal
Step0/4/8 and each fixture's `frontal` camera) on every path and both shadow techniques: 30 cells.
The editor runs on the wall clock, so wind moves between samples. Neither editor session's log holds
an error or a validation VUID.

On Vulkan the CSM region records its cascades in parallel items, where the timer pool cannot stamp,
so `ShadowPass/FoliageCasters` exists only with VSM (whose views record one after another); the
foliage share of the shadow pass is given as `ShadowPass(Shipped) − ShadowPass(NoFoliage)`. No frame
GPU time came back null; a handful of per-pass entries did, and the table counts them rather than
reading them as zero. Every cell and arm is in
[foliage-cost-baseline-1391/vulkan-live/summary.md](foliage-cost-baseline-1391/vulkan-live/summary.md).

| subject | pose | path | shadows | frame GPU | foliage share of the frame | main-view cull | shadow-view culls | ShadowPass Δ (casters + culls) | forward draw (+prepass) | G-buffer share (ScenePass Δ) | GPU culling off: frame Δ | density LOD off: frame Δ | null GPU samples | contended |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Traversal | Step0 | forward | CSM | 10.65 | 9.04 | 0.13 | 1.01 | 7.27 | 1.39 | -0.01 | 266.55 | 0.28 | 0 |  |
| Traversal | Step4 | forward | CSM | 13.23 | 11.07 | 0.13 | 1.08 | 9.10 | 1.78 | -0.02 | 264.55 | 0.26 | 0 |  |
| Traversal | Step8 | forward | CSM | 9.25 | 7.34 | 0.13 | 1.09 | 5.92 | 1.06 | 0.00 | 268.46 | -0.17 | 1 |  |
| Traversal | Step0 | forward | VSM | 37.39 | 35.22 | 0.13 | 2.32 | 32.97 | 1.55 | -0.01 | 811.86 | 0.82 | 0 |  |
| Traversal | Step4 | forward | VSM | 42.72 | 39.80 | 0.13 | 2.43 | 37.25 | 1.91 | -0.02 | 869.75 | 1.15 | 0 |  |
| Traversal | Step8 | forward | VSM | 23.04 | 20.54 | 0.12 | 2.62 | 19.03 | 1.16 | 0.00 | 914.78 | 1.21 | 0 |  |
| Traversal | Step0 | forwardplus | CSM | 11.06 | 8.82 | 0.13 | 1.01 | 7.78 | 1.40 | -0.01 | 267.60 | -0.10 | 1 |  |
| Traversal | Step4 | forwardplus | CSM | 13.29 | 11.35 | 0.14 | 1.09 | 9.29 | 1.80 | -0.02 | 264.94 | 0.26 | 0 |  |
| Traversal | Step8 | forwardplus | CSM | 9.18 | 6.82 | 0.13 | 1.09 | 5.69 | 1.06 | 0.00 | 269.18 | 0.17 | 2 |  |
| Traversal | Step0 | forwardplus | VSM | 37.51 | 34.28 | 0.14 | 2.33 | 33.30 | 1.56 | -0.01 | 855.05 | 0.40 | 1 |  |
| Traversal | Step4 | forwardplus | VSM | 42.61 | 39.07 | 0.14 | 2.76 | 36.88 | 1.92 | -0.01 | 860.35 | 0.77 | 3 |  |
| Traversal | Step8 | forwardplus | VSM | 23.04 | 20.01 | 0.13 | 2.73 | 19.10 | 1.15 | 0.00 | 866.30 | 0.59 | 2 |  |
| Traversal | Step0 | deferred | CSM | 10.83 | 7.67 | 0.13 | 1.05 | 7.58 | 0.00 | 1.16 | 266.08 | 0.38 | 0 |  |
| Traversal | Step4 | deferred | CSM | 13.69 | 9.80 | 0.14 | 1.13 | 9.16 | 0.00 | 1.66 | 264.60 | 0.05 | 0 |  |
| Traversal | Step8 | deferred | CSM | 9.26 | 5.44 | 0.13 | 1.11 | 5.64 | 0.00 | 0.86 | 267.25 | 0.40 | 0 |  |
| Traversal | Step0 | deferred | VSM | 37.45 | 34.25 | 0.13 | 2.34 | 33.33 | 0.00 | 1.20 | 844.70 | 0.75 | 0 |  |
| Traversal | Step4 | deferred | VSM | 42.41 | 38.97 | 0.14 | 2.73 | 37.54 | 0.00 | 1.79 | 838.90 | 1.21 | 0 |  |
| Traversal | Step8 | deferred | VSM | 23.09 | 19.62 | 0.13 | 2.72 | 19.09 | 0.00 | 0.88 | 850.48 | 0.43 | 0 |  |
| Meadow | frontal | forward | CSM | 21.16 | 17.39 | 0.09 | 0.58 | 15.99 | 1.90 | -0.03 | 171.88 | 0.20 | 1 |  |
| Meadow | frontal | forward | VSM | 55.07 | 51.64 | 0.09 | 1.65 | 48.85 | 1.97 | 0.00 | 530.08 | -0.18 | 0 |  |
| Meadow | frontal | forwardplus | CSM | 21.23 | 17.38 | 0.09 | 0.57 | 15.99 | 1.88 | -0.01 | 172.28 | 0.02 | 0 |  |
| Meadow | frontal | forwardplus | VSM | 54.39 | 49.93 | 0.09 | 1.65 | 48.39 | 1.97 | -0.01 | 527.41 | 1.17 | 0 |  |
| Meadow | frontal | deferred | CSM | 21.10 | 16.44 | 0.09 | 0.58 | 16.07 | 0.00 | 1.86 | 171.58 | 0.26 | 0 |  |
| Meadow | frontal | deferred | VSM | 53.31 | 49.77 | 0.09 | 1.62 | 48.15 | 0.00 | 1.78 | 504.24 | 0.20 | 0 |  |
| Woodland | frontal | forward | CSM | 21.94 | 19.37 | 0.09 | 0.57 | 16.64 | 2.26 | -0.01 | 136.05 | -0.05 | 0 |  |
| Woodland | frontal | forward | VSM | 76.49 | 73.65 | 0.08 | 1.36 | 70.73 | 2.31 | -0.02 | 369.26 | 1.69 | 0 |  |
| Woodland | frontal | forwardplus | CSM | 21.72 | 19.17 | 0.08 | 0.52 | 16.97 | 2.25 | -0.02 | 129.91 | 0.71 | 0 |  |
| Woodland | frontal | forwardplus | VSM | 75.34 | 72.48 | 0.08 | 1.36 | 69.63 | 2.32 | -0.02 | 368.56 | 1.07 | 0 |  |
| Woodland | frontal | deferred | CSM | 21.57 | 19.36 | 0.08 | 0.52 | 16.81 | 0.00 | 1.96 | 130.15 | -0.30 | 0 |  |
| Woodland | frontal | deferred | VSM | 75.27 | 72.47 | 0.08 | 1.35 | 69.65 | 0.00 | 1.98 | 370.90 | 0.68 | 0 |  |

Tails, from `olo_perf_frame_history` (1024 frames of one pose, CSM):

| pose | path | frames | frame time p50 | p95 | p99 | max | misses (16.67 ms) | GPU p50 | GPU p99 |
|---|---|---|---|---|---|---|---|---|---|
| Step0 | forward | 1024 | 10.75 | 20.45 | 71.98 | 176.70 | 73 | 10.64 | 62.05 |
| Step4 | forward | 1024 | 13.05 | 13.88 | 14.50 | 20.77 | 4 | 12.96 | 13.93 |
| Step8 | forward | 1024 | 9.04 | 10.61 | 13.63 | 92.76 | 3 | 8.92 | 10.14 |
| Step0 | forwardplus | 1024 | 10.61 | 11.61 | 12.35 | 36.45 | 3 | 10.51 | 11.97 |
| Step4 | forwardplus | 1024 | 13.08 | 14.37 | 18.17 | 35.38 | 15 | 13.03 | 14.25 |
| Step8 | forwardplus | 1024 | 9.48 | 16.12 | 25.93 | 42.11 | 46 | 8.96 | 19.43 |
| Step0 | deferred | 1024 | 10.82 | 16.50 | 22.66 | 35.40 | 51 | 10.76 | 15.08 |
| Step4 | deferred | 1024 | 13.33 | 19.80 | 27.12 | 38.87 | 110 | 13.28 | 17.14 |
| Step8 | deferred | 1024 | 9.10 | 13.83 | 21.31 | 37.00 | 28 | 8.98 | 15.65 |

**How Vulkan compares with OpenGL.** On the traversal at the same three poses, foliage costs
5.4–11.4 ms with CSM against GL's 8.4–15.4, and 19.6–39.8 ms with VSM against 26.3–53.6. Most of the
gap is the forward draw: 1.1–1.9 ms on Vulkan against 4.4–5.6 ms on GL at those poses. The GPU cull
is worth the same order of magnitude on both (+130–915 ms when it is off), and density LOD the same
nothing (−0.3 to +1.7 ms). One difference is open: **on the two fixtures, Vulkan's CSM foliage
shadow share is 2.5–4x GL's** (Meadow 16.0 against 3.9 ms, Woodland 16.6–17.0 against 6.4–6.7 ms),
while the traversal agrees (5.6–9.3 against GL's casters at the same poses). The live editor camera
has a 1000 m far plane where the manifests' cameras have 2000 m, which moves the cascade splits, so
this is not attributed to the backend; it is the first thing to check before treating the Vulkan
fixture CSM rows as a backend baseline.

## Frame-time tails and the rebuild spike

600-frame windows (OpenGL, traversal Step0/4/8, every path and shadow technique), steady and with an
in-place whole-layer rebuild every 100 frames. In steady state the GPU p99 is within 4 ms of the
p50 on every window but one (Step8 Forward CSM, 12.9 ms): the foliage frame is flat. The rebuild is
not (#1582):

* an in-place rebuild (`m_NeedsRebuild`, what editing any layer property does) is a **1–2 s** frame:
  the median of each window's five rebuild frames is 1.16–1.90 s, the fastest single rebuild 0.93 s,
  the slowest 7.18 s, with up to ~0.5 s of it on the GPU (a rebuild window's GPU p99 reaches 517 ms);
* a cold rebuild (the component re-enabled after being disabled) is 70–1066 ms, median 471 ms over
  288 rebuilds in run 3 (333–601 ms, median 381 ms, over run 5's 27).

With CSM, the steady frames at the Step4 pose already miss a 16.67 ms deadline on every path (143,
146 and 373 of 600 frames on Forward, Forward+ and Deferred); with VSM every frame misses it.

## Memory

From the #1342 report. OpenGL sizes are format estimates; the Vulkan report's committed (VMA) sizes
agree: the `Foliage pinned representations` owner holds 404.4 MB on GL and 406.3 MB committed on
Vulkan for the traversal scene, and the category rows are identical.

| scene | plants | instance buffers | cull group tables | cull streams (5 view slots) | card textures | impostor atlases | **pinned total** | authored mesh copies | mesh part textures | CPU canonical arrays |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| FoliageMeadowToWoodland | 491 773 | 23.6 MB | 2.0 MB | **319.7 MB** | 17.1 MB | 8.4 MB | **370.8 MB** | 1.9 MB | 35.6 MB | 63.0 MB |
| Benchmark/Meadow | 420 674 | 20.2 MB | 1.7 MB | **273.5 MB** | 4.2 MB | — | **299.5 MB** | 0.3 MB | 16.8 MB | 62.3 MB |
| Benchmark/Woodland | 141 348 | 6.8 MB | 0.6 MB | **91.9 MB** | 5.6 MB | 16.8 MB | **121.6 MB** | 2.1 MB | 25.5 MB | 23.6 MB |

The cull streams are 76–91% of foliage's GPU floor: each of the five view slots (the main view and
four shadow views) holds a compacted stream sized to every plant of every layer, twice over for a
layer with an authored mesh (#1581). An impostor atlas at the default `ImpostorAtlasResolution` of
1024 is 8 MB (albedo and normal-depth, RGBA8, no mips); the bake's depth attachment is not kept. The
`MeshSource` the authored meshes are loaded from belongs to the asset owner, not to foliage. The
Vulkan memory snapshot of `Benchmark/Woodland` was taken 4 s after opening the scene, before every
shadow slot existed; the GL rows are the reference for the slot count.

## What is not here, and why

* **Vulkan at every pose.** The live cells take three traversal poses and one camera per fixture on
  every path and both shadow techniques; the per-pose shape is OpenGL's.
* **Vulkan conditional cells (resolution, MSAA).** OpenGL only. The scaling result is a property of
  a workload whose largest part, the shadow casters, does not depend on the screen size.
* **MSAA on Forward.** There is none: `Deferred.MSAASampleCount` is the engine's only MSAA setting,
  so the MSAA cell is Deferred, where foliage writes the G-buffer.
* **A per-layer split.** The brackets are per pass, not per layer, and the NoFoliage control removes
  every layer at once. The memory rows are per category over all layers (per layer in the raw JSON).
* **CPU cost as a baseline.** Every cell records the headless wall time of one guarded editor tick
  (CPU submission plus driver back-pressure, no present), but sibling worktrees were compiling
  throughout, so only the GPU columns are baselines.

## Provenance and run validity

Five fresh-process OpenGL runs exist; three count. The box was shared with other worktrees' builds,
editors and GPU test sweeps, so a run is judged cell by cell: while it measures, the runner samples
every 30 s the GPU and every other engine or game process with its CPU time, and a cell that
overlaps a window in which such a process ran is left out (`summary.json` names every exclusion and
the process behind it).

| run | sampled | what it contributes |
|---|---|---|
| run-1, run-2 | no | raw data only. Another worktree's GPU test run overlapped at least half of them. On the 44 cells where run 1 overlaps a sampled run it agrees within 25% on 37; the 7 that disagree are all higher in run 1, which is what unrecorded contention looks like. |
| run-3 | yes (by a sampler run beside it, `run3-sampler.csv`) | every cell and tail window except the 7 its first 3 minutes shared with a foreign test binary |
| run-4 | yes | nothing: it re-measured those 7 cells while this branch's own Vulkan editor was rendering, and the sampler excluded all of them |
| run-5 | yes, no contention | the 7 traversal Forward CSM cells |

Every cell has at least one clean run, and the five runs agree where they overlap cleanly (traversal
Forward CSM Step7/8: 9.84/11.64 ms in run 3 against 9.97/11.86 ms in run 5). The Vulkan cells record
the processes beside them: five first ran beside another worktree's editor and tests and were
re-measured; a second attempt at those five read a stalled editor (three frames per arm, identical
across arms), which the driver now refuses, and they were measured a third time. Both excluded sets
are kept beside the data.

## What was measured, and how

**Subjects.** `Scenes/FoliageMeadowToWoodland.olo` (the epic #1224 traversal scene: six layers,
authored pine and shrub meshes, pine impostors, leaf transmission, hierarchical wind, GPU culling),
walked along the nine-pose meadow-to-woodland path that `FloraTraversalEvidenceTest` derives
(densest 16 m bucket of *Meadow Grass* to densest bucket of *Woodland Pines*, eye 2.6 m over the
plants). Plus the #1239 fixtures `Scenes/Benchmark/Meadow.olo` and `Scenes/Benchmark/Woodland.olo`
from their manifests' still cameras (`frontal`, `grazing`, `backlit`; the `moving` camera is a
temporal-quality probe, not a cost pose).

**Arms, interleaved.** At every pose four arms run in blocks, the order rotated by one arm per round
(three rounds), so slow clock drift on a shared box is split across arms instead of being charged to
whichever ran second:

| arm | what changes |
|---|---|
| Shipped | the scene as authored |
| CpuCull | `FoliageRenderer::SetGPUCullingEnabled(false)` (`OLO_FOLIAGE_CPU_CULL`): every generated instance submitted |
| NoDensityLod | `OLO_FOLIAGE_NO_DENSITY_LOD` (new): no distance thinning, no coverage scale-up, in the draw **and** the cull |
| NoFoliage | the `FoliageComponent` disabled: the control that turns a pass total into foliage's share of it |

Each block is 8 warm-up frames, then 12 measured frames; every arm returns to its steady cost within
one frame of a switch (measured), and the regeneration that re-enabling foliage triggers is timed on
its own, never inside a block. The GPU timings of exactly the measured frames are kept, matched by
frame number and drained under the same arm. The scene clock is pinned (`Time::SetMockTime(4)`), so
wind is the same bend in every arm; it still costs what it costs. No cull readback happens inside a
measurement: one used to leave the main view 6–20x slower for the rest of the process (see
*Found while measuring*).

**Instruments.**

* `GPUPassTimerPool` brackets at every GPU stage foliage runs: `FoliageCull` (the main view's cull,
  dispatched at scene submission, outside every render-graph pass, so before #1391 its time was in
  the frame total and in no pass), `ShadowPass/FoliageCull` (the shadow views' culls: one bracket for
  the CSM region, one per VSM view), `ShadowPass/FoliageCasters` (the shadow draws, from #1533),
  `FoliagePass` and `FoliagePrepassPass` (the forward draws). On Deferred foliage draws into the
  G-buffer inside `ScenePass` among everything else, so its share there is
  `ScenePass(Shipped) − ScenePass(NoFoliage)`. A bracket that resolved without a valid timestamp is
  counted, never read as zero.
* `FrameTimeTail` (nearest-rank p50/p95/p99, deadline misses at 16.67 ms) over 600-frame windows,
  with and without an in-place whole-layer rebuild every 100 frames.
* The #1342 renderer memory report. `FoliageRenderer`'s capacity rows now split the pinned floor by
  category (instance buffers, cull group tables, cull streams for the main view and for the shadow
  views, card geometry, card textures, impostor atlases) and the optional near field (authored mesh
  copies, part textures). The pinned categories are `PinnedGpuBytes` split, so they cannot drift
  from it. The cull buffers are now booked to the `Foliage pinned representations` owner; before,
  the main view's were booked to no owner and the shadow views' to `ShadowPass`.

## Found while measuring

* The cull readback (`FoliageRenderer::ReadbackCull`) read the GPU's live cull buffers back to the
  CPU, and on NVIDIA GL that left the main view's cull 0.09 → 1.9 ms and its forward draw 3 → 21 ms
  for the rest of the process. The first attempt at this baseline measured exactly that for every
  cell after its first survivor census. It now stages through `StagedBufferReadback`; see
  `docs/agent-rules/gpu-readback-stats-channel.md`.
* The cull buffers were booked to no memory owner (main view) or to `ShadowPass` (shadow views):
  288 MB of the traversal's 371 MB floor.
* `FloraLooseCookedParityEvidenceTest` restored the working directory from before the GPU fixture
  moved into `OloEditor/`, so every GPU suite after it in a process could not open its shaders.

## Reproduce

```powershell
cmake --preset dev-cached
pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command `
  'cmake --build build-cached --target OloEngine-Tests OloEditor --config Release --parallel 6'
# OpenGL: fresh-process runs, sampled, then the summary (summary.json / summary.md)
python scripts/perf/foliage-cost-baseline.py --exe build-cached/OloEngine/tests/Release/OloEngine-Tests.exe `
  --runs 1 --output <dir> --env OLO_FOLIAGE_COST_ROUNDS=3 --env OLO_FOLIAGE_COST_WARMUP=8 `
  --env OLO_FOLIAGE_COST_FRAMES=12
# re-measure only some cells: OLO_FOLIAGE_COST_{SUBJECTS,PATHS,SHADOWS,POSES}, OLO_FOLIAGE_COST_EXTRAS=0
python scripts/perf/foliage-cost-baseline.py --summarise-only --output docs/analysis/foliage-cost-baseline-1391
# Vulkan: a Release editor with --rhi=vulkan, MCP writes on, EditorPreferences ThrottleEditMode false
python scripts/perf/foliage-cost-live.py --port <port> --editor-pid <pid> `
  --poses <dir>/run-<k>/foliage-cost.json --output vulkan-live.json --pose-filter Step0,Step4,Step8,frontal
python scripts/perf/foliage-cost-live.py --summarise vulkan-summary.md --output vulkan-live.json
```

The committed data under `foliage-cost-baseline-1391/` reproduces the same `summary.json` (compared as parsed JSON) with
`--summarise-only` (the raw runs are gzipped, with each run's cell times extracted from its engine
log). `FoliageCostBaselineTest`'s four contract tests run in the normal suite and pin the
instruments; `MeasureBaseline` runs only with `OLO_FOLIAGE_COST=1`.
