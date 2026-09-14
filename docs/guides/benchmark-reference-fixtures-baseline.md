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

**Raw samples**, so the medians above are recalculable rather than asserted (top-level pass sum
per run, ms, sorted):

| Fixture | The six runs |
|---|---|
| `reference-head` | 2.74 / 2.94 / 3.24 / 3.88 / 4.59 / 10.76 |
| `meadow` | 10.68 / 12.68 / 13.58 / 16.46 / 17.28 / 18.16 |
| `woodland` | 8.41 / 9.28 / 11.38 / 12.05 / 15.65 / 16.98 |
| `meadow` `ScenePass/DepthPrepass` | 1.71 / 2.13 / 3.41 / 4.55 / 7.12 / 8.06 |

Note `reference-head`: five runs inside 2.74-4.59 and one at 10.76. The median is robust to that;
a mean would not be, which is the second reason not to gate on a single capture.

**Exact inputs.** Branch base `1fe145af7`; each run is a fresh process invoked as

```powershell
build-cached/OloEngine/tests/Release/OloEngine-Tests.exe `
  --olo-capture-manifest=OloEditor/assets/benchmark/manifests/<id>.diagnostic.yaml `
  --olo-capture-out=<dir>
```

Windows 11 Pro 10.0.22631, clang-cl via the `dev-cached` preset, Release. Every capture's own
`result.json` additionally records the commit SHA, backend, GPU strings, machine tag, applied
settings and the full per-pass table — that file is the machine-readable record, and result
directories are git-ignored by the issue-#974 contract rather than committed.

**Known gap in the record:** the capture does not record a GPU *driver version* — the GL renderer
string (`NVIDIA GeForce RTX 4090/PCIe/SSE2`) carries none. Two runs on different drivers are
therefore indistinguishable from the result directory alone. Worth closing before these numbers
are compared across a driver update.

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
| `woodland` | 76 | 985,996 | 491 MiB | `ScenePass` 3.72 ms † |

† `woodland`'s heaviest-pass cell is the only one re-measured after PR #1265 (2026-09-14,
master `5acc47238`, N=5) — before it, this row read `FoliagePass` 1.75 ms. Its Draws, Triangles
and GPU-mem columns are the original N=3 figures and were confirmed unchanged by the re-run
(76 / 985,996 / 491 MiB), so only the pass attribution moved.

The two fixture classes are bound by different things, and that is the durable result:

- **Character fixtures are shading-bound.** At 2-62k triangles their geometry cost is noise;
  `DeferredLightingPass` (1.28-1.36 ms) dominates and barely moves between a 2,370-triangle fox
  and a 62,303-triangle head. Adding a real groom or a skin shader will show up here, not in
  `ScenePass`.
- **Vegetation fixtures are geometry- and shadow-bound.** `meadow` spends 9.50 ms in `ScenePass`
  against 1.36 ms of deferred lighting at 3.4M triangles, plus 2.76 ms of `ShadowPass`.
  `woodland` renders a third of the meadow's triangles in `ScenePass` (3.72 ms) — its canopy used
  to sit in a dedicated forward `FoliagePass` at 1.75 ms. Since PR #1265 a Deferred frame whose
  G-Buffer foliage programs loaded routes **every** foliage layer, impostor and billboard alike,
  to `ScenePass`, so `FoliagePass` receives no commands and does not appear in `passTimingsMs`.
  `FoliagePass` is still the live path for Forward/Forward+ and for a Deferred frame missing one
  of those programs, which the engine warns about once.

### Proposed gates

Set on the **median of N>=5 runs**, with headroom over the measured median, per the finding above.
A single-capture gate at these values would flap.

| Fixture | Measured median (N) | Proposed gate (median of N>=5) |
|---|---:|---:|
| `reference-head` | 3.56 ms (6) | 6.0 ms |
| `animal-short-coat` | 1.82 ms (3) | 4.0 ms |
| `animal-long-coat` | 1.90 ms (3) | 4.0 ms |
| `meadow` | 15.02 ms (6) | 22.0 ms |
| `woodland` | 11.72 ms (6) | 18.0 ms ‡ |

‡ `woodland`'s frame total was measured before PR #1265 moved its canopy out of `FoliagePass`
and into `ScenePass`. Only the per-pass numbers were re-measured afterwards, so treat this row as
provisional until someone re-runs the frame total; nothing here estimates what it became.

Per-pass gates are the better instrument where a pass is stable. The `FoliagePass` figure this
section originally quoted for `woodland` (1.744-1.749 ms over three runs) no longer exists: since
PR #1265 the impostor canopy draws in `ScenePass` and `FoliagePass` is culled to zero commands, so
a foliage gate on this fixture belongs on `ScenePass`. Measured there on master `5acc47238`
(2026-09-14, RTX 4090, Debug, OpenGL): median **3.723 ms** over five back-to-back runs, range
3.652-3.738 — but a sixth run issued minutes later, after a 35 s test-suite run on the same GPU,
measured **4.827 ms**. So the tight spread is a within-session property, not a property of the
pass: a gate here needs headroom for the machine's warm-up state, exactly as the frame-total
gates above do. Do not quote the 2% figure as the tolerance.

