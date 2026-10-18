# Grooms in the VSM's local-light layers: one draw per reached layer, share and width at one texel (#1592)

Read before touching `ShadowRenderPass::RenderGroomVirtualShadowLocalLayers`, the local domain of
`include/VSM_GroomDepthVertex.glsl`, `VirtualShadowMap::BoundsReachLocalLayer` /
`LocalLayerReceiverResolution`, or any local raster's `vsmLocalRasterSubRect` call.

## The rules

1. **A groom reaches a lamp's layers through the VSM's local external seam, one draw per layer its
   box reaches.** `RenderCasters`' second `ExternalCasterRenderer` runs with the local viewport bound;
   grooms draw there before the terrain/voxel/foliage families. The reach test is
   `VirtualShadowMap::BoundsReachLocalLayer`, the C++ twin of `VSM_CullLocalCasters.comp`'s: a layer
   serving no light, a box beyond the light's range or wholly beyond one frustum plane is rejected. A
   false negative is a lamp that drops the coat with nothing to report it.

2. **The CPU cannot read the raster mip, so the share is cut for the receivers' texel, and the
   shader floors the width at the coarser of that texel and the raster mip's.** A layer is rasterised
   at the finest mip with a dirty page (`b_LocalRasterMip`), which can be finer than the mip the
   coat's receivers sample. `LocalLayerReceiverResolution` picks the mip the marker selects at the
   coat's centre (`SelectLocalMip`, with `GetLocalDetailBias()` -- the clamped bias the GPU reads, not
   the raw setting), and `DecideGroomCasterRun` cuts the share for it. The shader floors at
   `min(raster texels, Width.z)`: a share is a promise about world coverage, so many strands each so
   wide, and floored at a finer raster texel it drew strands too thin for what it was cut for.

3. **Every local raster places its mip through `vsmLocalRasterSubRect`.** A layer redrawn at mip m
   lands in the `(2048 >> m)²` corner of the mip-0 viewport; a raster with its own copy of the map
   can write a caster into another mip's corner. Meshes, skinned meshes, the families and grooms all
   call the one helper (`VirtualShadowMapLocal.EveryLocalRasterPlacesItsMipThroughOneSubRectMap`).

4. **A moving coat needs no invalidation of its own for the layers.** `AddDynamicInvalidation`
   already sweeps every local layer (`UpdatePages` step 2b), so `SubmitGroomDynamicInvalidations`
   covers lamps too; `ALampTheVsmServesFollowsAMovingCoat` pins it.

5. **The page marker reads only the rendered region of the depth target.** Under a render scale the
   depth target keeps its display size and the frame fills its `[0, render)` corner;
   `VirtualShadowMapMarkPass` clamps its grid to the pass's `m_RenderViewportWidth/Height`. Sized by
   the texture, it unprojected every texel to the wrong world point: a lamp's coat shadow came out cut
   along page edges at a 0.67 scale (overlap with the atlas 0.47, 0.985 after).

6. **A strand's light-exit lookup into a lamp's layers never passes the lamp.** An exit at or past
   the lamp moves to 0.11 m short of it; one before the lamp stays put, since pulling it back would
   put the lookup inside the coat again (CodeRabbit on #1637). The layers
   now hold the coat, and with the lamp inside the coat's box the light leaves the box past the
   lamp; a lookup there read the far side's face through the coat's own fur (821 of 24 886 coat
   pixels darkened, 0 after). `oloGroomLocalExitPoint` in `GroomStrand.glsl` clamps it, past the
   layers' near plane (`LocalNearPlane` caps it at 0.1 m). Place such a test's lamp by the asset's
   bounds, not by eye: a coat's strands droop, and a lamp first put "inside" sat above the box.

## Things that will bite

- **Read stats with the camera where the lamps are.** `olo_screenshot`'s one-shot `camera` restores
  the previous pose afterwards; stats read next describe that pose. Use `olo_camera_set_pose`.
- **Freeze animated bodies before a live A/B** (`AnimationStateComponent.PlaybackSpeed` 0): an idle
  head turns between two captures and the diff measures the turn.
- **`Acceptance.olo`'s head hair and horse coats carry no `GroomSceneShadowComponent`**, so they cast
  no scene shadow at all; only the fox coats do. The live check opted them in on a scratch copy.

## Where the evidence lives

- Pixels: `GroomSceneShadowVisualEvidenceTest`'s lamp cases, `GroomSceneShadow[Off]_GL_<Path>[_<Variant>]_{Vsm,Atlas}{Point,Spot}.png`,
  the negative control `..._Vsm<Lamp>NoLocalLayers.png` (`OLO_FAULT_SKIP_GROOM_VSM_LOCAL_LAYERS`).
- Contracts: `VirtualShadowMapLocalTest`'s groom cases.
