# A groom in the scene's shadows: borrowing the geometry, and the double-count boundary (#1323)

Read before touching `GroomRenderPass::BeginFrame`/`AcquireShadowCaster`, `Groom/GroomShadowWidening.h`,
the groom half of `Renderer/Passes/ShadowRenderPass.cpp`, `GroomStrandDepth.glsl`, `VSM_GroomDepth.glsl`,
`include/GroomShadowWidening.glsl`, or `oloGroomSceneShadow` in `GroomStrand.glsl`.

## The rules

1. **A caster family whose geometry is built inside a pass that runs AFTER `ShadowRenderPass` must
   borrow it through that pass, at the top of the shadow pass.** The other families submit during
   Scene's traversal because their VAOs exist; a groom's is built from cooked curves by
   `GroomRenderPass`'s cache, and that pass runs long after the shadow map. `ShadowRenderPass::
   CollectGroomCasters` calls `GroomRenderPass::AcquireShadowCaster` on the render thread. #1380 lifted
   the whole cache into a third object instead; the re-land (#1523) kept the cache where #1426-#1514 had
   grown it, because moving 1,400 lines to save one accessor would have collided with every later
   groom PR.

2. **One frame, two consumers: `BeginFrame` owns the boundary.** `RenderPipeline` calls
   `GroomRenderPass::BeginFrame` right after `SetRequests`; `AcquireShadowCaster` and `Execute` call it
   themselves when nobody has, and `Execute` consumes it. A tick advanced inside `Execute` hands the
   shadow pass last frame's tick, so a bound coat's frame buffer is skipped as "already uploaded" and
   casts from the previous pose, and `Execute`'s stats reset wipes the caster tallies the shadow pass
   wrote. Eviction stays at the end of `Execute`: both consumers have drawn by then.

3. **A GPU-deformed coat uploads once per frame, whoever asks first.** `CacheEntry::DeformUploadedTick`
   short-circuits the second acquire. The CPU reference path (`GroomGpuDeformation` off) still rebuilds
   per consumer, deliberately: its drawn pose for the coat bake lives in one shared scratch vector, and a
   skipped rebuild would hand the bake another groom's pose.

4. **The depth shaders deform a bound coat, or it casts garbage.** Since #1427 a bound coat's stream
   is a REST stream in each root's bind frame. Both depth shaders include `GroomStrandDeform.glsl` and
   run `GroomStrand.glsl`'s mode-1 branch on the same `DeformModes`/`DeformBases` lanes (now in
   `GroomShadowParamsUBO`, 128 B), with the entity's frame buffer bound at `SSBO_GROOM_DEFORMATION`
   before every caster draw, or the zeroed placeholder at mode 0.

5. **A GPU-deformed coat's stream bounds describe no pose; bound it from its posed roots.**
   `GroomRenderPass::PosedObjectBounds` takes the valid `RootTransforms` origins and pads them by the
   rest stream's `MaxReach` (the farthest point from its own root, measured once when the stream is
   built), times three when simulated (a pinned, length-preserving guide can swing a point twice its
   length), plus the widest radius. The cull box and the receiver offset both use it.

6. **The one-texel width floor is the casting mechanism, and its number is measured.** A 70 µm hair
   against a 20 m cascade at 2048 texels is 0.0036 texels of half width; rasterised honestly it casts
   nothing. `GroomShadowWidening.AHairIsFarBelowACascadeTexelSoTheFloorIsWhatMakesItCast` pins it.

7. **No compensating alpha on the shadow side; each view casts the share its floor allows, group by
   group.** A depth target has no alpha, and a hashed discard would be a stochastic technique with
   nothing to converge it (`groom-strand-visibility.md` rule 6). A widened strand over-occludes by the
   widening factor, so a view casts a prefix of each RUN of `GroomCasterOrder` -- one run per groom
   group, each in its own hashed order -- as `DecideGroomCasterRun` allows: 2 / factor of the run's
   strands (at least 1/16), and never fewer than four of the run's ESTIMATED layers. The estimate is a
   run-wide mean, not a per-texel count: the projected length is the moment lower bound across the
   light (the ribbon has no end caps, so a strand along the light lays none), over the run's box.
   A coat-wide share thinned a sparse plume to a dense patch's share, thick guard hairs to the thin
   majority's, and light-aligned strands as if they lay flat; `GroomCasterCoverageTest` measures each
   against a raster model, with the coat-wide rule as the negative control. Strided subsets alias with
   periodic cooks; a subset never adds shadow (#1533).

8. **A strand samples the OPAQUE copies at itself; without a copy its receiver is the coat's
   LIGHT-EXIT POINT, gated on CASTING, not on the density volume.** A caster's strands are in the map,
   so a strand sampling the full map at its own position is occluded by its own coat on top of the
   volume's charge. Tied to the volume, a coat without one fell from **44.98 mean luma to 0.22** (#1380,
   measured): a shadow map is binary and a coat is not. The exit point cures that, but it lies outside
   the coat's box, so nothing inside the box -- the body included -- could shadow the fur. So the
   cascades and the local-light atlas, when a groom casts, render their opaque casters, are copied into
   `ShadowMap`'s opaque copies, and then take their grooms, uncleared (#1533). `GroomRenderPass` binds
   the copies over the four shadow slots for its own draws and invalidates them after. `CoatModes.y` is
   a bitfield (1 receives, 2 opaque cascades, 4 opaque atlas); a set bit samples that map at the strand:
   the body shadows its fur, and fur is counted only by the volume. A copy cannot hold another groom's
   fur, so one coat does not shadow a second. The VSM keeps the exit point: its cached pages hold the
   fur. A copy covers only the receiving coats' texels (their posed box projected through each view,
   padded 96 texels for the kernels): at the dog's 4096² cascades the whole-layer copy cost ~0.3 ms.
   Against the atlas a strand at itself offsets 1 cm toward the light with a 1e-5 depth bias, because
   the surfaces' constant 0.005 spans metres of a perspective map's non-linear depth. `GroomsShadowedByOpaqueCascades` / `...Atlas` say which coats got the opaque lookup, and
   `OLO_FAULT_GROOM_SHADOW_AT_COAT_EXIT` brings the exit point back for a negative control. `known`,
   the gate on forwarded dual scattering, is true only where the lookup ran at the strand: an exit-point
   answer says nothing about the body. `CoatModes.z` gates the offset, `.x` the march, `.y` the
   receive; a whole-vector assign to `u_GroomCoatModes` in the coat block clears the other two. The exit
   distance stays in world metres because the direction is not normalised.

9. **A strand has no surface normal, so the receiver bias is spent along `L`.** A ribbon's
   `v_ViewNormal` faces the camera. A zero vector is not an option: the CSM helper normalises it.

10. **Wire every technique, count draws per technique, and say what is not wired.** Grooms reach the
    CSM cascades, the local-light atlas and the VSM clip levels (the `ExternalCasterRenderer` seam), with
    `CascadeDraws`, `AtlasDraws` and `VirtualShadowLevelDraws` counted separately. They do **not** reach
    the VSM's local-light LAYER pool, which GPU-driven mesh batches fill — virtual geometry has the same
    limit. With VSM `LocalLights` on, lamps cast no coat shadow; that is counted
    (`VirtualShadowLocalLightsWithoutGrooms`), warned once, and shown in the inspector.

11. **A moving coat invalidates its VSM pages.** #1380 never did, so a walking coat left its old
    silhouette in every cached page it crossed. `SubmitGroomDynamicInvalidations` runs before
    `UpdatePages`: a bound coat is a mover every frame, an unbound one when its transform changes; the
    swept box joins last frame's footprint; a departed caster's last footprint is invalidated; the boxes
    go in RENDER-RELATIVE.

12. **Grooms are item-safe in the parallel cascade region; acquiring is not.** The draws write only
    `ItemResources::Groom`, but acquiring can create buffers and upload the frame buffer, which amendment
    (92) rule 7 refuses on an item context. Silent on OpenGL, a fault on Vulkan.

13. **The caster's width is the drawn width.** Since #1428 the per-role compensation is in the
    stream's radii, so the only lever left is `request.WidthScale`, which both draws apply.

## Things that will bite

- **`length()`, never the signed element, when deriving a scale from a projection.** Vulkan's
  `[1][1]` is negative (`TheScaleTakesTheMagnitudeSoAVulkanYFlipDoesNotInvertIt`).
- **The VSM route re-binds the physical pool image after binding its own program.** In a scene whose
  only casters are grooms the mesh raster returns before binding anything, and every `imageAtomicMin`
  is discarded.
- **Binding 7 is shared by three blocks:** `GroomStrandParamsUBO` (strand pass) and
  `GroomShadowParamsUBO` (128 B) per cascade item and for the VSM route. Each occupant rebinds and
  refills it before its own draws.
- **The ray-traced shadow guard is a count, not an absence.**
  `GroomRayTracingProxy.TheCoatShaderReadsNoRayTracedShadowTermSoAProxyCannotShadowItsOwnCoat` pins
  one `oloGroomSceneShadow`, one light-exit offset and one lookup per technique, and forbids every RT
  mask token: a coat's TLAS proxy (#1253) would shadow its own coat twice.
- **The evidence masks are derived, not typed.** `GroomSceneShadowVisualEvidenceTest` gets the coat's
  mask by toggling `GroomComponent::m_RenderStrands`.

## Where the evidence lives

- The floor: `OloEngine/tests/Groom/GroomShadowWideningTest.cpp`.
- The pixels: `GroomSceneShadowVisualEvidenceTest.cpp`, writing
  `OloEditor/assets/tests/visual/GroomSceneShadow[Off]_GL_<Path>[_<Case>].png`.
- The save game: `OloEngine/tests/SaveGame/GroomSceneShadowSaveLoadTest.cpp`.
- The double-count split: [groom-coat-self-shadowing.md](groom-coat-self-shadowing.md) rule 1.
- Why a family does not reach a technique by itself:
  [virtual-geometry-into-a-second-shadow-technique.md](virtual-geometry-into-a-second-shadow-technique.md).

## History

#1380 landed this; the branch-side merge of #1382 restored the pre-#1380 files and deleted all of it
while its PR body said otherwise (#1523). Re-landed by #1523 on the #1426-#1514 groom pass.
