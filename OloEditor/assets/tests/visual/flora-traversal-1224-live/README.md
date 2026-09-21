# Live Vulkan captures — epic #1224 acceptance

**These are NOT goldens and no test regenerates them.** Everything else in
`assets/tests/visual/` is written by a headless evidence test on each run; these five files were
taken by hand from a running editor and committed once, as the evidence for the matrix's
**live-only** cells (PR #1389).

They live in a subdirectory for exactly that reason. A file sitting directly in
`assets/tests/visual/` under the `<Feature>_<Backend>_<Path>[_<Angle>].png` convention is a claim
that some test produces it, and a reviewer counting files would be entitled to go looking for the
test that writes these. There isn't one, and there cannot be: the headless fixtures need a real
GL 4.6 context and skip without one, so the suite *cannot* cover Vulkan (see
`docs/process/task-loop.md` §2a).

## What each file is

All five are `Scenes/FoliageMeadowToWoodland.olo` on an RTX 4090, editor launched with
`--rhi=vulkan` (confirmed in the log as `[RHI] Backend: Vulkan (source: --rhi flag)`), captured
2026-09-21.

| file | cell |
|---|---|
| `FloraTraversal_VK_Forward_Woodland.png` | Vulkan × Forward |
| `FloraTraversal_VK_ForwardPlus_Woodland.png` | Vulkan × Forward+ |
| `FloraTraversal_VK_Deferred_Woodland.png` | Vulkan × Deferred |
| `FloraTraversal_VK_Deferred_Overview.png` | Vulkan × Deferred, the whole meadow-to-woodland composition |
| `FloraTraversal_VK_Deferred_UpscaleQuality.png` | the upscale cell — FSR1 quality (FSR2 falls back to FSR1 spatial on this backend) |

The three `_Woodland` captures share one camera pose, so they are comparable to each other. The
upscale capture is **not** pixel-comparable to the others: the upscaler changes the internal render
resolution and the framing differs, so it evidences "the flora still renders correctly here",
not a pixel A/B.

## Refreshing them

There is no command. Attach a Vulkan editor session, open the scene, and drive
`olo_renderer_settings_set {setting: "renderpath", value: ...}` — reading the applied `value` back
rather than trusting `changed: true`, and confirming `olo_scene_summary`'s `name` first, because a
`olo_scene_open` that times out against a cold Vulkan shader cache leaves the *previous* scene
loaded and every capture afterwards looks healthy and is wrong.
