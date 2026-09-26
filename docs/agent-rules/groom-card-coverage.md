# Groom card coverage across the hand-over (#1428)

Read before touching `GroomLodBuilder`'s card widths, the per-role width compensation in
`GroomStrandMesh.cpp`, the card-tier branch of `GroomRenderPass::AcquireCoatVolume`, or
`GroomAnimalsAcceptanceEvidenceTest.TheCoatKeepsItsCoverageFromNearToFar`. The ladder's own rules
are in [groom-representation-lod.md](groom-representation-lod.md); this file adds what a real,
dense coat on a real animal taught.

## The rules

1. **A card is as wide as its members COVER, as the strand shader draws them at the hand-over,
   never as wide as their sum.** Strands that share a clump cell grow from roots a few millimetres
   apart and are combed the same way, so on a dense coat they overlap. On the long-coated horse the
   long-hair role's members cover 0.40 of their summed width, and a summed card tier drew **2.6x**
   the coat's contrast at the hand-over. `GroomCardWidth::Covered` integrates, across the card and
   averaged over the directions it can be seen from, the coverage of the members' bands widened to
   the pixel at `GroomCardSettings::SourcePixelSize` the way the shader widens them. It equals the
   sum when nothing overlaps, which is why #1252's reference coats (covered/sum 0.94 to 0.99)
   barely moved and still pass their comparison.

2. **A card-tier self-shadow is baked at the coat's FIBRE area, not the cards' drawn area.**
   Rule 1 makes the cards narrower than the fibre they stand for, and the density volume stores
   fibre. Baked from the cards as drawn, the card tier read as a lighter coat: switching the long
   coat's self-shadow off moved the card tier from 0.85 of the strand tier's contrast to 0.97. The
   pass scales a card-tier bake by the base groom's fibre area over the level's
   (`CacheEntry::CoatFibreAreaScale`), measured once from the asset.

3. **The strand budget's width compensation is PER ROLE, in the build.** The budget thins each
   coat role at its own stride; one groom-wide `1 / achieved` widened guard hair at stride 1 by
   2.2x beside an undercoat at stride 7, and drew the long coat at 1.12x its contrast one rung
   before the hand-over. `GroomRoleWidthCompensation` widens each role by the inverse of what its
   own stride kept, and the stream carries it, so the build settings carry the cap
   (`GroomStrandBuildSettings::MaxWidthCompensation`) and the cache key does too.

4. **Measure a LOD on a frozen pose, and conserve a linear quantity.** Two separate traps:
   - The first version of the coverage case captured the coat on and off while the walk advanced
     between them, so the legs moving read as coat. It put the long coat's share of the animal at
     0.84 where it is 0.41 frozen, and the card tier's error at 1.35 where it was 1.9. Freeze the
     clips (`m_IsPlaying = false`) and check the control: the Near stop draws the same strands on
     both arms and must read exactly 1.
   - The **share** (pixels changed over a threshold) is not linear in coverage. Cards carry the same
     coverage in fewer, fuller pixels than the sub-pixel strands they replace, so an equal-coverage
     card tier counts fewer pixels. The tolerance is on the **contrast**, the summed
     |luma(on) - luma(off)|; the share keeps its gross-pop bounds.

5. **Separate geometry from shading with the CPU coverage model first.** `GroomCoverage` is the
   GPU's arithmetic twin, and it put the covered cards at 1.06-1.10 of the strands' drawn coverage
   while the GL frames said 0.85. The gap was therefore not the width rule, and turning one lever
   (the self-shadow) off found it. One CPU pass cost seconds; the GL dolly that preceded it cost
   twenty minutes per arm.

## Measured

`TheCoatKeepsItsCoverageFromNearToFar`, GL, frozen pose, contrast kept (ladder on / off). The run
repeats to the integer.

| | 14 m (strands) | 19 m (hand-over) | 32 m |
|---|---|---|---|
| long coat, summed cards and one compensation (before) | 1.12 | 2.61 | 2.56 |
| long coat, covered cards, bake at the cards' area | 1.00 | 0.85 | 0.75 |
| **long coat, shipped** (Forward) | **1.06** | **0.88** | **0.82** |
| long coat, shipped, Deferred / Forward+ / Deferred MSAA 4 | | 0.87 / 0.88 / 0.87 | |
| long coat, shipped, self-shadow off | 1.01 | 0.98 | 0.94 |
| **short coat, shipped** | **1.02** | **0.85** | **0.74** |
| short coat, shipped, self-shadow off | 1.00 | 0.90 | 0.81 |

**Still open (#1428).** The width fix conserves the long coat's coverage (0.94-0.98 with the
self-shadow off, on every path). Two residuals remain, and neither is the card's width: the card
tier's self-shadow darkens about 10% less than the strands' even at the coat's fibre area (fewer,
fatter occluders), and the short coat loses 19% at 32 m even unshadowed. The coverage case pins the
fix with a card-tier floor of 0.70 and a ceiling of 1.10, which the summed tier fails at every stop.

## What does not help

- **The exact geometric union** (`SourcePixelSize = 0`): 0.84 at the hand-over against the
  footprint's 0.86. The pixel moves the answer by about 2% on this coat.
- **Reading the difference as distance-dependent.** Both the summed and the covered tiers held a
  near-constant ratio from 19 m to 45 m, so the fix is not a per-distance width.
- **`MaxWidthCompensation` in the pass stats.** It is a maximum over every groom drawn, so in the
  fixture it reads the cap whichever coat is being measured. Read the coat's own strides.
