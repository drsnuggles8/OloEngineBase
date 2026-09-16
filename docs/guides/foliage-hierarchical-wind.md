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
their bounds track `abs(speed) * (1 + abs(gust)) * .1` instead. Registry instance, spatial-group, and layer bounds include
`strength * (2 + .35 * branch + .15 * leaf)` on each axis. This covers the trunk
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
`WindLeafWeight`, and `WindDebugDisplacement`. Save-game format v35 appends those
fields after the v33 leaf material block. Older scenes and v34 saves default all
weights and debug to zero. Both loaders reject non-finite weights and clamp
them to [0, 1].

The inspector's **Wind Displacement** toggle displays an unlit blue-to-magenta
diagnostic colour. Red encodes displacement length divided by the maximum
all-modes envelope (2.5 times layer strength). Geometry, alpha coverage, depth,
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

## Recorded validation (2026-09-16)

Hardware: NVIDIA RTX 4090 (24 GiB, driver 616.64), Intel i7-14700KF,
64 GiB RAM, Windows 11. The live editor used a 960x540 viewport, the same scene
and recorded camera poses on both backends, and disabled editor guide overlays.
The [measurement record](../validation/foliage-hierarchical-wind-1236.json)
contains image hashes, wind-on/off pixel differences, raw velocity maxima,
pass timings, allocation totals, and the exact test counts.

All eight reachable cells have ground and oblique wind-on/off captures, visible
foliage and shadows, finite RG16F motion over all 518,400 texels, and exactly zero
paused velocity with a stationary camera. Forward and Forward+ use their
single-sample production path; deferred exercises both 1x and 4x MSAA.

| Backend | Path | MSAA | Colour pass minimum (ms) | CSM minimum (ms) | Memory (MiB) |
|---|---|---:|---:|---:|---:|
| GL | forward | 1 | 0.406 | 1.437 | 618.12 |
| GL | forwardplus | 1 | 0.559 | 1.445 | 618.12 |
| GL | deferred | 1 | 0.936 | 1.372 | 637.89 |
| GL | deferred | 4 | 1.200 | 1.405 | 651.74 |
| Vulkan | forward | 1 | 0.345 | 0.733 | 1058.20 |
| Vulkan | forwardplus | 1 | 0.361 | 0.729 | 1058.20 |
| Vulkan | deferred | 1 | 0.462 | 0.726 | 956.01 |
| Vulkan | deferred | 4 | 0.591 | 0.726 | 1048.83 |

These are whole live pass costs, including export synchronization. Deferred
colour is `ScenePass` and includes other opaque work; CSM includes terrain and
foliage depth. Twenty live timing reports followed five warmup reports, with GPU
results at most two frames old. One zero ShadowPass report in each Vulkan
forward cell was excluded; the JSON records 19 positive samples there and 20
elsewhere. These timings establish a budget, not an isolated deformation speed-up.

OpenGL memory is engine-tracked allocation bytes. Vulkan memory is VMA
**device-local block bytes**; its engine allocation tracker is uninstrumented
and returns zero. The definitions differ and must not be compared as equivalent
VRAM usage. VMA heap rows are retained in the record. Wind adds 96 bytes to each
foliage UBO and stores canonical phase in an existing instance lane; it adds no
wind texture or instance stream.

The controlled OpenGL L6 fixture rendered 1,310 authored plants, using five
warmups and minimum-of-20 sampling with the shared retry/median policy. The
strict comparison recorded foliage/CSM costs of 0.132096/0.180224 ms for legacy
weights and 0.100352/0.180224 ms for hierarchical weights. GPU boost and timing
variation prevent treating the difference as a speed-up.

The CPU/persistence run passed 78 tests, buffer/shader run 5, Vulkan device run 3,
strict visual comparison 3, and strict performance comparison 1: **90 passed,
zero skipped, all exits 0**. Fourteen foliage shader stages also compiled through
the Vulkan SDK. Persistence includes YAML, an actual cooked scene blob, current
save round trips, and an actual on-disk v34 archive. The pre-existing
`FoliageLeafTransmission.olo` also opened on both live backends.

CSM captures at different wind times changed 74,245 OpenGL and 49,028 Vulkan
foreground/background silhouette pixels, rather than relying on beauty images
alone. Both backends drew moving far impostors; changing mesh/card authoring
and pausing left finite, zero velocity. CPU/L2 history tests and the production
OpenGL L8 test pin the first reset/regeneration frame. Live stop captures check
finite resumed motion after settling; they do not claim to capture the first
reset frame.

Both final editor logs contain no VUID, synchronization hazard, or shader
compilation failure. OpenGL shader diagnostics report zero errors. Vulkan
still logs missing optional GPU-terrain SSBO occupants at 59/79 in the unchanged
terrain shaders; that terrain diagnostic is disclosed and no terrain correction
is claimed here. The compiled graph hazard sweep is empty on both backends;
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

Master remains save format v34 at publication. This task appends v35 wind fields;
the concurrently open species/clumping PR #1302 also appends fields. Whichever
PR lands second must rebase, place its fields after the first v35 block, use
v36, and rerun its persistence tests before merging.
