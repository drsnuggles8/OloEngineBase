# Live-editor captures for the groom epic's acceptance (#1223)

**No test regenerates these files.** They were captured by hand from a Release `OloEditor` over MCP
(`olo_screenshot`) on an RTX 4090, playing `Scenes/GroomAnimals.olo` with TAA on unless the name says
otherwise. They are the live-only cells of the verification matrix. The headless fixture cannot
produce a Vulkan frame, and every ray-traced cell is Vulkan Deferred only.

| file | cell | what the log said |
|---|---|---|
| `GroomAnimals_GL_{Forward,ForwardPlus,Deferred}_Live.png` | OpenGL × each path | 0 `[error]` lines |
| `GroomAnimals_VK_{Forward,ForwardPlus,Deferred}_Live.png` | Vulkan × each path | `[RHI] Backend: Vulkan (source: --rhi flag)`; 0 VUIDs, 0 `[error]` |
| `GroomAnimalsRt{Off,On}_VK_Deferred_Live.png` | Vulkan RT hybrid shadows off / on | with RT on, `olo_rt_scene_stats`: instances traced 5 → 8, all three coats represented on the detailed tier, 744,664 proxy triangles, 56.6 MB resident; 0 VUIDs |
| `GroomAnimals_VK_Deferred_Msaa4_Live.png` | MSAA 4× | `Created G-Buffer 1411x942 x4MSAA` |
| `GroomAnimals_VK_Deferred_UpscaleQuality_Live.png` | FSR1 quality upscale | the #1397 crop, and the coats misregistered against their bodies (#1430) |
| `GroomAnimalsNoTaa_GL_Forward_Live.png` | TAA off (the scene's default) | the coats fall back to the opaque tier and vanish (#1429) |

The subjects move, so no two captures show the same pose, and these frames are not pixel-comparable
with each other. The coated captures show all three coated subjects; the upscale capture records
their misregistration. The no-TAA capture shows the fallback in which the coats vanish. The measured
claims are in `GroomAnimalsAcceptanceEvidenceTest`.
