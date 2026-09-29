# Renderer documentation reconcile (#1357, 2026-09-29)

**The rule: a renderer comment or doc states behaviour at a named commit, backed by the declaration,
the implementation and a consumer that make it true. A closed issue, a ticked checkbox or a merged PR
title is not that evidence.** Every correction below was read from the code at `7c5aa1b98`. The
September review (`3b2e23a`) was static; so is this reconcile, except where a test is named.

`scripts/check_docs_consistency.py` (pre-commit hook `docs-consistency`) now catches the mechanical
half of this drift: dead links and anchors, docs missing from [the index](../README.md), dead
`docs/…` paths in source, and renamed files or symbols in the renderer contract docs. It proves
nothing about whether a sentence is true.

## Corrections

| Area | Was | Now, with evidence | Corrected in |
|---|---|---|---|
| Packets | "links to other packets"; alignment "64 bytes" read as the alignment | No link field; `CommandBucket` orders packets through `m_Keys`/`m_Packets`. `COMMAND_ALIGNMENT` is 16 (`CommandAllocator.h`); the header is 64 bytes. `m_ExecutionOrder` is read only by frame capture and the freeze digest. | `CommandPacket.h` |
| Material routing | Deferred overlay draws "skybox, terrain, foliage"; foliage and decals only in their own passes | Skybox, terrain and voxels switch to `*_GBuffer` shaders (`Renderer3DUtilityDraws.cpp`); Deferred foliage goes into the G-Buffer (`SelectFoliageRenderStream`); opaque decals go through `DeferredOpaqueDecalPass`. The overlay draws the grid, blended/transmissive meshes, meshes with no G-Buffer shader, debug draws and shader-load fallbacks. | `SceneRenderPass.h`, `ForwardOverlayRenderPass.{h,cpp}`, `DecalRenderPass.h`, `WaterRenderPass.h`, `GPUDrivenOcclusionPass.h`, `transparent-draw-order.md` |
| Blended on Deferred | #1404 tripwire "fails the day #1404 is fixed" | Rerouted to the overlay (`DeferredForwardOverlayRoute.h`); `TransparentBlendOrderVisualEvidence.DeferredBlendsTheNearerQuadLast` asserts order. | `transparent-draw-order.md` |
| Reflection composite | Tiers "lerp over the colour"; `mix(base, L, 0)` | Each tier replaces the indirect specular term: `C + c·(W·L − S)` (`ReflectionTierComposite.glsl`), and hands `S` on to SSR (#1325). | `RayTracedReflectionPass.{h,cpp}`, `ReflectionTier.h`, `RenderPipelineBuilderPost.cpp`, ADR 0020 |
| Alpha testing in RT | Masked candidates "reflect / shadow as solid (#805)" | Masked instances are built non-opaque (`RayTracingScene.cpp`, `ForceOpaque`); shadows and reflections alpha-test through the heap (`HybridRayTracingAlpha.glsl`) and stand down if it is unresolved. Without the material texture table the GPU path tracer and ReSTIR GI trace masked geometry as solid, and ReSTIR DI tests only the factor alpha; all three count it. | `RayTracedReflectionPass.{h,cpp}`, `RayTracedReflection.glsl`, `PostProcessSettingsPanel.cpp`, `RayTracingAlphaTest.glsl`, `vulkan-ray-tracing-acceleration-structures.md`, ADR 0020 §4 note |
| VSM local lights | Atlas "unaffected" by VSM; terrain "still renders through CSM" | With VSM on the CSM cascades are cleared and skipped; with `LocalLights` (default) the atlas has zero entries (`Scene.cpp`), so terrain, foliage and voxels cast nothing. Virtual geometry reaches clip levels only. | `VirtualShadowMap.h`, `ShadowRenderPass.cpp`, `virtual-shadow-map-page-cache.md` |
| Groom shadows | "casts no shadow … including from itself"; owner #1323 | The coat self-shadow volume exists (#1248). Scene shadows landed in PR #1380 and were deleted by the #1382 merge (`6ac29408a`, via `ed7220686`). | `GroomRenderPass.h`, support matrix, `virtual-geometry-into-a-second-shadow-technique.md` |
| Reference PT | "If you ever *do* need the GPU version"; "a future GPU path tracer" | `GpuPathTracerPass` (#1055) exists, consumes the v2 closure through `PathTracerBSDF.glsl`, and runs over the full raster graph (#1346). | `reference-path-tracer.md`, `gpu-path-tracer.md`, ADR 0016 |
| Buffer ordering | `VulkanVertexBuffer` "a seam, not fixed"; `ClearSubData` | Fixed in #1171, replaced by the #1351 policy; the ranged clear is `ClearData(offset, size)`. `StreamCommandOrdered` added to the refusal and the audit table. | `vulkan-command-ordered-buffer-writes.md` |
| Diagnostic limits | `olo_shader_list` "over-reports"; GLSL error "crashes a Debug editor"; no `olo_terrain_pick`; `olo_shader_errors` `notInitialized` "on Vulkan" | Pass-owned shaders reload through `ShaderRegistry`; compile failures return `failed` since #568 (`ShaderCompileFailureRecoveryTest`); `olo_terrain_pick` shipped in PR #1028; Release answers `unavailableInThisBuild`. | `notes-mcp-tool-authoring.md`, `mcp-diagnostics-server.md`, `McpToolsShader.cpp`, `terrain-gpu-lod-quadtree.md`, `task-loop.md` |
| Benchmarks | #1397 crop, #1486, #1487 as current blockers | Fixed by PR #1443 and PR #1517. Tables are unchanged and annotated with current owners. | `integrated-renderer-budgets-1338.md`, `renderer-benchmarks.md` |

## Why convex reflection weights were not an energy proof

ADR 0020 called `Σ w_t = 1` "the whole no-double-count guarantee" and "energy conservation as a
ratchet". The weights did sum to one, but the shaders applied them to the whole lit colour, so
diffuse, emission and direct light were scaled by `1 − c`, and the operands were in different units
(`S` already carried the split-sum weight). A convex combination conserves its weights, not energy.
[ADR 0020 §9](../adr/0020-reflection-tier-selection-contract.md#9-review-correction-convex-weights-are-not-an-energy-proof-2026-09-29-1357)
records the mechanism, what holds since #1325, the pinning tests, and the one unexecuted part: no
automated test runs the ray tier's GPU composite.

## Retained limitations and their owners

| Limitation | Owner |
|---|---|
| Groom scene-shadow casting and receiving (lost in a merge) | #1323, reopened 2026-09-29 (first filed as #1523, closed as its duplicate) |
| Terrain, voxel and foliage casters under VSM | #1524 |
| Virtual geometry in VSM local-light layers; persistent multi-view cull | #1143, reopened 2026-09-29 (an accidental keyword in PR #1167 had closed it) |
| Skinned virtual geometry in the TLAS; emissive virtual meshes as NEE lights | #1525 |
| Sub-scale benchmark readback, resize VUID rerun, split AOVs, staging timing baselines | #1526 |
| Textured ray-hit shading; rough-specular ray sampling | #1355, #1356 |
| Unconsumed raster work under the GPU path tracer | #1346 |
| Aggregate GPU memory telemetry | #1342 |
| Vulkan support contract | #1358 |
| Virtual-geometry tier 3 (WPO, tessellation, spline, programmable raster); hardware-raster rejects | #1152–#1155, #1049 |
| Gaussian splats as a scene entity | #1046 |
| Full bindless material selection on Vulkan | #805 |

## Roadmaps

- **#1359** is the single index for the September 2026 review; this reconcile adds no umbrella.
- **#654** owns the virtual-geometry gap list. Its body's status table is stale (#1144 and
  #1149–#1151 are delivered; "no mesh shaders" predates #813). #1143 was reopened on 2026-09-29.
- **#979** closed on 2026-09-22 with every phase delivered. It stays as the design record of the
  quality tiers; restricted ReSTIR PT and the reference modes are not production tiers (#1334
  support matrix). Its follow-ups are #1346, #1355 and #1356.
- **#812** wraps one open child, #805, and its two recorded seams are resolved or recorded
  elsewhere (`VulkanVertexBuffer`: #1171/#1351; `SetScissorBox`: ADR 0011 amendment (85)). It is
  the index that no longer earned its row, and was closed on 2026-09-29.

## The September 26 amendment, reconciled

| PR | Planned residual | State at `7c5aa1b98` |
|---|---|---|
| #1482 | GTAO (#1463) and forward foliage AO (#1474) | Merged; both closed. `SceneRenderPass.h` names `FoliagePrepassPass`. |
| #1489 | Coat bake (#1445), only part of #1428 | Merged; #1428 later closed by PR #1514. |
| #1480 | The two MCP gaps; FSR2 GL-only | Merged; FSR2 is still GL-only (`FSR2PolicyTest`); #607 stays the gap log. |
| #1485 | Leaves #1478, #1479, #1483 open | All three closed: PR #1505 (#1478, #1479), PR #1517 (#1483). |
| #1477 | Post-merge nightly evidence; #1484 | Merged; #1473 and #1484 remain open. |

## Found on the way

- Fixed here: the RT shadow pass's `MaskedOccludersShadowedAsSolid` stat was never published and its
  premise was false; ReSTIR GI and DI degraded masked geometry without a counter when textures were
  unavailable; the Post Process panel sent users to #805 for untextured reflection hits; the
  `olo_shader_reload` description repeated the stale crash caveat; a CMake comment pointed at a guide
  that was never written; seven dead anchors.
- **Unverified:** the Debug compile-failure path was corrected from #568 and its test, not re-run
  live; the resize VUID (#1526) needs a Vulkan rerun.
