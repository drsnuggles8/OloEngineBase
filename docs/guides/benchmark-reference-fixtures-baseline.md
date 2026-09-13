# Reference fixture measured baseline (issue #1239)

The measurement log for the five reference fixtures. The contract they are captured under is
[benchmark-reference-fixtures.md](benchmark-reference-fixtures.md).

**Append, do not overwrite.** Each re-measure is a new dated block naming its machine and build
configuration, so a number can always be traced to the run that produced it.

## 2026-09-13 — RTX 4090, Release, OpenGL

Everything below is a **measurement taken on one named machine**, not a forecast. Nothing here is
a performance promise, and none of it licenses reducing the AAA ambition — issue #1239 is explicit
that initial budgets are measurements to establish. A fixture that measures badly is a finding.

**Machine:** `OLE` — NVIDIA GeForce RTX 4090 (`NVIDIA GeForce RTX 4090/PCIe/SSE2`), OpenGL,
Deferred path, 1920x1080, render scale 1.0, TAA on, DDGI off, auto-exposure off.
**Method:** GPU timer queries (`GPUPassTimerPool`) on the final captured frame of each run, one
fresh capture process per run. Totals sum top-level passes only, so nested sub-passes are not
double counted.

### Determinism — MEASURED, and it is exact

Two fresh capture processes per fixture produced **byte-identical output on every attachment**:
44/44 for `reference-head`, `meadow` and `woodland`, 40/40 for both animal fixtures — 212
attachments in total, across all four cameras each, with the moving cameras and the skinned Walk
clip running. Every manifest therefore declares `Tolerance.RepeatRmse: 0.0`, not a small
tolerance.

That is stronger than the issue-#974 scenes (which document 0.05 for 1-LSB light-cull
accumulation noise) because these fixtures run no SSGI/SSR temporal resolve over a clustered light
cull. Camera motion does not weaken it: the pose is a closed form of the frame index.

### The headline timing finding: identical images, 2-4x frame-time spread

A single capture's frame total is **not a gateable number on this harness.** Across N=6 identical
Release runs of the same manifest, with byte-identical images every time:

| Fixture | N | Median frame | Range | Spread |
|---|---:|---:|---|---:|
| `reference-head` | 6 | 3.56 ms | 2.74 - 10.76 | 3.92x |
| `meadow` | 6 | 15.02 ms | 10.68 - 18.16 | 1.70x |
| `woodland` | 6 | 11.72 ms | 8.41 - 16.98 | 2.02x |

The capture harness records one frame's timings at the end of a fresh process, and that frame's
GPU state is cold in a way that varies run to run. `ScenePass/DepthPrepass` is the worst single
offender (`meadow`, N=6: 1.71 / 2.13 / 3.41 / 4.55 / 7.12 / 8.06 ms — 4.7x on identical input),
but no pass is immune, and the same spread is what made an earlier Debug-vs-Release comparison
look like a configuration difference when it was only run-to-run noise.

**So: never set a gate from one capture.** Gate on the median of N>=5 runs, and read per-pass
medians rather than frame totals.

### Per-pass cost (N=3, medians)

Passes whose median is stable to a few percent across runs. `ScenePass/Color`,
`ScenePass/DepthPrepass`, `DeferredLightingPass` and `ShadowPass` each showed at least one outlier
run; their ranges are given where the spread matters.

| Fixture | Draws | Triangles | Tracked GPU mem | Heaviest pass (median) |
|---|---:|---:|---:|---|
| `reference-head` | 38 | 62,303 | 598 MiB | `DeferredLightingPass` 1.33 ms |
| `animal-short-coat` | 30 | 2,370 | 482 MiB | `DeferredLightingPass` 1.28 ms |
| `animal-long-coat` | 30 | 4,218 | 476 MiB | `DeferredLightingPass` 1.36 ms |
| `meadow` | 76 | 3,365,412 | 489 MiB | `ScenePass` 9.50 ms |
| `woodland` | 76 | 985,996 | 491 MiB | `FoliagePass` 1.75 ms |

The two fixture classes are bound by different things, and that is the durable result:

