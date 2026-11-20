# What a walking coat's self-shadow bake costs, and how it was cut (#1445)

> The rules from this guide, without the stories: [groom-coat-bake-cost.md](groom-coat-bake-cost.md).

Read before touching `GroomRenderPass::{AcquireDrawnPose, AcquireCoatVolume, BakeCoatVolume}`,
`GroomCoatShadow::{CoatBakeSubsetStride, SubsampleCoatSegments, CoatRebuildBudget}`,
`GroomRenderPass::SharedCoatVolume` or `CoatRebakePolicy::{BakeSegmentsPerOccupiedVoxel, MaxRebuildsPerFrame}`. Why a bound coat is baked from its drawn pose at
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

7. **A subset bakes on the full pose's grid.** Its radii are scaled by full/kept to carry the
   dropped segments' fibre, and the deposit reads a radius as area, never as reach; padded by the
   whole scaled radius, a card tier's subset grid came out up to seven times the full pose's, and
   the shadow LOD read the larger box as a closer coat, so its step alternated between two
   resolutions and every change forced a full rebake (#1646). The pass
   sets `DensityVolumeSettings::RadiusExtent` to kept/full;
   `GroomCoatShadowDeformed.ASubsetBakesOnTheFullPosesGrid` pins it.

8. **One rest bake serves every coat that would bake it** (#1594). At the acceptance scene's
   `population-dolly` cut about 44 foxes crossed a coat-LOD step in one frame and each rebuilt its
   volume, 40-50 ms apiece, all of them the same bake: the same asset, build and step, and the same
   body placed through a matrix that differs between animals only by float noise. A rest bake
   reads nothing else (`SharedCoatVolumeKey`: the asset and its build, the width scale, the
   resolution, the body's surface by content), so the first coat to need one bakes it and
   publishes it as a `SharedCoatVolume`, and every later coat with an equal key and a body placed
   within `kBodyPlacementToleranceVoxels` takes it (`CoatShadow.SharedBakesReused`). Per-coat
   state that is not baked -- the pose, wetness, colour, the transform -- stays out of the key,
   so it cannot leak through the bake. The key is the resolution, not the step: two steps that
   give one resolution give one volume. A shared texture is never rewritten in place (another
   coat may be reading it), its bytes are counted once by the bake (`CoatVolumeBytes`, the memory
   report's "shared coat rest bakes" row with the per-coat charge it avoids), and it is given back
   when the last coat holding it lets go. A pose bake is the coat's own pose and is never shared.

9. **At most `CoatRebakePolicy::MaxRebuildsPerFrame` bakes run a frame** (#1594), default 2. It
   is the backstop for a scene of mixed assets, where sharing does not help.
   `GroomCoatShadow::CoatRebuildBudget` keeps the deferred coats in the order they were first
   deferred and reserves the budget for the oldest at each frame start; a newcomer bakes only with
   budget left after every unserved reservation, so a deferred coat is served within
   ceil(position / budget) frames of being drawn, however many coats keep asking in front of it.
   The budget is the physical frame's (`GroomFrameState::FrameIndex`): every view's pass of one
   frame spends one budget, and a coat waiting in one view keeps its place while another renders
   (keyed on the pass tick, two views alternated reservations and no waiting coat ever baked).
   `kMaxPassesPerFrame` passes of an unchanged frame index start the next frame anyway, for a host
   that never advances it. The ticket is the COAT's, so a coat keeps its place when its step or
   tier changes while it waits, and a herd mate that takes another's shared bake leaves the queue.
   Reservations are decided before the frame knows who will ask, so on the frame a camera cuts away
   from waiting coats their reservations go unused and the coats now in view wait one frame more. A deferred coat draws the volume it already holds if it is the right kind (a
   pose bake for a posed coat, a rest bake for one at rest), at that volume's resolution, and is
   counted in `DrawnFromDeferredVolume`; one holding nothing falls back as
   `RepresentationNotBuilt`. **A rebake due on drift is never deferred**: it is forced
   (`RebuildsForced`), always runs and spends the budget the frame's other rebuilds would take.
   The earlier objection to a budget -- "it needs the stale bound's prioritisation" -- was right:
   with a plain budget of two, `TheMovingCoatShadowFollowsTheWalk`'s three walking coats drew
   volumes 1.8 voxels behind their pose against the 0.5 bound and went stale on some frames. So
   the budget defers only resolution and source changes (a camera cut's, an arrival's), whose
   waiting volume is still within the drift bound; the drift policy already paces the rest. A
   waiting pose bake's drift is measured even across a stride change
   (`MaxCoatPoseDriftAcrossStrides`; a resolution change re-measures with a full bake, which
   changes the stride under a waiting coat), so a coat that moves while it waits is forced too. Deferrals, the queue and its oldest wait are in
   `GroomCoatShadowStats`, `olo_groom_budget_stats.coatShadow` and the inspector, and a wave of
   deferrals is logged when it starts and when it drains.

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
- **Vulkan's blocking upload.** `VulkanTexture3D::SetData` waits on a fence per bake, so on
  Vulkan each rebake drains the queue. The ring does not change that; a recorded, in-frame upload
  would. Live on Vulkan (Release editor, `Scenes/GroomAnimals.olo`) the whole bake read 4.8-8.9 ms
  a frame for three walking coats, so it is not worth that change yet.
