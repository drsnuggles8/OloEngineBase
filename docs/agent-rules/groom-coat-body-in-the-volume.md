# The body inside a coat's density volume

Rules for the body a coat grows on, marked into that coat's self-shadow volume (#1533). Read before
touching `GroomCoatShadow::MarkBodyInDensityVolume`, the march in
`include/GroomCoatShadowCommon.glsl`, or the light loop and environment term of `GroomStrand.glsl`.

## The rules

1. **The body has a texture of its own, on the coat's grid: `TEX_GROOM_COAT_BODY` (77), RGBA8.** A
   is the body's box-filtered occupancy, RGB the sky it leaves (rule 6). It is a separate array
   (`DensityVolume::Body`) and texture because the coat's texture cannot hold it at the skin:
   trilinear filtering mixes a shared cell's corners, so the body used to ride there as negative
   density, eroded two voxels from the skin and cleared a voxel from every coat voxel. That took about
   a centimetre off every part, and the ears, legs and tail tip had no core. A march that does not
   count the body never reads the texture, so every casting light's march is the coat's alone
   (`AMarchThatDoesNotCountTheBodyIsBitIdentical`).

2. **Occupancy is box-filtered, and the floor cuts the smear outside the skin.** Parity runs down
   4 x 4 sub-columns per voxel column with exact depths, so a voxel holds the share of it the closed
   parts fill and a part thinner than a voxel keeps its share. Filtered trilinearly, that share falls
   from 0.5 at the skin to 0 about 1.5 voxels out; the extinction is
   `kBodyOpacityPerVoxel / voxel * max(o - kBodyOccupancyFloor, 0) / (1 - kBodyOccupancyFloor)`
   (6 and 0.1), which is zero about 0.65 voxels out. Fur that far from the skin, lit on its outward
   side, sees no body. The constants are mirrored in GLSL (`OLO_GROOM_BODY_*`).

3. **The body is counted exactly where no map answered for it at the strand.** `countBody =
   !occlusionKnown && holdsBody`: a light that does not cast, the VSM (which answers at the coat's
   exit point) and a map with no opaque copy. `exp(-bodyTau)` multiplies the light arriving at the
   strand, as the scene's shadow does, and forwarded dual scattering then runs for that light too.
   Counting it for a light whose opaque copy already answered would shadow the fur by the body twice.

4. **Only a rest bake carries a body.** `CoatModes.w` is a bitfield: 1 the volume is the coat at rest
   (marched from each fragment's bind point along the light turned back through its root), 2 it holds
   the body. The body is the bind-pose surface (`GpuRootInputs.Surface` through `SurfaceToGroom`), so
   near a strand it is the body as posed and across the animal it is the body as bound. A pose bake has
   no body and shades as before #1533.

5. **Only closed parts are filled, and an open one says so.** The surface is welded by position
   (glTF splits every UV seam) and split into components; a component is filled only if every edge has
   an even number of faces. Parity uses exact fixed point with a top-left rule, so a sub-column centre
   on an edge or vertex counts once, and a sub-column that ends odd is left empty and counted. A
   surface with no closed part logs a warning and the coat keeps no body. The dog: the skin, nose,
   lips, gums and pads weld into one closed part of 224,668 triangles; the eyelid shells and teeth are
   open and skipped.

6. **The sky the body hides is a share of the lobe, not of one ray.** The environment term samples
   along the fibre's eye-facing normal, and that ray hardly ever crosses the body: the fur a viewer sees
   is on its near side. Yet at the silhouette the body fills half the lobe, and behind the fibre most
   of it. So the bake marches 32 rays through the occupancy from every fourth voxel, keeps the share
   that gets out and its mean open direction (RGB, `0.5 + 0.5 v`), and each voxel takes them from the
   nearby cells outside the body (a cell inside sees nothing, and no fur grows there).
   `BodySkyVisibility` reads it as a cap of that share around that direction, so a cosine lobe sees
   `v + 2 v (1 - v) cos`. The two environment marches count the coat only and multiply by this.

## What the volume misses, measured

Rays from 671 furred dog vertices (61 per part, 3 mm off the skin) toward the rim, cast against the
bind-pose skin mesh and through the volume at the shipped step (`OLO_DOG_BODY_SLICES=1`, 4.65 mm
voxels). The volume stops **all 367** rays the mesh stops, in every one of eleven parts; the eroded
body it replaced missed 140 of them (55 in the ears, 60 in the legs and tail). It also stops 24 of the
304 the mesh lets through, and every one of those passes within 2.1 mm (0.45 voxels) of the skin: the
band rule 2 leaves, where a box filter at this voxel size cannot tell a grazing ray from one that dips
in. `TheVolumeShadowsNoFurtherThanTheFloorAndMissesNoRayIntoTheBody` pins both edges on an analytic
sphere: no body on any ray a voxel or more outside it, and every ray that reaches a quarter of a voxel
in stopped, at the shipped step and at one voxel.

From the fur itself (1,100 cooked strands, points a third, two thirds and all of the way along): the
volume misses 11 of the 1,721 points the mesh shades (9 on the neck) and shades 147 of the 1,631 it
lights, every one within 3.8 mm of the skin, where two surfaces' bands add up in a crease.

The light-by-region record (`Dog_Lighting.txt`) holds no bound. Against the cascades, the volume
still lets through 0.12 of the rim light on the rear view's coat and up to 0.2 on the legs. That
excess is not the body's, because the probe shows the volume shades at least what the skin does
there. The record reads the same in the bind pose (`OLO_DOG_LIGHTING_CLIP=Rest`), so it is not the
rest bake's pose either. What it can count is every opaque caster the volume never holds (the eyes,
the teeth) and the cascades' own filtering on parts a few centimetres across; neither is measured
on its own.

## Things that will bite

- **A casting light answers for the body only through the strand's 1 mm receiver offset.** The
  shadow map stores a closed body by its far side, and the surfaces' receiver bias lit the fur on that
  side through the body ([groom-into-the-shadow-techniques.md](groom-into-the-shadow-techniques.md)
  rule 8). The volume is not the fix for that: it counts the body only where no map answered.
- **The body texture's placeholder is not zero.** A zero texel decodes as a sky shut on every side;
  the placeholder is `(128, 255, 128, 0)`, no body and the sky open.
- **A rest bake made before the bound surface arrived is made again once it has**
  (`CoatBakedWithSurface`), or a coat that bakes on its first frame would never get its body.
- **The lever is shader-side.** `OLO_GROOM_NO_COAT_BODY` clears bit 2 per frame; the volume keeps the
  body, so the A/B costs no rebake and changes nothing else.

## Where the evidence lives

- `GroomCoatBodyPropertyTests.cpp` (L1): the furred ball -- the coat's arrays untouched, the
  occupancy adding up to the ball, no body a voxel outside it and every ray a quarter voxel into it
  stopped, thin slabs keeping their share, the sky's share against the ball's geometry, open shells
  and corrupt input refused with the volume untouched.
- `DogShowcaseEvidenceTest.TheBodyInTheCoatVolumeIsSlicedForLooking` (`OLO_DOG_BODY_SLICES=1`): the
  rim probe above, and slices to look at.
- `DogShowcaseEvidenceTest.TheLightTheBodyCannotStopIsMeasuredRegionByRegion` and
  `assets/tests/visual/Dog_Lighting.txt`: every arm with and without the body, against the cascades.
