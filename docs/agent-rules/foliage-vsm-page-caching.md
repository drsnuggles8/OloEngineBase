# Foliage in the virtual shadow map: invalidate only the levels that can see the motion

**Rule.** Moving foliage invalidates a VSM clip level only where that level's texel is finer than the
layer's *declared* maximum displacement, and only where the motion is: the wind bound over the whole
footprint, wind plus the interaction bound around each influence. Coarser levels keep their cached
foliage pages, are refreshed one level every `kCoarseRefreshPeriodFrames` frames in rotation, and the
foliage clip-level cull rejects every group over no dirty page so a cached level draws nothing.
Code: `Renderer/Shadow/FoliageShadowInvalidation.h` (policy and error bound),
`FoliageRenderer::CollectVsmInvalidations`, `ShadowRenderPass::SubmitFamilyDynamicInvalidations`,
`compute/FoliageGroupCullVsm.comp`. Issue #1580.

## Why

Before #1580 the foliage footprint went to the VSM as *deforming* every frame, so every page it touched
in all sixteen clip levels was cleared and redrawn every frame, and each level re-culled and redrew
every layer: 19–103 ms of VSM foliage casters at 1080p against 4–10 ms with cascades
(`docs/analysis/foliage-cost-baseline-1391.md`). The page cache, which is what makes VSM cheap, never
applied to foliage.

## The error bound, and where each number comes from

* **Wind:** `FoliageWindMaximumDisplacement(layer)`, the same pad the cull's instance bounds carry.
* **Interaction:** `FoliageInteractionMaximumDisplacement(response)`, the clamp the shader applies to
  the summed push. Its contribution is exactly zero outside an influence's cylinder, so it widens only
  the influence boxes (`FoliageShadowInvalidation::InfluenceBounds`), never the footprint.
* **Texel:** `VSM::ClipProjection::TexelWorldSize` (level *L* is `2^(L-10)` m at the default
  `Clip0HalfExtent` of 2 m).

A cached level's texels lag the live plants by at most the peak-to-peak motion, `2 × displacement`,
which is at most two of that level's texels, and a receiver at distance *d* selects the level whose
texel is `d/2048..d/1024`, so a cached level is sampled only far away or as the one-frame fallback.

## What the displacement bound does not cover

A density-LOD fade, a mesh/card hand-over and a card turning toward the eye change the silhouette
without displacement. They are covered by (1) the coarse-level rotation, and (2) invalidating, when the
main eye moved, the levels a receiver within twice the layer's view distance selects plus the fallback
level. A regeneration, an edited layer or a moved terrain changes `FoliageRenderer::GetShadowRevision`
and invalidates every level through `ShadowFamilyCache`, as for every other family. **A new layer
parameter that the shadow draw or its cull reads belongs in `GetShadowRevision`**, or it can stay
stale in the coarse levels for one rotation.

## Checking it

`FoliageShadowInvalidationContractTest` pins the level selection, the rotation, the influence box and
the level lane through the invalidation queue. `FoliageVsmCacheVisualEvidenceTest` A/Bs the shipped
policy against per-frame invalidation (`FoliageShadowInvalidation::Mode::EveryFrame`) under moving wind,
with a never-invalidate negative control that must differ.

## What it bought on the committed scenes, and why the policy alone bought nothing

Measured at #1580 (RTX 4090, Release). With the default `Clip0HalfExtent` of 2 m a level's texel is
`2^(L-10)` m, and the traversal layers declare 0.10–0.84 m of wind, so the policy may cache only levels
10–15 (texel at least 1 m). Resident pages exist only where receivers request them, and receivers within
about a kilometre request levels 9 and below, so on these scenes every resident foliage page is in a level
the policy must invalidate: `FoliageVsmCacheVisualEvidenceTest` counts the same pages redrawn per frame
under the policy as under per-frame invalidation (1533 against 1522), and `FoliageCostBaselineTest`'s
same-binary A/B (`OLO_FOLIAGE_COST_VSM_POLICY=everyframe`) moves no cell outside noise.

