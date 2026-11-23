# Renderer state: configuration or scene publication

Applies to: adding or moving a `Renderer3D` settings struct, a scene-published render value, or a
pass-owned cache built from the scene's content (issues #1689, #1665;
[ADR 0025](../adr/0025-renderer-state-has-four-owners.md)).

## Rules

1. **A value a scene writes is scene publication and lives in `RendererSceneState`.** One that
   only the editor panels, the MCP tools, quality tiering or the user writes is configuration
   and lives on the view (`RendererViewResident`). If both write it, it goes where the runtime
   needs it, and the table below says why.
2. **`Renderer3D::ResetSceneState` drops every scene publication by assignment**, and each host
   publishes the incoming scene's values after it. `Scene::OnRuntimeStart` and
   `OnSimulationStart` publish their own scene's world settings
   (`SceneTransition::PublishSceneWorldSettings`). The editor keeps its live edit buffer across
   its own swaps (`ResetRendererForSceneSwap`), and `ApplySceneRenderSettings` publishes a newly
   opened scene's settings.
3. **A pass cache built from the scene's content is dropped on the same reset.**
   `ResetSceneState` calls `RenderGraphNode::DropWorldCache` on every view's pipeline. A cache
   that only converges over many frames (DDGI probes) otherwise keeps the old scene's light for
   the first frames of the new one.
4. **The guard tests are `RendererSceneStateContract.*`.** They fail when a `Get*Settings()`
   accessor returns neither a `RendererSceneState` member nor a listed configuration struct,
   when a reset leaves a scene-published struct off its default, or when a pass with a frame
   sequence of its own does not say whether it holds a world cache.

## The classification (#1689)

| Struct | Who writes it | Owner | Why |
|---|---|---|---|
| `RendererSettings` | renderer panel, MCP, quality tiering, benchmark; the scene's `Path` on activation | view configuration | The path is published on activation, but tiering and the user's panel choices go on top of it and must survive a scene reset, a view reset and a restart. |
| `PostProcessSettings` | post-process panel, MCP, tiering, benchmark, probe capture; the scene's stack on activation | view configuration | Per view since #1352. Activation republishes the authored stack and tiering goes on top. Resetting it would drop the tier and the user's debug taps until the next activation. |
| `ShadowSettings` | renderer panel, tiering | device / view configuration | Not scene-authored. |
| `SnowSettings` | scene activation; editor panel and MCP as the edit buffer of the open scene | scene publication | Authored per scene, stored in the `.olo`. |
| `SkinDiffusionSettings` | scene activation; panel, MCP, benchmark | scene publication | Authored per scene since #1689's first half (format 50). |
| `FogSettings` | scene activation; `WeatherSystem` every tick; panel, MCP | scene publication | Authored per scene and driven by a scene component. |
| `WindSettings` | scene activation; `WeatherSystem` every tick; panel | scene publication | As fog. `WindPaused` stays configuration (the editor's pause toggle). |
| `SnowAccumulationSettings` | scene activation; `WeatherSystem` every tick; panel | scene publication | As fog. |
| `SnowEjectaSettings` | scene activation; panel | scene publication | Authored per scene. |
| `PrecipitationSettings` | scene activation; `WeatherSystem` every tick; panel | scene publication | As fog. |
| `CloudscapeRenderState`, `UnderwaterFogState`, the three water fields | the scene every frame | scene publication | Already in `RendererSceneState` (ADR 0025). |
| Culling toggles, camera-relative, selection outline, render scale | panels, MCP | view / process configuration | Not scene-authored. |

In the editor the renderer's copy of a scene-published struct is the edit buffer of the open
scene: the panels write it, and a save or an auto-save captures it back into the scene
(`SceneRenderSettings::CaptureRenderer`). That is why the editor republishes the buffer across
its own swaps (Play, Simulate, Stop) instead of re-reading the stored scene, and why entering Play
copies the buffer into the runtime copy before `OnRuntimeStart` publishes from it.

## World caches (#1665)

| Owner | Cache | Lifetime | Dropped by |
|---|---|---|---|
| `DDGIProbeUpdatePass` | probe irradiance, visibility, radiance and hit atlases, probe data, capture records | per view | `DropWorldCache` → `DestroyResources`; the next volume recreates them cleared |
| `ShadowRenderPass` + `VirtualShadowMap` | VSM pages of the scene's caster depth; the per-family caster sets they are diffed against | per view | `DropWorldCache` → `VirtualShadowMap::InvalidateAllPages` (the full invalidate Init takes) and `ShadowFamilyCache::Clear` |
| `SnowAccumulationSystem` | snow depth field (snowfall, melt, footprints) | process-wide | `SnowAccumulationSystem::Reset`, called from `ResetSceneState` (before #1665 only the test fixture called it) |

Looked at and not world caches: the ReSTIR GI/PT reservoirs, the path tracer sum, FSR2, GTAO and
the volumetric-fog froxels (screen or camera space, invalidated through
`TemporalHistoryRegistry`); the cloud and volumetric shadow maps (rebuilt every frame); the
reflection probe array (an upload cache keyed by environment map, resubmitted every frame); the
shadow atlas allocator (layout, not content); impostor atlases and lightmaps (baked assets); the
water and foliage simulation fields (static `Reset()`s, already called on a run start).

Each pass reports what it holds through `HoldsWorldCache()`.
`DDGIVisualEvidenceTest.ASceneSwitchDropsTheProbeVolume` converges a DDGI scene, resets and checks
that no pass on any view holds one (`Renderer3D::HoldsWorldCache`).
`RendererSceneStateContract.EveryPassWithAFrameSequenceDeclaresItsWorldCache` makes every pass
with a frame sequence of its own say whether it keeps one. The scan cannot see a cache in a pass
that has no frame sequence. Such a pass is a new kind, and it should get a sequence or a
`DropWorldCache` when it is written.
