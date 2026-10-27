# What a walking coat's self-shadow bake costs, and how it was cut (#1445)

Read before touching `GroomRenderPass::{AcquireDrawnPose, AcquireCoatVolume, BakeCoatVolume}`,
`GroomCoatShadow::{CoatBakeSubsetStride, SubsampleCoatSegments}` or
`CoatRebakePolicy::BakeSegmentsPerOccupiedVoxel`. Why a bound coat is baked from its drawn pose at
all is in [groom-deformed-coat-self-shadowing.md](groom-deformed-coat-self-shadowing.md).

## The rules

1. **Split a bake's time by stage before cutting it.** `GroomCoatShadowStats` times each stage
   (`BakeSegmentMicroseconds`, `BakeBinMicroseconds`, `BakePackMicroseconds`,
   `BakeUploadMicroseconds`, `DriftMicroseconds`, `DeformedPoseMicroseconds`).
   [Details](groom-coat-bake-cost-details.md#the-rules)
2. **Take the subset at the POSE, before it is evaluated,** so every per-segment stage pays for
   the subset, not only the binning. [Details](groom-coat-bake-cost-details.md#the-rules)
3. **The subset is fixed and hashed.** `CoatBakeKeepsSegment` hashes the segment index, so the same
   segments are kept every frame and spread along every strand; each kept radius is scaled by
   segments / kept. [Details](groom-coat-bake-cost-details.md#the-rules)
4. **The stride comes from the coat's own occupancy, measured by a full bake,** leaving
   `BakeSegmentsPerOccupiedVoxel` (8) per occupied voxel; a shadow-LOD resolution change
   re-measures. [Details](groom-coat-bake-cost-details.md#the-rules)
5. **Rewrite a ring slot in place only when no unsubmitted recording can read it:** never a slot
   bound on this tick or the one before; a slot of the wrong size gets a new texture.
   [Details](groom-coat-bake-cost-details.md#the-rules)
6. **Pack RGBA16F,** keeping RGBA32F only for a volume whose density would overflow a half.
   [Details](groom-coat-bake-cost-details.md#the-rules)
7. **A subset bakes on the full pose's grid.** The pass sets `DensityVolumeSettings::RadiusExtent`
   to kept/full, so the scaled radii carry fibre area but not reach (#1646);
   `GroomCoatShadowDeformed.ASubsetBakesOnTheFullPosesGrid` pins it.
   [Details](groom-coat-bake-cost-details.md#the-rules)

## Not done

Multithreaded binning, a per-frame rebake budget and a recorded Vulkan upload were each measured
and left out. [Details](groom-coat-bake-cost-details.md#not-done-and-why)

---

Full text, with the measurements per stage, the subset's quality curve and the stories behind
these rules: [groom-coat-bake-cost-details.md](groom-coat-bake-cost-details.md).
