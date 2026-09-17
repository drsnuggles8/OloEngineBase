# Foliage hierarchical wind

Author grass with stiffness and zero branch/leaf weights; author woody plants
with stiffness, branch movement, and leaf flutter. All three weights are in
[0, 1]. Zero weights retain legacy linear sway, including the original global-field clock and amplitude, and the legacy
impostor card amplitude.

`WindStrength` controls displacement in terrain-local metres. `WindStiffness`
progressively replaces linear tip influence with a quadratic bend and reduces
trunk response. `WindBranchWeight` adds slower height-dependent branch motion.
`WindLeafWeight` adds fast flutter to elevated vertices away from the trunk.
The unit-height, base-at-origin plant convention remains unchanged. Roots have
zero displacement. Normals follow the deformation Jacobian for authored layers.

`FoliageParams.glsl` declares the one UBO used by every foliage shader stage.
`FoliageWind.glsl` produces current and previous positions with the same
function. Colour, G-Buffer, depth, and shadows use that producer. Macro gust
phase uses absolute world roots, independent of camera-relative rebasing.
Fine motion uses a phase hashed from the canonical instance ID and local vertex
position, so regeneration and row ordering do not rephase surviving plants.

The hierarchical global response is capped to velocity length 20 before applying
the layer strength. Legacy zero-weight layers retain their uncapped response;
their bounds track `abs(strength) * max(abs(speed) * (1 + abs(gust)) * .1, 1.118034)` instead. Registry instance, spatial-group, and layer bounds include
`abs(strength) * (2 + .35 * branch + .15 * leaf)` on each axis. This covers the trunk
cap and the maximum lengths of both independent fine modes. Impostor card bounds
also include `sqrt(2) * atlas radius`, covering square-card corners at oblique
angles. Vulkan RT is a separate consumer (#1240); this
change provides the raster deformation contract and conservative instance data.

## Motion history

Hierarchical wind uses the scene animation clock, which freezes on pause.
The existing field clock is retained for legacy layers and also freezes while
rendering a paused scene. A paused frame
evaluates both wind positions at the same time; camera motion still uses the previous view-projection matrix. Reset, first appearance, or regeneration seeds the
previous wind time from the current time once. The following frame resumes
normal history. Changes to global wind parameters also suppress stale wind
velocity for one frame, because the old field is no longer the same surface.

Mesh/card LOD uses the existing complementary dither partition. Each draw
reprojects its own stable representation; it never interprets a mesh vertex as
a previous impostor vertex. Editing LOD configuration regenerates instances
and resets wind history once. Far impostors retain coherent trunk/branch sway
through the same producer, sampled at representative canopy height; detailed
leaf flutter is omitted from the baked atlas. Their depth shader uses the same
card placement, main-view atlas selection, and cutout as colour.

## Persistence and inspection

Scene YAML and cooked scene blobs carry `WindStiffness`, `WindBranchWeight`,
`WindLeafWeight`, and `WindDebugDisplacement`. Save-game format v36 appends those
fields after the landed v35 habitat block. Older scenes and v35-or-earlier saves default all
weights and debug to zero. Both loaders replace non-finite weights with defaults
and clamp finite weights to [0, 1].

The inspector's **Wind Displacement** toggle displays an unlit blue-to-magenta
diagnostic colour. Red encodes displacement length divided by the maximum
all-modes envelope (2.5 times absolute layer strength). Geometry, alpha coverage, depth,
and velocity remain active in this view.

Open `FoliageHierarchicalWind.olo` for grass, woody plants, shadows, and a far
impostor. `FoliageWindContract`, `FoliageWindShader`, and `FoliageWindSaveLoad`
pin bounds, anchoring, phase, previous positions, pause/reset, and persistence.
`FoliageWindEvidenceTest` captures the real OpenGL pipeline in each rendering
path, including deferred MSAA, two views, wind on/off, motion, and pause.

`FoliageWindPerf.HierarchicalColourAndShadowBudget` records actual foliage and
CSM pass timings for zero-weight and authored layers at 960x540, using five
warmup frames and the minimum of twenty samples. Its measurements use the
shared L6 baseline/retry policy rather than a wall-clock frame estimate.

## Recorded validation (2026-09-17)

Hardware: NVIDIA RTX 4090 (24 GiB, driver 616.64), Intel i7-14700KF,
64 GiB RAM, Windows 11. The live editor used a 960x540 viewport, the same scene
and recorded camera poses on both backends, and disabled editor guide overlays.
The [measurement record](../validation/foliage-hierarchical-wind-1236.json)
contains hashes, pixel differences, velocity, timings, allocations, and test counts.
Initial images are from `6b7c97132`; `postMergeReviewValidation` records the
eight-cell live matrix below and 121 tests at the post-merge snapshot `44180d762`.
`reviewFollowupValidation` separately records 132 focused tests at `791ac723d`;
these later tests did not repeat the live captures.

All eight reachable cells have ground and oblique wind-on/off captures, visible
foliage and shadows, finite RG16F motion over all 518,400 texels, and exactly zero
paused velocity with a stationary camera. Forward and Forward+ use their
single-sample production path; deferred exercises both 1x and 4x MSAA.

| Backend | Path | MSAA | Colour pass minimum (ms) | CSM minimum (ms) | Memory (MiB) |
|---|---|---:|---:|---:|---:|
| GL | forward | 1 | 0.292 | 1.380 | 618.12 |
| GL | forwardplus | 1 | 0.571 | 1.478 | 618.12 |
| GL | deferred | 1 | 1.069 | 1.543 | 637.89 |
| GL | deferred | 4 | 1.214 | 1.384 | 651.74 |
| Vulkan | forward | 1 | 0.361 | 0.751 | 918.51 |
| Vulkan | forwardplus | 1 | 0.809 | 1.765 | 918.51 |
| Vulkan | deferred | 1 | 0.503 | 1.655 | 956.01 |
| Vulkan | deferred | 4 | 0.979 | 1.290 | 1048.83 |

These are whole live pass costs, including export synchronization. Deferred
colour is `ScenePass` and includes other opaque work; CSM includes terrain and
foliage depth. Twenty live timing reports followed five warmup reports, with GPU
results at most two frames old. Vulkan Forward+ had one zero ScenePass and
one zero ShadowPass report excluded; affected passes retain 19 positive samples,
with 20 elsewhere. These timings establish a budget, not an isolated deformation speed-up.

OpenGL memory is engine-tracked allocation bytes. Vulkan memory is VMA
**device-local block bytes**; its engine allocation tracker is uninstrumented
and returns zero. The definitions differ and must not be compared as equivalent
VRAM usage. VMA heap rows are retained in the record. Wind adds 96 bytes to each
foliage UBO and stores canonical phase in an existing instance lane; it adds no
wind texture or instance stream.

The controlled OpenGL L6 fixture at `1ae024479` rendered 1,310 plants with five
warmups, twenty fresh positive samples per pass, and the shared retry/median
policy. Legacy foliage/CSM costs were 0.346112/0.445440 ms; hierarchical costs
were 0.254976/0.338944 ms. Timing variation prevents claiming a speed-up.

The 132-test follow-up passed 113 CPU/persistence/SSIM, 6 buffer/shader, 4 Vulkan,
8 strict visual/golden, and 1 strict performance test, with zero skips and all exits 0.
Fourteen foliage stages compiled through the Vulkan SDK. Persistence covers
YAML, cooked blobs, current saves, on-disk v34/v35 archives, and habitat compatibility.
`FoliageLeafTransmission.olo` also opened on both live backends.

`blitPreconditionValidation` records the latest 133-test rerun. Vulkan format
rejections preserve layouts and contents at 1x/4x. Both engine depth enums allocate
combined D32/S8; native depth-only mismatches are unreachable. Colour conversion
for deferred albedo/velocity debug channels remains unsupported.

CSM captures at different wind times changed 74,245 OpenGL and 49,028 Vulkan
foreground/background silhouette pixels, rather than relying on beauty images
alone. Both backends drew moving far impostors; changing mesh/card authoring
and pausing left finite, zero velocity. CPU/L2 history tests and the production
OpenGL L8 test pin the first reset/regeneration frame. Live stop captures check
finite resumed motion after settling; they do not claim to capture the first
reset frame.

The recorded Vulkan matrix log and fresh GL deferred-4x control log have no VUID,
synchronization hazard, or shader compilation failure. GL shader diagnostics
report zero errors; the Vulkan shader debugger is not initialized, so it gives
no count. Vulkan logs five terrain-only optional SSBO occupant diagnostics;
clean-base attribution remains unestablished and no terrain fix is claimed. The compiled graph hazard sweep is empty on both backends;
Vulkan's census records all six foliage shaders with positive draws and zero
dropped draws.

Representative live captures (the full on/off and velocity grid is committed):

- [OpenGL ground](../../OloEditor/assets/tests/visual/FoliageWindLive_GL_Forward_MSAA1_Ground.png)
  and [Vulkan ground](../../OloEditor/assets/tests/visual/FoliageWindLive_Vulkan_Forward_MSAA1_Ground.png).
- [OpenGL oblique](../../OloEditor/assets/tests/visual/FoliageWindLive_GL_Deferred_MSAA4_Oblique.png)
  and [Vulkan oblique](../../OloEditor/assets/tests/visual/FoliageWindLive_Vulkan_Deferred_MSAA4_Oblique.png).
- [OpenGL displacement](../../OloEditor/assets/tests/visual/FoliageWindLive_GL_Debug.png)
  and [Vulkan displacement](../../OloEditor/assets/tests/visual/FoliageWindLive_Vulkan_Debug.png).
- [Vulkan impostor](../../OloEditor/assets/tests/visual/FoliageWindLive_Vulkan_Impostor.png),
  [depth](../../OloEditor/assets/tests/visual/FoliageWindLive_Vulkan_Depth.png),
  and [shadow](../../OloEditor/assets/tests/visual/FoliageWindLive_Vulkan_ShadowA.png).

Master's species/clumping PR #1302 lands habitat v35; this branch appends wind
at v36. On-disk v34/v35 tests preserve habitat and default wind fields.
