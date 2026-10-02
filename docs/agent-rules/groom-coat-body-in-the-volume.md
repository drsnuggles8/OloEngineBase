# The body inside a coat's density volume

Rules for the body a coat grows on, marked into that coat's self-shadow volume (#1533). Read before
touching `GroomCoatShadow::MarkBodyInDensityVolume`, the march in
`include/GroomCoatShadowCommon.glsl`, or the light loop and environment term of `GroomStrand.glsl`.

## The rules

1. **The body is NEGATIVE density in the coat's own texture, and never beside a coat voxel.** A
   marked voxel holds `-OpacityPerVoxel / voxelSize` (an extinction in 1/m; one voxel passes
   `exp(-6)`). It is eroded `ErodeVoxels` (2) from the skin and then cleared from every voxel within
   one voxel (26-neighbourhood) of any positive density, so no trilinear cell mixes body and coat.
   That is what makes a march that skips samples at or below zero -- the march every light whose map
   answered at the strand takes -- bit-identical with the body in the volume
   (`AMarchThatDoesNotCountTheBodyIsBitIdentical`). A second texture was not an option: the sampler
   namespace has one index left.

2. **The body is counted exactly where no map answered for it at the strand.** `countBody =
   !occlusionKnown && holdsBody`: a light that does not cast, the VSM (which answers at the coat's
   exit point), a map with no opaque copy, and the sky (both environment marches). `exp(-bodyTau)`
   multiplies the light arriving at the strand, as the scene's shadow does, and forwarded dual
   scattering then runs for that light too, because the body that made forwarding light through the
   animal wrong is now in the answer. Counting it for a light whose opaque copy already answered
   would shadow the fur by the body twice.

3. **Only a rest bake carries a body.** `CoatModes.w` is a bitfield: 1 the volume is the coat at rest
   (marched from each fragment's bind point along the light turned back through its root), 2 it holds
   the body. The body is the bind-pose surface (`GpuRootInputs.Surface` through `SurfaceToGroom`), so
   near a strand it is the body as posed and across the animal it is the body as bound: a leg lifted
   in a clip shades the belly's fur from where the leg rests. A pose bake has no body (it would need the
   surface skinned at every rebake) and shades as before #1533.

4. **Only closed parts are filled, and an open one says so.** The surface is welded by position
   (glTF splits every UV seam) and split into components; a component is filled only if every edge has
   an even number of faces. Parity runs down each z column of voxel centres in exact fixed point with a
   top-left rule, so a centre on an edge or vertex counts once, and a column that ends odd is left
   empty and counted. A surface with no closed part logs a warning and the coat keeps no body. The dog:
   the skin, nose, lips, gums and pads weld into one closed part of 224,668 triangles; the eyelid shells
   and teeth are open and skipped; the tongue is closed but thinner than the erosion.

5. **What the erosion removes stays transparent to the body.** Ears, the tail's tip and anything else
   thinner than about `2 * ErodeVoxels + 1` voxels has no core left: light still reaches their fur
   through them, attenuated by the coat alone. That is close to right for an ear and wrong for nothing
   else on the dog.

## What the volume misses, measured

Rays from 671 furred dog vertices (61 per part) toward the key, cast against the skin mesh and through the volume
(`OLO_DOG_BODY_SLICES=1`, 4.65 mm voxels): the volume stopped **none** the mesh let through, in every
one of eleven parts. It stopped 227 of the 367 the mesh did. Of the 140 it missed, 55 were the ears
and 60 the legs and tail, and at most one per part had a body voxel anywhere on the ray. So a miss is
a ray through the eroded outer shell (a grazing chord) or a part with no core, never a march that
stepped over the body. `TheVolumeNeverShadowsWhereTheBodyDoesNotAndMissesOnlyItsShell` pins both
halves on an analytic sphere: no false shadow, and no miss deeper than `ErodeVoxels + 3` voxels.

The light-by-region record's rim leak is not this measurement and holds no bound: against the
cascades it also counts every opaque caster the volume never holds (the lawn, the eyes, the teeth).
In the rear view it kept 0.135 of the rim's light the cascades stop, against 0.136 without the body.

## Things that will bite

- **A casting light answers for the body only through the strand's 1 mm receiver offset.** The
  shadow map stores a closed body by its far side, and the surfaces' receiver bias lit the fur on that
  side through the body ([groom-into-the-shadow-techniques.md](groom-into-the-shadow-techniques.md)
  rule 8). The volume is not the fix for that: it counts the body only where no map answered.

- **The half-float pack checks the largest MAGNITUDE.** The body is negative; a voxel under 0.1 mm
  would push `-6 / voxel` past 65504, so the RGBA32F fallback keys on `max |density|`.
- **A rest bake made before the bound surface arrived is made again once it has**
  (`CoatBakedWithSurface`), or a coat that bakes on its first frame would never get its body.
- **The lever is shader-side.** `OLO_GROOM_NO_COAT_BODY` clears bit 2 per frame; the volume keeps the
  body, so the A/B costs no rebake and changes nothing else.

## Where the evidence lives

- `GroomCoatBodyPropertyTests.cpp` (L1): the furred ball -- coat voxels untouched, nothing beside
  them, the casting march bit-identical, a ray into the body stopped and one out of it or along the
  skin not, open shells and thin parts and corrupt input refused with the volume untouched, and 4000
  coat rays against the analytic sphere (the shell contract above).
- `DogShowcaseEvidenceTest.TheLightTheBodyCannotStopIsMeasuredRegionByRegion` and
  `assets/tests/visual/Dog_Lighting.txt`: every arm with and without the body, against the cascades.
