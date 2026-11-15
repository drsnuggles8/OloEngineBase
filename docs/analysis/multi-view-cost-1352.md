# What a second scene view costs

Issue #1352, acceptance item 5: report per-view and shared cost and memory, and show single-view
behaviour and frame cost unchanged. The design is
[ADR 0026](../adr/0026-a-scene-view-is-one-struct-and-a-frame-is-one-step-of-the-world.md). Every number
here is from the RTX 4090 development machine and names the test or command that reproduces it.

**The frame times below are not a baseline.** While they were taken another application held the GPU
at about 80 % (2026-11-15); the repeats show the spread. The memory figures are byte counts and do
not depend on load.

## What a view owns, and what the views of a frame share

| Per view (allocated by `Renderer3D::CreateView`, released by `DestroyView`) | Shared by every view of a frame |
|---|---|
| the render graph's targets and transient pool at the view's size | the GPU Scene records, material tables and their uploads |
| every texture history (TAA, SSR, SSGI, cloudscape, ray-traced masks and reservoirs) | the acceleration structures and the deformed-vertex streams |
| the Hi-Z occlusion pyramid | object pose history, the frame clock, the stochastic index |
| the shadow maps: cascade array, local-light atlas, virtual shadow pool | wind, snow, wake, spray and precipitation simulation |
| the light grid and its buffers | shaders the `ShaderLibrary` owns, meshes, textures, the fixed uniform blocks |
| each pass's own objects, including the programs a pass compiles for itself | `FrameResourceManager` allocators and the descriptor heap |
| DDGI's camera cascades and probe atlases | foliage placement and streamed-detail residency |

Per view frame, a view pays its own submission, culling, shadow rendering, graph execution and
post-process. Once per physical frame, whichever view is primary pays the scene extraction and
commit, the acceleration-structure build, the skeletal deform dispatch and the world simulation; a
later view links its draws to the committed records instead.

## Measured: memory

`MultiViewCost.ASecondViewCostsItsOwnStorageAndLeavesNothingBehind` reads
`RendererMemoryTracker::GetGpuResidentBytes()` (the #1342 memory report's resident total) with the
main view alone, with a second view of the same size and configuration, and after destroying it.
Deferred, 256x256, default shadow settings. Identical in three runs per backend.

| Backend | Main view alone | With a second view | Second view | After `DestroyView` |
|---|---|---|---|---|
| OpenGL | 232.165 MiB | 352.409 MiB | +120.244 MiB | 232.165 MiB (0 MiB left behind) |
| Vulkan | 273.287 MiB | 401.182 MiB | +127.895 MiB | 273.287 MiB (0 MiB left behind) |

At this size the second view's memory is almost entirely its shadow maps, which do not scale with the
view's resolution: the cascade array and the local-light atlas are sized by the shadow settings. The
test asserts that less than 1 MiB survives the view.

## Measured: frame time

Same test, Release binary, wall time of the scene tick per physical frame over 40 frames, three runs
per backend. "View frame" is `RenderViewInfo::LastFrameCpuMs`, `BeginScene` to the end of `EndScene`,
which each view reports for itself.

| Backend | One view | Two views | Second view adds | Primary view frame | Later view frame |
|---|---|---|---|---|---|
| OpenGL | 1.80, 1.82, 1.88 ms | 2.99, 3.25, 3.06 ms | 1.18 to 1.42 ms (63 to 78 % of a single-view frame) | 1.19, 1.28, 1.19 ms | 1.05, 1.17, 1.07 ms |
| Vulkan | 4.69, 4.28, 4.26 ms | 7.48, 7.93, 6.39 ms | 2.13 to 3.65 ms (50 to 85 %) | 1.68, 1.67, 1.64 ms | 1.47, 1.48, 1.54 ms |

The later view's frame is about a tenth cheaper than the primary's: it skips the scene extraction,
the commit and the world simulation and links its draws instead. In this scene (five cubes, a ground
plane, two lights) that shared work is small, so a second view costs most of a frame. The share grows
with the scene: extraction, the acceleration-structure build and skeletal deformation scale with
content, a view's own passes with its resolution.

The frame-transient descriptor ring, which the views of a bracketed Vulkan frame share
(`kDescriptorHeapTransientSlots`, 1024 per frame), was not drawn on by this scene at all: peak use 0
with one view and with two, on both backends. The test asserts that two views overflow nothing.

## Measured: creating a view

`Renderer3D::CreateView` builds a whole pass pipeline, and the passes that compile a program of their
own (`Shader::Create` in the pass, not a `ShaderLibrary` entry) compile it again for the new view.
Same test:

| Backend | Release binary | Debug binary |
|---|---|---|
| OpenGL | 521, 524, 529 ms | 3.6 to 5.0 s |
| Vulkan | 371, 377, 395 ms | 4.2 to 4.5 s |

Create a view once and keep it. The state-machine harness does exactly that for its
`isolated-vs-shared-view` pair.

## Single-view frame cost, before and after

A/B of the test binary built at `c341429e4` (before) and at this branch (after), same Debug
configuration, through the benchmark capture entry point
(`OloEngine-Tests --gtest_filter=BenchmarkCapture.* --olo-capture-manifest=<manifest>`), 1000 measured
frames per run, runs alternating A B B A so neither arm always goes first. Median over runs of each
run's p50.

| Workload | Arm | Runs | `renderCallMs` p50 | `cpuMs` p50 | `gpuMs` p50 |
|---|---|---|---|---|---|
| `drift-menu-performance` | before (`c341429e4`) | 10 | 6.05 ms (runs 5.60 to 6.79) | 4.59 ms | 0.472 ms |
| `drift-menu-performance` | after | 10 | 6.31 ms (runs 5.81 to 7.37) | 4.63 ms | 0.472 ms |

GPU time is the same in both arms. The CPU-side medians differ by less than the spread of
either arm's own runs, and the order changed between batches: the first four pairs read 6.09 ms
before and 6.56 ms after, the next six 6.05 ms before and 5.98 ms after. An A/B at the head before
the review fixes, eight runs per arm, read 5.81 ms before and 5.62 ms after. No measurement here
shows the single-view frame slower.

The identical-arms control (the `c341429e4` binary in both positions, six runs each) bounds what this
procedure can resolve: 5.548 ms against 5.556 ms `renderCallMs` p50 between the two positions, with
single runs between 5.40 and 7.25 ms. A difference inside that spread is not a measurement of the
change. The dense `integrated-deferred` workload was not run: one Debug run of it takes over four
minutes and the GPU was not free for an A/B of that length.

Single-view behaviour is pinned separately: with no second view created, `BeginScene` opens and
closes its own physical frame and is that frame's primary view, so every gated step runs exactly
where it ran before. The existing suites are the evidence (the full single-process sweep in the PR
body).
