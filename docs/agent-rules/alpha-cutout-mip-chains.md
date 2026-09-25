# An alpha cutout's mip chain matches level 0's alpha histogram, so it needs no cutoff

*Issue #1453 (the cooked half of #1441). Rule first, measurements second.*

## The rule

1. **Build an alpha-tested texture's mips with `AlphaCoverageMips::Build(..., preserveCoverage = true)`.**
   Each level is box-filtered as usual, then its alpha values are replaced *in rank order* by
   level 0's: the texel whose footprint held the most alpha gets level 0's largest alpha, and so
   on. For every threshold, the fraction of texels at or above it then equals level 0's to within
   one texel. The chain does not depend on a cutoff, so one texture is right for every consumer.
2. **Stop the chain at `CappedLevelCount` (64 texels on the short side).** No remap can put 12%
   coverage into a 2x2 level. #1441 chose 64 by looking at the frames.
3. **Measure after the block encode, not before.** BC7 moves alpha by a few codes per block, and a
   remapped level has sharper alpha than level 0, so it suffers more. The cook decodes each level
   and nudges alpha by how far each texel's decoded value sits from the target for its decoded rank.
   It then re-encodes, up to three passes, keeping the best (`EncodeCoverageLevel` in
   `TextureCompression.cpp`).
4. **The cook has no consumer, so it gates on the pixels:** `IsCutoutAlpha` accepts some texels
   ≤ 8, some ≥ 247, and at most a third in between. The `.oloimport` field `AlphaMipChain:
   Coverage | Box` overrides it for a texture it misjudges.

## Why not a per-cutoff chain (Castaño, #1441's first version)

A cutoff belongs to the consumer, and one texture has several consumers. The repository draws
`pine_card` at 0.5, 0.3 and 0.25; `AlphaBlendLabels.png` is Opaque, Blend and Mask at 0.25, 0.5
and 0.75 at once. A per-cutoff chain is right only for whichever consumer set it last, and a cook
does not know any of them.

A Castaño chain built for 0.5 errs in a cutoff-dependent direction at the others. Measured on the
cards before encode, at the coarsest kept level: grass_card +7.4 points at 0.25 and −5.8 at 0.75;
broadleaf −10.5 at 0.75. The histogram-matched chain is within 0.01 points at all four cutoffs.

## On-screen numbers (`CookedCutoutCoverageEvidenceTest`, far pose, cards ~13 px)

Deviation from the one-level reference, which samples level 0 only:

| texture | cutoff | cooked | loose | plain box chain (the old cook) |
|---|---|---|---|---|
| pine_card | 0.25 | +9% | +9% | +47% |
| pine_card | 0.5 | 0% | 0% | −14% |
| pine_card | 0.75 | −11% | −11% | −83% |
| grass.png | 0.25 | +5% | +5% | +15% |
| grass.png | 0.75 | −9% | −9% | −45% |

Cooked and loose agree to 0.07 points everywhere. The residue that remains is bilinear filtering
of a coarse binary level: coverage at a low cutoff grows and at a high one shrinks. No single chain
avoids it; a per-cutoff chain can only trade it for being wrong at the other cutoffs.

## Traps

- **A box chain looks fine at 0.5.** At that cutoff it roughly keeps these shapes' coverage down to
  64 texels. A control that only checks 0.5 is vacuous. Check the other cutoffs: below 0.5 the
  box chain thickens a card, above 0.5 it erases it.
- **Rank by the float footprint average, not the stored byte.** Rounding merges footprints, and
  then texel order decides the tie.
- **The all-thresholds error overstates what the eye sees.** Decoded BC7 alpha lands on values
  level 0 never had, so the worst difference over all 255 thresholds (6% on the soft Sponza
  texture) is mostly quantisation between adjacent codes. Judge the correction at the cutoffs that
  are used: there it went from 1.4 to 0.3 points.
- **Soft alpha is the hard case, binary is easy.** A binary card is usually right after the first
  encode.
