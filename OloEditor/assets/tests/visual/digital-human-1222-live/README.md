# Live captures — epic #1222 acceptance

**These are NOT goldens and no test regenerates them.** Everything else in
`assets/tests/visual/` is written by a headless evidence test on each run; these ten files were
taken by hand from a running editor and committed once, as the evidence for the matrix's
**live-only** cells (PR #1388).

They live in a subdirectory for exactly that reason, following `flora-traversal-1224-live/`. A
file sitting directly in `assets/tests/visual/` under the
`<Feature>_<Backend>_<Path>[_<Angle>].png` convention is a claim that some test produces it, and a
reviewer counting files would be entitled to go looking for the test that writes these. There
isn't one, and there cannot be: the headless fixtures need a real GL 4.6 context and skip without
one, so the suite *cannot* cover Vulkan (see `docs/process/task-loop.md` §2a).

## The subject

`Scenes/DigitalHuman.olo` — one real human head (`InfiniteScanHead`, CC BY 3.0, attribution in
that model's README) under a three-rig studio setup. Captured from the editor's `SceneColor`
target rather than as a window screenshot, with the editor debug-draw overlays off, on an
RTX 4090, 2026-09-21.

Every `_VK_` file came from an editor launched with `--rhi=vulkan`, confirmed in the log as
`[RHI] Backend: Vulkan (source: --rhi flag)`, with **0 VUIDs and 0 `[error]` lines**.

| file | cell / purpose |
|---|---|
| `DigitalHuman_VK_Forward.png` | Vulkan × Forward |
| `DigitalHuman_VK_ForwardPlus.png` | Vulkan × Forward+ |
| `DigitalHuman_VK_Deferred.png` | Vulkan × Deferred |
| `DigitalHuman_GL_Deferred_LiveControl.png` | the **same pose on OpenGL**, so the three above are comparable against something rather than only against each other |
| `DigitalHuman_VK_Deferred_ProfileId.png` | the profile-identity AOV on Vulkan |
| `DigitalHuman_VK_Deferred_Diffuse.png` | the diffuse AOV on Vulkan |
| `DigitalHuman_VK_Deferred_MSAA4.png` | the MSAA cell — the setting really applied: `SceneRenderPass: Created G-Buffer 1411x942 x4MSAA` |
| `DigitalHuman_VK_Deferred_UpscalePerformance.png` | the upscale cell, and it shows a **defect** (#1397) |
| `ReferenceHeadStock_VK_Deferred_UpscaleOff.png` | attribution control for #1397 |
| `ReferenceHeadStock_VK_Deferred_UpscalePerformance.png` | attribution: the same defect on a scene with **no skin in it** |

## The four camera-matched files

`DigitalHuman_VK_{Forward,ForwardPlus,Deferred}.png` and
`DigitalHuman_GL_Deferred_LiveControl.png` share one camera pose, so they are pixel-comparable.
Mean luma over the frame:

| backend | forward | forward+ | deferred |
|---|---|---|---|
| OpenGL | — | — | 0.1792 |
| Vulkan | 0.1788 | 0.1788 | 0.1792 |

Vulkan deferred matches the OpenGL control to four decimals.

## What the profile-identity capture does and does not show

It is **one hue over the head and black everywhere else**, and that is correct: this scene has one
visible skin profile. It is evidence that the material debug views — which are **deferred-only**,
`RenderPipeline.cpp` forces `None` on the forward paths — work on the non-GL backend at all.

It is **not** evidence about the seven-slot profile budget. An earlier revision of this scene
carried six extra primitives to keep that count up; they were removed (see the scene header for
why that was a bad idea). The slot-budget finding is asserted in
`SkinDigitalHumanEvidenceTest`, on a controlled multi-profile probe, and written up in #1393.

## The upscale captures are NOT pixel-comparable, and that is the finding

They document #1397: a non-native `UpscaleMode` **crops and magnifies** the frame instead of
upscaling it to fill the viewport. The render scale itself applies correctly (`SceneColor` really
does resize 1024×683 → 705×471), so this is a presentation bug, not a scaling one.

The two `ReferenceHeadStock_*` captures are the **attribution**: 0.5819 → 0.2493 mean luma with
the subjects cropped out of frame, on the committed `Benchmark/ReferenceHead.olo` fixture that
PR #1388 does not touch, from a branch that changes no engine or shader code. That is what makes
the defect pre-existing rather than something the skin work introduced.