What does save work is the clip-level cull's dirty-page test: a level culls only the groups over pages it
is redrawing, so the coarse levels, which have almost no resident pages, draw no foliage at all. Live on
Vulkan (traversal Step4 framing, 941x628 viewport) the VSM frame went from 39.0 to 15.1 ms
(shadow pass 35.5 to 11.5 ms); from an elevated framing 18.9 to 9.6 ms. The OpenGL headless baseline cells
did not show that drop; that discrepancy is open.

## The card cut-over: cards in every level a receiver beyond the mesh band samples

**Rule.** In a clip level past `SelectClipLevel(R)`, where R is how far from the eye the LAYER still
draws its authored mesh (`FoliageRenderer::GetLayerShadowMeshReach`), that layer casts through cards
(`FoliageShadowInvalidation::FirstCardLevel`, decided per layer in the cull and in `RenderShadows` from
the view's `CardCutover`). A receiver samples the level `SelectClipLevel(d)` gives, which only grows
with d, so such a level is read only beyond the layer's mesh band, where the lit plant already is its
card. Per layer, because one pine stand meshed to 87 m would otherwise hold a meadow that hands over
at 12 m to the pines' level. Every layer's level is part of the foliage's shadow revision
(`GetVsmCardCutoverKey`), so a change invalidates every level. `OLO_FOLIAGE_VSM_CARD_LEVEL` overrides it
for all layers (16: meshes everywhere; 0: cards everywhere).

**Why there and not finer.** The per-level brackets (`ShadowPass/VsmSun/FoliageCasters`, one per level,
in level order) show where the cost is: levels 2–6 on the traversal (texel 4 mm to 6 cm, all finer than
the wind bound, so redrawn every frame), levels 9–15 near zero. A card costs about a third of a mesh in a
level. GL, 1080p, Release, VSM foliage casters per frame:

| cut-over level | Step0 | Step4 | Step8 |
|---|---:|---:|---:|
| 16 (meshes everywhere) | 12.9 ms | 22.1 ms | 8.4 ms |
| 6 | 11.1 | 17.7 | 6.8 |
| 5 | 10.1 | 15.8 | 5.8 |
| 4 | 8.2 | 13.9 | 4.9 |
| 3 | 6.2 | 11.7 | 4.5 |

Below the mesh-band level the saving grows, but a receiver beside a mesh plant would then read a card
shadow: the shadow and the plant would disagree, the #1233 rule. With one reach for all layers the
farthest (pines meshed to 87 m) held every layer to L7 and saved 3–17 %; per layer (meadow grass, leaf
litter and dune grass L5, wildflowers L6, shrubs and pines L7) it saves 11–30 %: Forward 13.0/21.4/8.4 to
10.7/16.0/7.1 ms, Deferred 17.9/16.8/8.5 to 15.9/11.9/7.0 ms at Step0/4/8.
`FoliageCostBaselineTest.VsmCardCutoverKeepsTheShadowOfTheMeshBand` measures 0–5 moved pixels of shadow
factor on Deferred at every pose and on every path at Step0 and Step4 (a repeat of one arm moves 0–4),
against 437–630,000 for cards everywhere.

**What it does not cover.** A long shadow at a low sun, cast by a mesh plant inside R onto a receiver
beyond it, comes from the card. A receiver whose page is not resident walks up to two levels coarser
(`vsmShadowFactor`), and two coarser can be a card level: on Forward and Forward+ the densest foreground
grass at the woodland pose does, 44 of 2,073,600 pixels by at most 31/255 (Deferred marks those pages and
shows none). Covering the walk would have pushed most layers back to L7; the user accepted the 44 pixels
for the saving (#1580). Local-light layers keep the meshes: a lamp's layer is not a clip level.
