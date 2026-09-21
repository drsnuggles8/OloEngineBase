# Live captures — epic #1222 acceptance

**These are NOT goldens and no test regenerates them.** Everything else in
`assets/tests/visual/` is written by a headless evidence test on each run; these nine files were
taken by hand from a running editor and committed once, as the evidence for the matrix's
**live-only** cells (PR #1388).

They live in a subdirectory for exactly that reason, following
`flora-traversal-1224-live/`. A file sitting directly in `assets/tests/visual/` under the
`<Feature>_<Backend>_<Path>[_<Angle>].png` convention is a claim that some test produces it, and a
reviewer counting files would be entitled to go looking for the test that writes these. There
isn't one, and there cannot be: the headless fixtures need a real GL 4.6 context and skip without
one, so the suite *cannot* cover Vulkan (see `docs/process/task-loop.md` §2a).

## What each file is

All nine are captured from the editor's `SceneColor` target — not a window screenshot — with the
editor debug-draw overlays off, on an RTX 4090, 2026-09-21. The seven `DigitalHuman_*` files are
`Scenes/DigitalHuman.olo`; the two `ReferenceHeadStock_*` files are the **stock, untouched**
`Scenes/Benchmark/ReferenceHead.olo`.

Every `_VK_` file was taken from an editor launched with `--rhi=vulkan`, confirmed in the log as
`[RHI] Backend: Vulkan (source: --rhi flag)`, with **0 VUIDs and 0 `[error]` lines**.

| file | cell / purpose |
|---|---|
| `DigitalHuman_VK_Forward.png` | Vulkan × Forward |
| `DigitalHuman_VK_ForwardPlus.png` | Vulkan × Forward+ |
| `DigitalHuman_VK_Deferred.png` | Vulkan × Deferred |
| `DigitalHuman_GL_Deferred_LiveControl.png` | the **same pose on OpenGL**, so the three above are comparable against something rather than only against each other |
| `DigitalHuman_VK_Deferred_ProfileId.png` | the profile-identity AOV on Vulkan — the material debug views are deferred-only, and this is the evidence they work on the non-GL backend |
| `DigitalHuman_VK_Deferred_MSAA4.png` | the MSAA cell. The setting really applied: `SceneRenderPass: Created G-Buffer 1411x942 x4MSAA` |
| `DigitalHuman_VK_Deferred_UpscalePerformance.png` | the upscale cell, and it shows a **defect** (#1397) |
| `ReferenceHeadStock_VK_Deferred_UpscaleOff.png` | attribution control for #1397 |
| `ReferenceHeadStock_VK_Deferred_UpscalePerformance.png` | attribution: the same defect on a scene with **no skin in it** |

## The four camera-matched files

`DigitalHuman_VK_{Forward,ForwardPlus,Deferred}.png` and
`DigitalHuman_GL_Deferred_LiveControl.png` share one camera pose, so they are pixel-comparable.
Measured mean luma over the frame:

| backend | forward | forward+ | deferred |
|---|---|---|---|
| OpenGL | 0.2474 | 0.2473 | 0.2475 |
| Vulkan | 0.2474 | 0.2473 | 0.2475 |

## The upscale pair is NOT pixel-comparable, and that is the finding

`DigitalHuman_VK_Deferred_UpscalePerformance.png` and the two `ReferenceHeadStock_*` files
document #1397: a non-native `UpscaleMode` **crops and magnifies** the frame instead of upscaling
it to fill the viewport. The render scale itself applies correctly (`SceneColor` really does
resize 1024×683 → 705×471), so this is a presentation bug, not a scaling one.

The two stock-scene captures are the **attribution**: 0.5819 → 0.2493 mean luma with the subjects
cropped out of frame, on a committed benchmark fixture that PR #1388 does not touch, from a branch
that changes no engine or shader code. That is what makes the defect pre-existing rather than
something the skin work introduced.