- **Character fixtures are shading-bound.** At 2-62k triangles their geometry cost is noise;
  `DeferredLightingPass` (1.28-1.36 ms) dominates and barely moves between a 2,370-triangle fox
  and a 62,303-triangle head. Adding a real groom or a skin shader will show up here, not in
  `ScenePass`.
- **Vegetation fixtures are geometry- and shadow-bound.** `meadow` spends 9.50 ms in `ScenePass`
  against 1.36 ms of deferred lighting at 3.4M triangles, plus 2.76 ms of `ShadowPass`.
  `woodland` renders a third of the meadow's triangles and adds a dedicated `FoliagePass`
  (1.75 ms, the most repeatable pass measured anywhere in this set).

### Proposed gates

Set on the **median of N>=5 runs**, with headroom over the measured median, per the finding above.
A single-capture gate at these values would flap.

| Fixture | Measured median (N) | Proposed gate (median of N>=5) |
|---|---:|---:|
| `reference-head` | 3.56 ms (6) | 6.0 ms |
| `animal-short-coat` | 1.82 ms (3) | 4.0 ms |
| `animal-long-coat` | 1.90 ms (3) | 4.0 ms |
| `meadow` | 15.02 ms (6) | 22.0 ms |
| `woodland` | 11.72 ms (6) | 18.0 ms |

Per-pass gates are the better instrument where a pass is stable — `FoliagePass` on `woodland`
(1.744-1.749 ms over three runs) would catch a foliage regression far more sharply than the frame
total it sits inside.

**Untested tiers have no number, and none is invented here.** These were measured on an RTX 4090
only. No mid-range or integrated GPU, no other driver, no other OS, and no other resolution was
measured, so this table says nothing about them. Adding a tier means running the fixtures on it.
Vulkan was verified for correctness (below) but not timed; the numbers above are OpenGL.

### Cross-backend check

`reference-head` was captured under `--rhi=vulkan` through the editor front door
(`olo_benchmark_capture`), all four cameras including the moving one, `warmupTimedOut: false`. The
Vulkan frame matches the OpenGL frame: same subjects, same rig, same contact shadows, same
specular response. `LinearDepth` is skipped in that host by design — the editor camera seam cannot
pin the manifest's near/far clips, so metric linear depth is only available from the test binary.

### Findings the fixtures already produced

1. **The mesh-foliage canopy does not write the G-Buffer.** In `woodland`, the canopy is present
   in `Beauty` and in `ShadowMapCSMCascade0` but **absent from both `GBufferAlbedo` and
   `GBufferNormal`** — they are empty above the horizon while the terrain and the understory
   billboards appear normally. Every G-Buffer-derived term therefore treats the canopy as sky:
   SSAO, SSGI and SSR get no canopy occluder, and the derived `Roughness` lane is blank there.
   It is a large part of why the fixture's forest floor is lit like open ground. **Reported, not
   fixed** — `FoliageRenderPass` belongs to the in-flight #1230 work.
2. **The editor capture front door drew the grid and world axis into its captures.** It disabled
   the viewport helpers on the `Scene`, but `EditorLayer` re-pushes them from `RendererSettings`
   every frame, so the disable was overwritten before the first warm-up frame. Found by looking at
   a Vulkan capture; **fixed** in `McpToolsBenchmark.cpp` by clearing them in `RendererSettings`
   (which the existing epilogue already restores). Its restore path also hardcoded `true`,
   switching helpers on for a user who had them off.
3. **Hemi-octahedral impostors smear when viewed from above.** An early `woodland` camera placed
   above the canopy produced vertical streaking across the whole near field. The committed cameras
   sit at understory height, which is the view the impostor bake is built for; anyone measuring a
   top-down canopy should expect this.
4. **The velocity AOV needs `Normalize: on` to be readable at all.** Screen-space velocity for
   these shots spans roughly -0.004..0.001, so an unnormalized PNG quantises the entire field to 0
   or 1 of 255 — the moving sequence's headline AOV renders as a black frame. Absolute values stay
   in `result.json`'s per-attachment min/max.
