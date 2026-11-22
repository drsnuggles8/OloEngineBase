# What a walking coat's self-shadow bake costs, and how it was cut (#1445)

Read before touching `GroomRenderPass::{AcquireDrawnPose, AcquireCoatVolume, BakeCoatVolume}`,
`GroomCoatShadow::{CoatBakeSubsetStride, SubsampleCoatSegments, CoatRebuildBudget}`,
`GroomRenderPass::SharedCoatVolume`, `CoatRebakePolicy::{BakeSegmentsPerOccupiedVoxel, MaxRebuildsPerFrame}`,
`VulkanTexture3D::SetData` or `VulkanRendererAPI::RecordStagedImageUpload`. Why a bound coat is baked from its drawn pose at
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
4. **The stride comes from the coat's own occupancy, measured by its first full bake,** leaving
   `BakeSegmentsPerOccupiedVoxel` (8) per occupied voxel; at another resolution the measurement is
   scaled (`EstimateCoatOccupiedVoxels`: the cube finer, the square coarser) rather than re-measured
   by a mandatory full bake. The estimate may still ask for stride 1; the square can undercount a
   sparse coat on a coarser grid, which the next bake that counts more corrects.
   [Details](groom-coat-bake-cost-details.md#the-rules)
5. **Rewrite a ring slot in place only when no unsubmitted recording can read it:** never a slot
   bound on this tick or the one before; a slot of the wrong size gets a new texture.
   [Details](groom-coat-bake-cost-details.md#the-rules)
6. **Pack RGBA16F,** keeping RGBA32F only for a volume whose density would overflow a half.
   [Details](groom-coat-bake-cost-details.md#the-rules)
7. **A subset bakes on the full pose's grid.** The pass sets `DensityVolumeSettings::RadiusExtent`
   to kept/full, so the scaled radii carry fibre area but not reach (#1646);
   `GroomCoatShadowDeformed.ASubsetBakesOnTheFullPosesGrid` pins it.
   [Details](groom-coat-bake-cost-details.md#the-rules)

8. **One rest bake serves every coat that would bake it** (#1594). A rest bake reads only
   `SharedCoatVolumeKey` (asset, build, width scale, resolution, the body by content) and the
   body's placement within `kBodyPlacementToleranceVoxels`, so a herd's coats at one coat-LOD step
   take one `SharedCoatVolume`. It is written once and never rewritten in place; a pose bake is
   never shared. `CoatsOfOneAssetAtOneStepShareOneRestBakeAndOthersDoNot` pins it.
   [Details](groom-coat-bake-cost-details.md#the-rules)
9. **At most `CoatRebakePolicy::MaxRebuildsPerFrame` bakes run a frame** (#1594), the oldest
   deferred coats first; a deferred coat draws the volume it holds, and every deferral is counted.
   **A rebake due on drift is never deferred**: it is forced and spends the budget, or a walking
   coat's shadow slides off it. `AMixedSceneRebuildsWithinTheBudgetAndNoWaitingCoatGoesUnshadowed` pins it.
   [Details](groom-coat-bake-cost-details.md#the-rules)
10. **On Vulkan, a bake inside a frame records its upload on the frame's command buffer**
    (`VulkanRendererAPI::RecordStagedImageUpload`, #1594), never the blocking one-shot: a one-shot
    waits for every frame in flight, a GPU frame or more of stall per bake whatever the bake's own
    cost. Outside a frame the one-shot stays; inside one, an upload that cannot be recorded is
    refused with an error rather than handed to the one-shot, and the bake that asked fails.
    [Details](groom-coat-bake-cost-details.md#the-rules)
11. **A rest bake's walk is kept across resolutions and baked at the subset** (`AcquireRestWalk`,
    #1594): the walk does not depend on the grid, and the stride follows rule 4 from the walk's
    first full bake. [Details](groom-coat-bake-cost-details.md#the-rules)

## Not done

Multithreaded binning was measured and left out.
[Details](groom-coat-bake-cost-details.md#not-done-and-why)

---

Full text, with the measurements per stage, the subset's quality curve and the stories behind
these rules: [groom-coat-bake-cost-details.md](groom-coat-bake-cost-details.md).
