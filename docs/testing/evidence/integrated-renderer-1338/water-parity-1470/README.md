# Integrated benchmark water: GL / Vulkan geometry parity (issue #1470)

The water tile renders with the same geometry on both backends and all three render paths once the
capture time is pinned. The difference #1470 reported came from the editor capture host's live
clock, and was amplified by the surface being displaced twice. Both are fixed in the #1470 PR.

## What changed the picture

1. **The editor capture host now pins the clock** (`olo_benchmark_capture`): `t0 + n * dt` from the
   manifest's `Determinism` block, like the test binary. Before, the host used the live clock, so
   each capture drew the 10 m tile under a 180 m FFT swell at a different wave phase.
2. **The water surface is displaced once.** With tessellation off (the `WaterComponent` default), the
   vertex stage displaced the surface and the tess-eval stage displaced it again, doubling the swell
   and the sideways drift.

## Measurements

The near-edge metric takes, for each 10th row, the first run of 8 water-blue pixels and compares its
x position between two frames (`Stationary_*`, 1920 x 1080 sources).

| pair (integrated-deferred, `stationary`) | clock | median / p90 edge offset |
|---|---|---|
| GL test binary vs Vulkan editor, before either fix (`vk-deferred-live-01`) | live | 620 px / 765 px |
| GL editor vs Vulkan editor, pinned, before the displacement fix (`*_PinnedBeforeFix`) | 24.1833 s | 1 px / 16 px |
| GL test binary vs Vulkan editor, both fixes (`*_AfterFix`, `*-deferred-pinned/`) | 24.1833 s | 2 px / 32 px |

The p90 outliers are grass blades crossing the edge.

Water-angle cells (`WaterParity_<Backend>_<Path>_<Angle>.png`, three cameras framing the tile,
downscaled from 1920 x 1080). GL: test binary. Vulkan: editor. Same pinned clock.

| cell | measure | result |
|---|---|---|
| Vulkan Forward vs GL Forward (both editor) | strong differences (> 20/255) outside the GL water mask grown by 6 px | 152 / 0 / 84 px (Above / Side / Grazing) of 2.07 M |
| Vulkan Forward+ vs GL Forward (both editor) | same | 1134 / 0 / 633 px |
| GL Forward+ vs GL Forward (test binary) | pixels > 8/255 | 0 / 7 / 1 px |
| Vulkan Deferred vs GL Deferred | water-mask disagreement after a 5 px opening | 158 / 319 px (Above / Side); Grazing not comparable, see below |

The GL-editor Forward reference frames are not committed (same host as the Vulkan ones, used only
for this measurement).

## Known shading differences (not geometry)

- **Vulkan Forward and Forward+** draw the tile as a grey-white mottled sheet: the water samples a
  scene-depth snapshot at its own height, so shoreline foam covers the surface. Tracked in #1486.
- **GL Deferred (test binary), Grazing** draws the tile white where the other cells draw it blue.
  Observed, not investigated; noted on #1486.

## Reproduce

`OloEngine-Tests.exe --olo-capture-manifest=OloEditor/assets/benchmark/manifests/integrated-deferred.yaml`
for GL; `olo_benchmark_capture` with the same manifest in an editor launched with `--rhi=vulkan` for
Vulkan. The water-angle manifests are `integrated-<path>.yaml` with the three cameras below, 32 warm-up
frames and 100 measured frames:

| camera | position | yaw / pitch (deg) | FOV |
|---|---|---|---|
| `water-grazing` | (7, 4.2, 18) | 0 / 13 | 60 |
| `water-above` | (7, 13, 14.5) | 0 / 58 | 60 |
| `water-side` | (19, 4.6, 7) | -90 / 18 | 60 |

Host: i7-14700KF, RTX 4090 (driver 617.14), Windows 11, Debug build, clang-cl. Vulkan run logs: 0
VUIDs; the only errors are the Forward+ storage-binding lines tracked in #1487.
