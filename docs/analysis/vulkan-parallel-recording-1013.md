# Vulkan parallel recording: #1013 measurements and evidence

Measure command-recording work in Release and keep elapsed region time separate
from summed worker CPU time. The dense benchmark improves recording time, while
the small scene pays extra scheduling cost. These measurements do not establish
an isolated end-to-end FPS speedup.

## Method and machine

Measured on 2026-09-05 with clang-cl Release and an NVIDIA GeForce RTX 4090
(reported Vulkan API 1.4.351). The final engine build includes prepared-pass
scheduling before barrier and transient-lifetime planning. Both modes use the
same executable and live `OLO_VK_PARALLEL_RECORDING` console variable.

The dense scene is `Scenes/Benchmark/ParallelRecording.olo`: 3,600 independent
mesh/index windows, a sun, two shadow spots and two shadow points. Camera is
`[0,9,14]`, yaw 0, pitch 35, FOV 60. The normal-scene control is the unchanged
`Scenes/Benchmark/MaterialLab.olo`, with its authored camera `[0,9,16]`, yaw 0,
pitch 31.5127, FOV 45. Both use deferred rendering, GTAO and VSM, with upscaling,
TAA and DDGI cascades off. Actual viewport: 1411 x 942.

Each block warms 60 rendered frames, then samples 40 distinct frames through
`olo_perf_snapshot` and `olo_perf_pass_timings`. Blocks are interleaved
off/on/on/off/off/on. The editor was visible, not iconified, and ticking for every
sample. Observed FPS uses advancing frame indices divided by wall-clock time;
it is not the reciprocal of a sparsely sampled median frame duration.

Another worktree was compiling and running its own editor on this shared host.
This limits total-frame comparisons. GPU timestamp samples were sometimes stale
or zero; no GPU speedup is claimed. Vulkan presentation uses the existing FIFO
path, so a 60 FPS control can hide a change in CPU headroom.

## Interleaved results

Recording columns are block medians in milliseconds. "Worker" sums recording
durations across items; it is not elapsed time. "Join" excludes GPU waits.

| Scene | Block | Lever | Observed FPS | Recording wall |
|---|---:|---|---:|---:|
| Dense | 1 | off | 29.78 | 4.399 |
| Dense | 2 | on | 42.51 | 3.286 |
| Dense | 3 | on | 35.99 | 3.240 |
| Dense | 4 | off | 35.99 | 4.599 |
| Dense | 5 | off | 39.13 | 4.392 |
| Dense | 6 | on | 37.79 | 3.256 |
| Material Lab | 1 | off | 59.90 | 0.674 |
| Material Lab | 2 | on | 59.99 | 1.063 |
| Material Lab | 3 | on | 60.02 | 0.962 |
| Material Lab | 4 | off | 59.82 | 0.653 |
| Material Lab | 5 | off | 60.09 | 0.692 |
| Material Lab | 6 | on | 59.92 | 0.971 |

The median of dense block medians falls from 4.399 to 3.256 ms, about 26%.
Material Lab adds about 0.30 ms of recording wall time, with unchanged observed
vsynced throughput. The 32-command minimum grain avoids tiny packet lists but
does not guarantee a gain for every eligible list. Do not market this as a
universal performance improvement.

| Dense recorded family | Off block-median range | On block-median range |
|---|---:|---:|
| Scene depth/color wall | 3.596-3.820 | 2.391-2.467 |
| Scene depth/color worker sum | 3.593-3.815 | 25.340-27.786 |
| Scene depth/color join | 0 | 0.520-0.575 |
| Shadow wall | 0.752-0.770 | 0.696-0.716 |
| Shadow worker sum | 0.748-0.766 | 1.386-1.572 |
| Shadow join | 0 | 0.116-0.125 |
| GTAO/VSM whole-pass wall | inline executor | 0.080-0.086 |

The dense on blocks execute five regions and 66 secondaries with zero merge
conflicts. The main improvement is scene replay; the shadow recording change is
small. Worker CPU rises under concurrent recording, so worker/wall ratios alone
would overstate the benefit.

Untouched pass controls: dense Bloom CPU block medians are 0.039-0.045 ms off
and 0.040-0.042 ms on; ToneMap remains 0.006-0.008 ms. Deferred GPU occlusion
records about 0.001 ms in this fixture and stays inline for its small list.
Seven shader-debug channel draws are likewise below the replay grain and retain
their primary readback boundary. There is no evidence that manufacturing more
small regions would help.

Additional Release scene checks use off/on/on/off, 30 warm frames and eight
samples per block at each authored camera (Water uses `[0,8,24]`, pitch 24).
VSM is off; DDGI cascades are enabled only for their dedicated scene. The table
shows total recorded-region wall medians, including any inline regions. These
short checks establish cost direction on the fixtures, not a general speedup.