**Untested tiers have no number, and none is invented here.** These were measured on an RTX 4090
only. No mid-range or integrated GPU, no other driver, no other OS, and no other resolution was
measured, so this table says nothing about them. Adding a tier means running the fixtures on it.
Vulkan was verified for correctness (below) but not timed; the numbers above are OpenGL.

### Cross-backend check — all five match

All five fixtures were captured under `--rhi=vulkan` through the editor front door
(`olo_benchmark_capture`), all four cameras each including the moving one, every run
`warmupTimedOut: false`.

| Fixture | Subject type | OpenGL vs Vulkan |
|---|---|---|
| `reference-head` | static models | matches |
| `animal-short-coat` | skinned + animated | matches |
| `animal-long-coat` | static mesh | matches |
| `meadow` | BILLBOARD foliage | matches |
| `woodland` | impostor canopy + billboard understory | matches (since PR #1265) |

**The `woodland` canopy was absent on Vulkan when these fixtures were first run** (2026-09-13):
where OpenGL rendered a dense canopy with trunks and sky gaps, Vulkan rendered only scattered
disconnected fragments at the horizon. **Fixed by PR #1265** (closing #1264) — `ImpostorBaker` was
the one capture path that skipped `RHIProjectionSeam`, so its GL-convention ortho put the mesh at
negative clip z, which Vulkan clips before rasterization and the atlas baked as its clear colour.

Re-verified 2026-09-14 on master `5acc47238` for issue #1267: all four cameras on `--rhi=vulkan`
render a dense canopy with trunks and sky gaps, comparable to OpenGL, with the canopy present in
`GBufferAlbedo` and `GBufferNormal` on `frontal` and `grazing`, `attachmentFailures: 0`,
`warmupTimedOut: false` and 0 shader errors. `meadow` was re-captured on Vulkan as the control
and is unchanged.

`LinearDepth` is skipped in the editor host by design — the editor camera seam cannot pin the
manifest's near/far clips, so metric linear depth is only available from the test binary.

**Do not compare editor-host captures pixel-for-pixel against test-binary ones.** The editor runs
a live clock with its own frame pacing (`result.json` records `host: "editor-mcp"` and says so);
the comparison above is qualitative — same subject, same pose, same rig behaviour.

### Editor-host capture races the editor's own warm-up

Applying a manifest's output resolution to a freshly launched Vulkan editor **asserts and kills
the process**:

    GTAORenderPass: scene depth/normals did not resolve (publishing fully visible AO)
    AOApplyRenderPass: missing input/output (inputTex=<null>, ..., depthTex=<null>)
    Assertion Failed: AOApplyRenderPass enabled without resolved graph input/output

Observed 2026-09-13 when a capture was issued ~10 s after the MCP server came up, while scene
mesh optimisation and IBL loading were still in flight; the viewport override resized 941x628 ->
1920x1080 into a graph that had not yet published depth/normals. The identical capture succeeds
and the editor survives when given ~60 s to settle first, and all five fixtures then captured
back to back without incident. Pre-existing in the issue-#974 editor front door, not introduced
by the fixtures. **Let the editor settle before the first capture.**

### Findings the fixtures already produced

1. **The impostor canopy did not write the G-Buffer.** In `woodland`, the canopy was present in
   `Beauty` and in `ShadowMapCSMCascade0` but **absent from both `GBufferAlbedo` and
   `GBufferNormal`**, so every G-Buffer-derived term treated it as sky and SSAO/SSGI/SSR got no
   canopy occluder. The cause was routing: an impostor layer (`Impostor.Enabled` with a valid
   atlas) had no deferred program, so it fell to the forward `FoliagePass`, which runs after
   `DeferredLightingPass` and writes only `SceneColor`. **Fixed by PR #1265**, which added
   `Foliage_Impostor_GBuffer.glsl`; `SelectFoliageRenderStream` now asks only whether the
   G-Buffer sibling loaded, so the canopy routes through `ScenePass` instead.
   The split was `UseImpostor`, not mesh-vs-billboard — no foliage layer draws mesh geometry at
   all, every layer is a card quad (`FoliageRenderer::BuildQuadGeometry`), and `MeshPath` only
   feeds the impostor bake. Real plant meshes are #1233.

   Re-verified 2026-09-14 on master `5acc47238` for issue #1267: the canopy is present in
   `Albedo`, `Normals` and `AOBuffer` on the `frontal` and `grazing` cameras — the two the
   original report named, and the only two re-read at AOV level. `FoliagePass` no longer appears
   in `result.json`'s `passTimingsMs` at all, in 5 of 5 runs — the whole canopy moved to
   `ScenePass`.
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
