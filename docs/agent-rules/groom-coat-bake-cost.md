# What a walking coat's self-shadow bake costs, and how it was cut (#1445)

Read before touching `GroomRenderPass::{AcquireDrawnPose, AcquireCoatVolume, BakeCoatVolume}`,
`GroomCoatShadow::{CoatBakeSubsetStride, SubsampleCoatSegments}` or
`CoatRebakePolicy::BakeSegmentsPerOccupiedVoxel`. Why a bound coat is baked from its drawn pose at
all is in [groom-deformed-coat-self-shadowing.md](groom-deformed-coat-self-shadowing.md).

## The rules

1. **Split a bake's time by stage before cutting it.** `GroomCoatShadowStats` carries
   `BakeSegmentMicroseconds`, `BakeBinMicroseconds`, `BakePackMicroseconds`,
   `BakeUploadMicroseconds` and `DriftMicroseconds`; the pose evaluation is
   `DeformedPoseMicroseconds`. On the walk, four of the six stages scale with the segment count and
   together were 92% of the cost. The texture upload the issue suspected was 2%.

2. **Take the subset at the POSE, before it is evaluated.** Every per-segment stage then pays for
   the subset. Subsampling inside the bake would have cut the binning and left the pose
   evaluation, the largest single stage after it, untouched.

3. **The subset is fixed and hashed.** `CoatBakeKeepsSegment` hashes the segment index, so the same
   segments are kept every frame (a per-bake draw would re-dither the shadow on a coat that is only
   walking) and spread along every strand (`index % stride` keeps the same positions of every
   strand when the segment count divides by the stride). Each kept radius is scaled by
   segments / kept, the achieved fraction, so the expected fibre area in every voxel is unchanged.

4. **The stride comes from the coat's own occupancy, measured by a full bake.** Rule 4 of the
   static guide says a voxel holding too few fibres stores a sample, not an average. The first bake
   of an entry is full; it records how many voxels it occupied, and the stride is the one that
   leaves `BakeSegmentsPerOccupiedVoxel` (8) in each on average. A shadow-LOD resolution change
   re-measures with one full bake, so a coat first baked coarse at range is not held at that
   stride up close.

5. **A ring slot may be rewritten in place only when no unsubmitted recording can read it.**
   `BakeCoatVolume` never rewrites a slot bound on this tick or the one before (a split-screen frame
   runs the pass once per camera). Older reads are in submitted frames: GL orders the upload after
   them, and Vulkan's `SetData` is a one-shot on the same queue, whose barrier's first scope is
   everything submitted before it. A slot of the wrong size gets a new texture, and the old one
   goes to the deferred deletion as before. Every slot is counted in `CoatBytes`.

6. **Pack RGBA16F.** It halves the volume. It was RGBA32F only because the GL backend handed
   RGBA16F uploads to the driver as `GL_FLOAT` while its size check wanted eight bytes a texel;
   `Texture3DUploadGpu` pins the fix. The pass keeps RGBA32F for a volume whose density would
   overflow a half.

## Measured

`TheMovingCoatShadowFollowsTheWalk`, **Release**, GL, two walking horses (57k and 94k strands) and
a 44k-strand turning head, 59 frames after warm-up, three rebakes a frame. Two interleaved runs of
each binary; per frame for the three coats:

| stage (ms/frame) | before | after |
|---|---|---|
| pose evaluation | 28.0 / 28.5 | 3.8 / 3.7 |
| drift | 4.6 / 4.5 | 0.3 / 0.3 |
| segments | 12.3 / 11.9 | 0.7 / 0.7 |
| binning | 42.8 / 42.8 | 4.8 / 4.8 |
| pack | 0.6 / 0.6 | 1.1 / 1.1 |
| texture create + upload | 1.5 / 1.7 | 0.4 / 0.4 |
| **total** | **~95** | **~11.5** |

Pack rose because the half conversion costs more than a copy; it is the price of half the bytes.

**Quality** (`GroomCoatShadowDeformed.ASubsetBakeKeepsTheShadowItReplaces`: a 40 000-strand pelt at
32³, 29 segments per occupied voxel, against the exact reference). Excess mean |dT| over the full
bake's own 0.024:

| segments per occupied voxel | 14.6 | **9.7 (shipped, stride 3)** | 7.3 | 3.6 | 1.8 | 0.5 |
|---|---|---|---|---|---|---|
| excess mean \|dT\| | +0.0009 | **+0.0021** | +0.0028 | +0.0078 | +0.018 | +0.048 |

The error grows smoothly as the voxels empty, and the target of 8 keeps it under a tenth of the
representation's own error.

## Not done, and why

- **Multithreaded binning.** After the subset it is 4.8 ms for three coats. Order-independent
  binning is only deterministic with a fixed partition and a fixed merge order, which costs a
  per-partition volume; not worth it at this size.
- **A per-frame rebake budget.** With every coat rebaking every frame for 11.5 ms total, a
  budget that lets coats lag would buy little, and it needs the stale bound's prioritisation.
- **Vulkan's blocking upload.** `VulkanTexture3D::SetData` waits on a fence per bake, so on
  Vulkan each rebake drains the queue. The ring does not change that; a recorded, in-frame upload
  would. Measured live in the PR, not fixed here.
