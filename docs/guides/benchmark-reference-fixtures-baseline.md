# Reference fixture measured baseline (issue #1239)

The measurement log for the five reference fixtures. The contract they are captured under is
[benchmark-reference-fixtures.md](benchmark-reference-fixtures.md).

**Append, do not overwrite.** Each re-measure is a new dated block naming its machine and build
configuration, so a number can always be traced to the run that produced it.

## 2026-09-13 — RTX 4090, Debug, OpenGL

Everything below is a **measurement taken on one named machine**, not a forecast. Nothing here is
a performance promise, and none of it licenses reducing the AAA ambition — issue #1239 is explicit
that initial budgets are measurements to establish. A fixture that measures badly is a finding.

**Machine:** `OLE` — NVIDIA GeForce RTX 4090 (`NVIDIA GeForce RTX 4090/PCIe/SSE2`), OpenGL,
Deferred path, 1920x1080, render scale 1.0, TAA on, DDGI off, auto-exposure off.
**Method:** GPU timer queries (`GPUPassTimerPool`) on the final captured frame of each run, N=3
fresh capture processes per fixture. Totals sum top-level passes only, so nested sub-passes are
not double counted.

### Determinism — MEASURED, and it is exact

Two fresh capture processes per fixture produced **byte-identical output on every attachment**:
44/44 for `reference-head`, `meadow` and `woodland`, 40/40 for both animal fixtures — 212
attachments in total, across all four cameras each, with the moving cameras and the skinned Walk
clip running. Every manifest therefore declares `Tolerance.RepeatRmse: 0.0`, not a small
tolerance.

That is stronger than the issue-#974 scenes (which document 0.05 for 1-LSB light-cull
accumulation noise) because these fixtures run no SSGI/SSR temporal resolve over a clustered light
cull. Camera motion does not weaken it: the pose is a closed form of the frame index.

### Frame cost

| Fixture | Draws | Triangles | Tracked GPU mem | GPU frame min / **med** / max (ms) |
|---|---:|---:|---:|---|
| `reference-head` | 38 | 62,303 | 598 MiB | 2.36 / **2.38** / 8.02 |
| `animal-short-coat` | 30 | 2,370 | 479 MiB | 1.80 / **1.81** / 2.22 |
| `animal-long-coat` | 30 | 4,218 | 476 MiB | 2.15 / **2.48** / 2.75 |
| `meadow` | 76 | 3,365,412 | 489 MiB | 8.41 / **8.43** / 9.46 |
| `woodland` | 76 | 985,996 | 491 MiB | 11.42 / **12.76** / 13.20 |

**Read the median, not the max.** At N=3 the max column is dominated by first-run warm-up: the
`reference-head` max of 8.02 ms is a single 5.90 ms `SSRPass` sample against a 0.45 ms median,
which is pipeline/shader warm-up, not frame cost. This repo's GPU-timing guidance expects swings
of this size on cold GPU state.

Dominant pass per fixture (median):

| Fixture | Heaviest pass | ms | Second | ms |
|---|---|---:|---|---:|
| `reference-head` | `DeferredLightingPass` | 1.34 | `SSRPass` | 0.45 |
| `animal-short-coat` | `DeferredLightingPass` | 1.28 | `SSAOPass` | 0.18 |
| `animal-long-coat` | `DeferredLightingPass` | 1.93 | `SSAOPass` | 0.20 |
| `meadow` | `ShadowPass` | 3.21 | `ScenePass` | 3.23 |
| `woodland` | `ScenePass` | 7.58 | `FoliagePass` | 1.97 |

The vegetation fixtures are **geometry-bound, and shadow-bound before they are shading-bound** —
the meadow spends more in `ShadowPass` (3.21 ms) than in `DeferredLightingPass` (1.52 ms) at
3.4M triangles. The character fixtures are the opposite: at 2-62k triangles they are entirely
deferred-lighting-bound and their geometry cost is noise.

### Proposed gates — and what they are not

These are **regression gates for this machine and this configuration**, set with headroom over the
measured median so ordinary run-to-run variation does not trip them:

| Fixture | Measured median | Proposed gate |
|---|---:|---:|
| `reference-head` | 2.38 ms | 4.0 ms |
| `animal-short-coat` | 1.81 ms | 3.0 ms |
| `animal-long-coat` | 2.48 ms | 4.0 ms |
| `meadow` | 8.43 ms | 12.0 ms |
| `woodland` | 12.76 ms | 18.0 ms |

**Untested tiers have no number, and none is invented here.** These were measured on an RTX 4090
only. No mid-range or integrated GPU, no other driver, no other OS, and no other resolution was
measured, so this table says nothing about them. Adding a tier means running the fixtures on it.

**Not yet re-measured in a shipping configuration.** These come from a Debug host. GPU timer
queries measure GPU work and the shaders are driver-compiled identically, but a Debug host submits
more slowly and can leave the GPU idle inside a pass. Before any of these gates is enforced in CI
it should be re-measured in Release; treat the ratios between fixtures as the solid part and the
absolute milliseconds as provisional.

### Findings the fixtures already produced

1. **The mesh-foliage canopy does not write the G-Buffer.** In `woodland`, the canopy is present
   in `Beauty` and in `ShadowMapCSMCascade0` but **absent from both `GBufferAlbedo` and
   `GBufferNormal`** — they are empty above the horizon while the terrain and the understory
   billboards appear normally. Every G-Buffer-derived term therefore treats the canopy as sky:
   SSAO, SSGI and SSR get no canopy occluder, and the derived `Roughness` lane is blank there.
   It is a large part of why the fixture's forest floor is lit like open ground. **Reported, not
   fixed** — `FoliageRenderPass` belongs to the in-flight #1230 work.
2. **Hemi-octahedral impostors smear when viewed from above.** An early `woodland` camera placed
   above the canopy produced vertical streaking across the whole near field. The committed cameras
   sit at understory height, which is the view the impostor bake is built for; anyone measuring a
   top-down canopy should expect this.
3. **The velocity AOV needs `Normalize: on` to be readable at all.** Screen-space velocity for
   these shots spans roughly -0.004..0.001, so an unnormalized PNG quantises the entire field to 0
   or 1 of 255 — the moving sequence's headline AOV renders as a black frame. Absolute values stay
   in `result.json`'s per-attachment min/max.