| Scene | Off medians (ms) | On medians (ms) | On secondaries |
|---|---|---|---:|
| Terrain VT sample, fallback shading | 1.033, 1.400 | 0.711, 0.685 | 4 |
| Foliage | 0.428, 0.463 | 0.800, 0.524 | 4 |
| Decal mode matrix, small inline list | 0.102, 0.113 | 0.098, 0.100 | 0 |
| Water/reflection | 0.242, 0.225 | 0.415, 0.558 | 4 |
| DDGI volume | 0.286, 0.410 | 0.584, 0.503 | 10 |
| DDGI cascades | 0.837, 1.022 | 1.120, 1.016 | 26 |
| Volumetric fog scene | 0.178, 0.202 | 0.424, 0.574 | 16 |

All seven off/on screenshots match exactly and were inspected; merge conflicts
and shader errors are zero. These scenes mostly pay fork overhead, confirming
that small or batched workloads do not benefit simply from enabling workers.
The fog scene's table includes its shadow regions; the separate prepared-group
fixture enables VSM and proves actual fog/GTAO/VSM concurrency.

## Fork-cost probe

A separate Release process used `OLO_VK_RECORDING_COSTS=1`, four off/on/off/on
blocks, 60 warm frames and 20 samples per block. Its actual viewport was
1920 x 1080. Clock reads per draw perturb the workload, so these numbers are
not substituted into the ordinary A/B results above.

| Cost, summed over frame regions | On block medians (ms) |
|---|---|
| Copy framebuffer selections into items | 0.037, 0.036 |
| Prepare bound target attachments | 0.020, 0.020 |
| Prepare seeded sampled images | 0.040, 0.043 |
| Graphics pipeline lookup, summed worker CPU | 0.629, 0.620 |

Selection copies and attachment preparation are retained: each is a small
fraction of recording wall time and maintains independent state plus legal
identity layouts. Pipeline lookup already constructs its key outside the shared
builder lock. Its measured summed cost does not dominate recording; a last-key
memo would also need correct shader-reload and cache-invalidation handling. This
change retains the existing lookup and exposes the probe for a focused future
comparison rather than adding an unmeasured cache. Ordinary runs leave the
environment variable unset and report zero for these optional fields.

## Correctness evidence

- CPU/device regression selection: 235 cases covered. The initial run passed
  234; the remaining new test expected the wrong number of disabled-node batches.
  Correcting that expectation and rerunning all nine graph-recording tests passed.
  No engine behavior was changed to satisfy the expectation.
- Real threaded frontend isolation, fabricated-handle layout claims and ordered
  overlay merging run without a Vulkan device and are included in Linux TSan CI.
- Real-device tests compare private bucket targets, graph shared-UBO/MRT output,
  96 mesh particles in three distinct color ranges, and 96 fluid submissions'
  nonempty depth/thickness. The mesh and fluid comparisons use respectively
  three and six secondaries, exact output comparisons and zero merge conflicts.
- Nine refreshed OpenGL visual tests passed for SSAO, EASU, fog, fluid and the
  dense scene. A separate real-scene mesh-particle test passed from three angles;
  all three color ranges remain visible. PNGs were opened and inspected.
- Asset-content validation passed 25 cases with one environment-dependent shader
  cache check skipped. The benchmark model is registered; unrelated editor-generated
  registry entries and existing regenerated image noise are excluded from the change.
- Live prepared groups report actual item names: GTAO/VSM; fog/GTAO/VSM;
  EASU/depth-velocity upscale; and SSAO/depth-velocity upscale. Off/on beauty
  comparisons are exact, with zero conflicts and shader errors.
- The final golden manifest captured beauty, scene depth and CSM cascade zero
  at near/overview/side: all nine Vulkan off/on pairs match exactly, with no
  capture failures or warmup timeouts. Beauty/depth are 1920 x 1080; CSM is
  4096 x 4096. OpenGL captured the same nine attachments successfully. All were
  visually inspected. The near camera has useful cascade-zero caster coverage.
- Three additional dense-scene beauty pairs with VSM and the actual whole-pass
  group enabled are also pixel-identical. Capture hooks intentionally select
  sequential graph execution, so the extra screenshots prove the worker path.

Permanent scene evidence is under `OloEditor/assets/tests/visual/` with prefixes
`ParallelRecording_` and `ParallelMeshParticles_`. Reproduce the attachment set
with `assets/benchmark/manifests/parallel-recording.golden.yaml` through
`olo_benchmark_capture`, toggling the recording lever between runs. Live-editor
capture uses a best-effort clock; exact results here do not promise determinism
for animated scenes.

Known Vulkan helmet/raw-geometry and compressed-VT limitations, plus the empty
unchanged billboard sample, are documented in the [pass audit](../agent-rules/vulkan-parallel-pass-audit.md).
Empty captures are not successful evidence. Five incidental fixes were kept in
separate commits: mesh-cache bounds, deferred shader routing, submission worker
capacity, linear color-array storage usage, and mesh-particle vertex pulling.
