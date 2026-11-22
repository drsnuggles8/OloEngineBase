# What a walking coat's self-shadow bake costs, and how it was cut (#1445)

> The rules from this guide, without the stories: [groom-coat-bake-cost.md](groom-coat-bake-cost.md).

Read before touching `GroomRenderPass::{AcquireDrawnPose, AcquireCoatVolume, BakeCoatVolume}`,
`GroomCoatShadow::{CoatBakeSubsetStride, SubsampleCoatSegments, CoatRebuildBudget}`,
`GroomRenderPass::SharedCoatVolume`, `CoatRebakePolicy::{BakeSegmentsPerOccupiedVoxel, MaxRebuildsPerFrame}`,
`VulkanTexture3D::SetData` or `VulkanRendererAPI::RecordStagedImageUpload`. Why a bound coat is baked from its drawn pose at
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

4. **The stride comes from the coat's own occupancy, measured by its first full bake.** Rule 4 of
   the static guide says a voxel holding too few fibres stores a sample, not an average. The first
   bake of an entry is full; it records how many voxels it occupied, and the stride is the one that
   leaves `BakeSegmentsPerOccupiedVoxel` (8) in each on average. At another resolution the
   measurement is scaled, not re-measured (#1594): `EstimateCoatOccupiedVoxels` scales a finer grid
   by the cube (each voxel splits into ratio^3, of which a coat occupies at most all: never fewer
   voxels than the coat has, so a coat first baked coarse at range is not held at that stride up
   close) and a coarser one by the square (a coat's surface: right for a shell or a pelt). Every
   bake then teaches the measurement (`UpdateCoatOccupancy`): a full bake measures it, a subset
   that counted more than the estimate raises it (a subset only undercounts -- a sparse coat on a
   coarser grid, a first full bake in a compact pose, another coat's width scale), and one that kept
   the floor's segments in every voxel it occupied replaces an estimate scaled from another grid --
   never a measurement at its own grid, which a subset only undercounts: adopting that shrinks the
   occupancy bake after bake, grows the stride, and rebakes a still coat on a different subset every
   frame. So only the first bake at a new grid takes the estimate's stride. Keeping the whole measurement for a coarser grid instead was tried
   and measured: a walking coat's forced rebakes after the cut put nearly every segment into the
   grid, 100 ms a bake for five frames in a row. The pose takes the stride for the resolution it
   will be baked at (`ChooseCoatLod`), so a cut's first bake is not the old grid's subset.
   `AResolutionChangeEstimatesOccupancyOnTheSafeSide` and `ABakeCorrectsTheOccupancyEstimateBothWays`
   pin both halves. It used to re-measure with one full bake, and live at the acceptance scene's
   camera cut that full bake of a walking coat -- forced by its drift (rule 9) -- was 250 ms of
   binning in one frame.

5. **A ring slot may be rewritten in place only when no unsubmitted recording can read it.**
   `BakeCoatVolume` never rewrites a slot bound on this tick or the one before (a split-screen frame
   runs the pass once per camera). Older reads are in submitted frames: GL orders the upload after
   them, and Vulkan's upload (rule 10) is a copy on the same queue whose barrier's first scope is
   everything submitted and recorded before it. A slot of the wrong size gets a new texture, and the old one
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
   is the backstop for a scene of mixed assets, where sharing does not help. **Why 2**, from the
   acceptance scene's population-dolly cut (Release Vulkan editor, rules 8, 10 and 11 in force):
   a deferrable bake there costs 7-40 ms (a herd rest bake at the subset; 110 ms only for a coat's
   first walk), the frame around it ~33 ms, and the cut queues up to 16 bakes, which drain in 8
   frames at 2 a frame (the log's "59 deferrals, at most 16 waiting at once", 8 frames). One a frame
   would leave those coats on their old volume for 16 frames, half a second, to save about one
   bake's 7-40 ms in each; four would put up to ~160 ms of bakes into a single frame, the hitch
   the budget exists to cut. Drift rebakes are forced past it either way.
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
   (`MaxCoatPoseDriftAcrossStrides`; a resolution change changes the stride under a waiting coat,
   rule 4), so a coat that moves while it waits is forced too. Deferrals, the queue and its oldest wait are in
   `GroomCoatShadowStats`, `olo_groom_budget_stats.coatShadow` and the inspector, and a wave of
   deferrals is logged when it starts and when it drains.

10. **On Vulkan, a bake inside a frame records its upload on the frame's command buffer**
    (#1594). `VulkanTexture3D::SetData` used a blocking one-shot submit, which executes after every
    frame already submitted and makes the CPU wait for all of them. Live on the acceptance scene
    (Release Vulkan editor, the GPU busy with another worktree's sweep) the population-dolly cut's
    `Groom::CoatVolume` calls read 80-400 ms each with no child scope -- the bake's own stages are
    a few to tens of milliseconds -- so the two bakes a frame the budget allowed still cost
    300-470 ms. Inside a recording bracket the upload is now
    `VulkanRendererAPI::RecordStagedImageUpload`: staging, a barrier into TRANSFER_DST from the
    tracked layout, the copy, and a barrier back to the sampled layout with the tracker updated,
    since a coat volume is bound through a persistent heap slot that carries no per-draw transition.
    The staging buffer goes to the deferred-reclaim queue. Outside a frame the one-shot stays.

11. **A rest bake's walk is kept across resolutions and baked at the subset** (#1594). A rest bake
    reads the coat at rest: with coat authoring the drawn strands' walk
    (`BuildGroomRestCentrelines`), without it the asset sampled at the strand budget's stride.
    Neither depends on the grid, yet every coat-LOD step change produced it again -- 35-85 ms of each
    rest bake at the acceptance scene's camera cut, under no named scope -- and binned all of it at
    the finer grid. `AcquireRestWalk` keeps it per source (the asset, its build and coat digest, the
    streamed level, authored or sampled), without the width scale; at least
    `kRestWalksAlwaysKept`, and past that as many as fit `kRestWalkBytes`, the least recently used
    out first, all given back after `kCoatBodyIdleFrames` without a bake, and counted in
    `CpuBakeInputBytes` and the memory report's CPU row. The walk's first bake is full and records
    its occupancy; later bakes take rule 4's stride from it, scaled to their resolution, with rule
    7's `RadiusExtent` so the subset keeps the full grid. A coat that shares a bake (rule 8) may take
    one made at another stride: both are the same coat within rule 4's error.
    `CoatShadow.RestWalksReused` and `MaxRestBakeStride` count it.

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

## Measured at the camera cut (#1594)

The acceptance scene's `coats-closeup` -> `population-dolly` cut, Release Vulkan editor,
`scripts/perf/acceptance-live.py --keep-series` with the CPU scope ring armed (1850 frames x 1024
events) before each cut. Master and branch arms alternated, and only renderer-advanced frames are
counted. A hitch is a frame in the first 120 after the cut over 300 ms or over twice the ring's
p50; its owner is the scope with the most self time.

At the final head, arms in branch, master, branch order:

| | master | branch |
|---|---|---|
| worst frame after the cut (forward / forward+ / deferred) | 1058-1133 ms | 607-721 ms |
| worst frame after the cut (hybrid) | 1293-1462 ms (earlier rounds) | 958-979 ms |
| hitches owned by `Groom::CoatVolume` | every one (7-9 a cell) | none (0 of 39) |
| hitches whose top self-time scope is under `Groom::CoatVolume` | every one | 1 of 39, by 1.5 ms |
| largest single `CoatVolume` call | 297-301 ms | 78-116 ms |
| population-dolly p95 (forward / forward+ / deferred) | 64.9-65.4 ms | 48.8-50.0 ms |

The branch's worst frame is now `ShadowPass::CollectGrooms` (226-406 ms self) and `GroomPass`.
The one frame topped by a scope under the coat volume is a forced drift rebake of a walking coat
at the new, finer grid: `CoatBin` 117.9 ms against `CollectGrooms` 116.4 ms. Its first bake there
takes rule 4's safe-side cube estimate, which keeps more segments than the coat needs until the bake
corrects it. The coat-volume subtree's summed self time also exceeds the single top owner in the
one or two frames that hold a coat's first rest walk and bake after the cut. Each rule's share, in
the order they landed:

- Sharing and the budget (rules 8, 9) cut the per-frame `CoatVolume` from 700-900 ms to
  240-340 ms. Single calls were still 80-400 ms with no child scope: the Vulkan wait (rule 10).
- The recorded upload (rule 10) cut the upload stage to 0.1-1.2 ms. A walking coat's full
  re-measure then showed up as one 250 ms bake (rule 4), and each rest bake re-walked the coat for
  35-85 ms (rule 11).
- With rules 4 and 11, no hitch is the coat volume's, and after +8 the largest pose bake is
  0.7-4 ms a frame.

## Not done, and why

- **Multithreaded binning.** After the subset it is 4.8 ms for three coats. Order-independent
  binning is only deterministic with a fixed partition and a fixed merge order, which costs a
  per-partition volume; not worth it at this size.
- **Vulkan's blocking upload** was left out at first: live on Vulkan (Release editor,
  `Scenes/GroomAnimals.olo`) the whole bake read 4.8-8.9 ms a frame for three walking coats. It is
  done now (rule 10): at the acceptance scene's camera cuts, with the GPU busy, single bakes read
  80-400 ms with no child scope, the CPU waiting on the queue.
