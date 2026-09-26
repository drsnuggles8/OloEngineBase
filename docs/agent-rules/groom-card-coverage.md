# Groom card coverage across the hand-over (#1428)

Read before touching `GroomLodBuilder`'s card widths, the per-role width compensation or
`GroomCardFibreScales` in `GroomStrandMesh.cpp`, the coat jitter in `GroomCoat.cpp`, the ribbon depth
in `GroomStrand.glsl`, the card-tier bake in `GroomRenderPass`, or
`GroomAnimalsAcceptanceEvidenceTest.TheCoatKeepsItsCoverageFromNearToFar`. The ladder's own rules are
in [groom-representation-lod.md](groom-representation-lod.md).

## The rules

1. **A card is as wide as its members COVER, as the strand shader draws them at the hand-over,
   never as wide as their sum.** Strands that share a clump cell overlap. On the long-coated horse
   the long-hair role's members cover 0.40 of their summed width, and a summed card tier drew 2.6x
   the coat at the hand-over. `GroomCardWidth::Covered` integrates, averaged over the directions
   around the card, the coverage of the members' bands widened to the pixel at `SourcePixelSize`.

2. **A card-tier self-shadow is baked at each GROUP's fibre area.** The volume stores fibre, and a
   card covers a different fraction of its members' fibre in every group: 0.9 of it in a body
   undercoat, a fifth of it in a dense tail. `GroomCardFibreScales` gives every drawn segment its
   group's base-over-level fibre ratio. A single groom-wide factor (2.7 on the long coat, 3.2 on the
   short) baked the body cards at two to three times their fibre and the tail's at two thirds of it.
   **Apply it wherever a pose is formed.** The GPU rest stream builds its pose in two places, and the
   first version scaled only one; the card tier then baked unscaled and read 25% lighter.

3. **A card carries its members' MEAN jitter, not one strand's.** The coat's length, width and shade
   jitter are per-strand draws. A card stands for N strands, so it gets amplitude / sqrt(N), per
   group (`CardTierCoat`). At the full amplitude a whole cluster's length moved together: long cards
   stood out as lone, dense lumps in the shadow volume, and the card tier darkened 15% more than the
   strands.

4. **A ribbon's depth is its fibre's FRONT SURFACE, one radius nearer than its axis.** A card's radius
   is centimetres. Drawn at its axis, the body hid the card's fringe wherever the skin curves towards
   the camera, and the long coat's card tier lost 9% of its coverage over the animal. Only the depth
   moves; the screen position and the velocity stay the axis's. Strand roots also leave a depth tie
   with the skin (+2% on the strand tier).

5. **The strand budget's width compensation is PER ROLE, in the build.** One groom-wide `1 / achieved`
   widened unthinned guard hair 2.2x beside an undercoat at stride 7.

6. **Measure a hand-over in LINEAR quantities, apart, on a frozen pose, from the coat's open side.**
   - Coverage from the entity-ID target, and the coat's own linear luma from the HDR scene colour on
     those pixels, averaged over frames with no temporal resolve. There is no background subtraction:
     a dark coat over a lighter body turns background noise into signal.
   - Coverage, radiance (self-shadow at kappa = 0), shadow (lit over kappa = 0) and energy, reported
     separately. The old 8-bit |coat - body| contrast rose when the coat got DARKER, and hid a 30%
     loss.
   - **kappa = 0, not the component switched off.** Switching the volume off brings back the root ramp
     it replaces, so that arm measures shadow against ramp.
   - Freeze the clips, and keep the Near stop as the identical-strands control, which reads 1.
   - Look at each coat from its own open side. The first fixture put the short coat behind the long
     horse, and its numbers described a sliver of coat.

7. **Separate the representation from the shadow volume before blaming either.** The volume's result
   depends on its resolution and its march step, and differently for cards and strands (#1508). Pin
   both arms to one converged volume (128^3, half-voxel steps) to judge the representation.

## Measured

`TheCoatKeepsItsCoverageFromNearToFar`, GL, frozen pose, ladder on / off. The four columns are the
four quantities of rule 6.

| | coverage | radiance | shadow | energy |
|---|---|---|---|---|
| long coat, 19 m, converged volume | 0.98 | 0.98 | 0.96 | **0.92** |
| long coat, 32 m, converged volume | 0.98 | 1.00 | 0.95 | **0.94** |
| short coat, 19 m, converged volume | 1.13 | 0.88 | 1.07 | **1.07** |
| short coat, 32 m, converged volume | 1.10 | 0.87 | 0.99 | **0.95** |
| long coat, 19 m, shipped shadow LOD | 0.98 | 0.98 | 1.24 | 1.19 |
| long coat, 32 m, shipped shadow LOD | 0.99 | 0.99 | 1.33 | 1.30 |
| long coat, 19 m, Deferred / Forward+ / Deferred MSAA 4 | | | | 1.20 / 1.20 / 1.26 |

On a converged volume the card tier keeps every coat within 10%, and the evidence test asserts that.
At the shipped shadow LOD the hand-over bakes the strands at 16^3 and the cards at 8^3, and there the
long coat's cards read 1.2-1.3x the strands' energy. Holding 64^3 through the hand-over removes it
(0.98 / 0.99) but costs 3-4 ms of bake CPU per frame for three walking animals. That is filed as #1508,
and the shipped cells hold a gross guard.

The short coat's cards cover 1.10-1.13 of its strands. At range this coat is mostly mane and tail,
flat fans seen face-on, and a card's width is the members' coverage averaged over every direction.

## What does not help

- **The exact geometric union** (`SourcePixelSize = 0`), or a cook at the real hand-over size: the
  coverage moves 1-2%.
- **A per-POINT fibre scale** instead of per group: within 2-3% of per group on both coats, and it
  needs a format change.
- **`MaxWidthCompensation` in the pass stats.** It is a maximum over every groom drawn.
