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

4a. **A ribbon's coat-shadow march starts on its tube's LIT side, one radius from the axis
   (#1533).** This is rule 4's reasoning applied to light, in `oloGroomCoatTauAndBody`. A card sits on its
   lock's centreline, the densest line in the volume, so a march from the axis shadowed every card
   fragment by its own lock. A viewer sees a lock's outer strands, not its centre, and under a
   converged volume the long coat's cards read 0.74-0.80 of the strands. From the lit side they
   read 1.05-1.06. A strand's radius is far below a voxel, so the strand tier is unchanged. The
   offset answers for the lit outer strands. A backlit card, whose viewer sees the lock's shaded
   side, therefore reads too bright.

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

`TheCoatKeepsItsCoverageFromNearToFar`, GL, frozen pose, ladder on / off, after #1533 (rule 4a,
and stand-ins that keep their guides' drape length). The first four columns are rule 6's quantities.
The last is seen energy (#1509): both frames over the union of the coats' pixels, backdrop
included. The contract holds on seen energy wherever the coat is a real share of the region.

| | coverage | radiance | shadow | energy | seen |
|---|---|---|---|---|---|
| long coat, 19 m, converged volume | 1.03 | 0.97 | 1.15 | 1.15 | **1.05** |
| long coat, 32 m, converged volume | 1.02 | 0.96 | 1.14 | 1.11 | **1.06** |
| short coat, 19 m, converged volume | 1.17 | 0.92 | 0.96 | **1.03** | |
| short coat, 32 m, converged volume | 1.14 | 0.86 | 0.95 | **0.94** | |
| long coat, 19 m, shipped shadow LOD | 1.03 | 0.99 | 1.16 | 1.18 | 1.09 |
| long coat, 32 m, shipped shadow LOD | 1.02 | 0.96 | 1.46 | 1.42 | 1.37 |
| long coat, 19 m, Deferred / Forward+ / Deferred MSAA 4 | | | | 1.21 / 1.20 / 1.21 | 1.12 / 1.12 / 1.14 |

On a converged volume the card tier keeps every coat within 10%, and the evidence test asserts that.
The short coat's seen energy is ill-conditioned (a coat share of 0.05), so it falls back to
per-pixel energy. At the shipped shadow LOD the hand-over bakes the strands at 16^3 and the cards at
8^3, and the long coat's cards read up to 1.4x the strands' energy there. Holding 64^3 through the
hand-over removes it but costs 3-4 ms of bake CPU per frame for three walking animals. That is filed
as #1508, and the shipped cells hold a gross guard (0.75-1.40). The 32 m cell sits near its ceiling.

The short coat's cards cover 1.14-1.17 of its strands. At range this coat is mostly mane and tail,
flat fans seen face-on, and a card's width is the members' coverage averaged over every direction.

## The dog's hand-over, held (#1533)

`DogShowcaseEvidenceTest.TheCardHandOverStepsTheCoatWithinTheNearLaddersBar` holds the camera at
17-27 m (the hand-over happens in at about 16 m and out at about 22.5 m at 1920x1080) and compares the
coat on cards with the same coat on strands at the same visibility step. `m_CardPixelSize` 0.5 is the
smallest threshold the policy keeps; 0 reads as unset and falls back to 256. Cards cover 0.9-1.6% more
and send 3.6-4.8% less light, inside the near ladder's 5% step bar.

The split at 22 m, cards over strands in energy: 0.960 shipped, 0.953 at kappa 0, 0.941 without the
sky, 1.110 with single scattering only, 0.985 unlit (coverage 1.015 throughout). So it is the fibre
model, not the shadow: a card's single scattering is brighter than its strands', and its
multiple-scattering back-scatter, a lobe of the tangent, much weaker. A card shades at its kept
strand's one tangent while its members spread around it. Widening a card's lobes by that spread is
the fix that would take the step toward zero; it needs the spread cooked per card.

## What does not help

- **The exact geometric union** (`SourcePixelSize = 0`), or a cook at the real hand-over size: the
  coverage moves 1-2%.
- **A per-POINT fibre scale** instead of per group: within 2-3% of per group on both coats, and it
  needs a format change.
- **`MaxWidthCompensation` in the pass stats.** It is a maximum over every groom drawn.
