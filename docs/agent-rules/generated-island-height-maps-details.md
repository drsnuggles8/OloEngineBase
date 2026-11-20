# Generated island height maps: details

The stories, measurements and derivations behind
[generated-island-height-maps.md](generated-island-height-maps.md). From issue #1721, the art
review of *Drift*'s six islands.

## 1. The scene block is derived data

`tools/drift-islands/build_islands.py` writes four maps per island and, with `--stamp-scene`,
rewrites each island's entity in `Drift.olo` from its header comment to the end of its
`FoliageComponent`, plus the LAYOUT section of the scene's header comment. Everything in those
ranges follows from the recipe: the tile size and height scale from `recipes.TILES`, each layer's
`TilingScale` from the tile size, the landfall radius and the table of peaks and distances from
the height map on disk.

The header's table used to be typed. After the first regeneration it named peaks and distances
that no island had any more, and nothing failed. It is generated now for that reason.

`--check` regenerates every map and compares pixels with the files on disk; it passed on a second
run of all six. The generator is seeded and uses no clock, so a difference means the recipe, numpy
or scipy changed (`requirements.txt` pins the versions the shipped maps were made with).

## 2. What the radial mask could not make

The islands were `TerrainGenerator::GenerateHeightField` with `IslandFalloff: 1`: fBm, a ridged
blend and a domain warp, uniform over the tile, multiplied by one radial mask. The review found,
on the base commit:

- Top-down, all six share one circular outline with a scalloped rim. Atoll had no ring or
  lagoon, Sisters was one blob, Stacks had no stacks.
- The coast was the mask's own ramp. Jolt rays down each island's approach bearing measured the
  slope at the waterline: Dunes 58 degrees, Sisters 55-60, Ridgeback 45-50. Mesa's terracing
  quantised the ramp into a 17 m vertical step, from 7 m under the sea to 10 m above it within
  2 m.
- `ProceduralErosionIterations` was 0 on all six, so there was no drainage, talus or strata.

A mask fixes the tile border (see [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md)) and
nothing else: it has no notion of where a spine runs or where sand lies. So the tool draws each
island's character directly, in metres, and derives the rest:

1. **Landform** (`recipes.py`): a polyline spine with separate north and south half-widths
   (Ridgeback), two tilted blocks and a shingle bar (Sisters), tiered superellipse blocks and
   faceted, fluted sea stacks on a platform (Stacks), a caprock, bench and talus (Mesa), transverse
   and crescent dunes aligned to the scene's wind with a spit (Dunes), a ring reef with tapered
   cays and one pass (Atoll).
2. **Erosion** (`erosion.py`): the droplet model `TerrainGenerator::ApplyErosion` uses, with a
   hardness map so a cliff resists and a dune does not, the sea as a sink, and thermal slumping to
   an angle of repose.
3. **Shore, sites, border, materials**: sections 3, 4, 7 and 11.

Two traps in the erosion, both of which showed as a peak that grew instead of wearing down
(Ridgeback reached 154 m from a recipe that tops out at 84):

- Heights were in metres while the model's constants assume a map normalised to its width. The
  tool divides by the map's width in metres before eroding.
- A batch of droplets was applied as if each acted alone. Twenty thousand droplets that share a
  cell each removed the full amount the cell could give. The loop now applies one step for the
  whole batch and shares each cell's change among the droplets standing on it.

## 3. Border and summit

The border rule is the one [terrain-tile-meets-ocean.md](terrain-tile-meets-ocean.md) rule 3
states for the mask, restated for a map: the last pipeline step sets every border texel to exactly
0, after erosion and after the sites, so no later pass can undo it. The test is an equality.

A map stores heights as 16 bits of `HeightScale`. A recipe whose summit reaches the top of that
range is clipped flat with no error anywhere, so `pipeline.generate` raises when the peak does not
fit, and the test asserts the stored maximum is below 1.

`HeightScale` also sets every foliage altitude band, which are metres above the tile floor: the
sea is at 24.

## 4. The coast is shaped, not left over

`shaping.apply_beaches` pulls ground within a band of the waterline onto an equilibrium profile: a
7 degree foreshore, a berm at 1.6 m, and a 5 to 10 degree nearshore bed. `apply_rock_platform`
cuts a platform of uneven width where the coast is rock. The band's inland reach is per island:
Atoll's cays are only 25 m wide, and the default 18 m reach flattened them to the berm height
(2.1 m), so Atoll uses 7 m.

Measured on the shipped maps, the median slope within 1.5 m above the sea is 6.6 to 9.2 degrees on
every island, cliffs included, because the median is taken over the whole shore band.

## 5. Albedo is physical

The old layers were solid colours picked as display colours: "Scrub" `0.26 / 0.42 / 0.19` and sand
at `0.8-0.92`. The terrain shader consumes `BaseColor` as linear albedo, so Scrub displayed as
sRGB 140/173/120, a mint green, and the sand was brighter than fresh snow. Under the scene's
3.2-intensity sun the islands read as pastel: the mean HSV saturation of the island region at clear
noon was 39 of 255.

`materials.PALETTES` are linear reflectances. `GroundAlbedoIsPhysicalAndNoTwoIslandsShareAPalette`
bounds each island's mean land luminance to 0.03-0.55 (wet basalt to dry coral sand) and requires
every pair of islands to differ, which the copied palettes of the old scene did not (Dunes used
Atoll's four layers verbatim, "Coral rock" on a dune).

## 6. One layer library, six islands

Eight CC0 scans from Poly Haven (`layers.py`, credits in `tools/drift-islands/README.md`) serve all
six islands. Each albedo is rescaled so its mean linear colour is a fixed neutral
(`materials.NEUTRAL`); the macro map then carries `target colour / neutral` per texel. The macro
multiplier's ceiling is 255/128, about two, which is why the neutrals are chosen near the middle
of what the islands need: a white coral sand cannot be reached from a brown one.

The scans download on demand into `.island-source/` (git-ignored) and are checked against the MD5
in `sources.json`. Only the processed 1024 px layers are committed.

## 7. An unclaimed texel is eight textures averaged

The terrain shader normalises the splat weights through its height blend. Where the splatmaps
hold no weight at all, the blend gives every layer a share, and the ground there is the average
of all eight textures. The first bake left 494,645 such texels on Dunes (the seabed deeper than
3.5 m, which no sand rule claimed) and 7 on Atoll. They were under water and nobody saw them; the
map test found them as a weight sum of 0.

`materials._norm` now raises on a texel with no weight instead of dividing by one.

## 8. One orientation for four maps

Image row 0 is the tile's z = 0 edge and column 0 its x = 0 edge, for the height map, both
splatmaps and the macro map. Before #1721 the engine's file path flipped a splatmap vertically and
did not flip a height map, so a splatmap authored beside a height map came out mirrored in z. No
scene had ever loaded a splatmap from a file, so nothing showed it.

## 9. Tiling is repeats across the tile

`TilingScale` used to mean repeats across the tile on flat ground and repeats per metre on a slope
steep enough for the triplanar path: a layer that repeated 40 times across a 200 m tile repeated
8000 times on the slope beside it. It now means repeats across the tile everywhere, so the stamper
writes `tile size / metres per repeat` (`layers.LIBRARY` holds the metres).

## 10. The landfall radius

`DriftWeatherDirector.lua` arms a leg 45 m outside each island's coast and reads the coast off the
terrain component: `coast = (ShapingIslandFalloffRadius + 0.5) * 0.5 * WorldSizeX`. That formula
estimated where a radial mask's waterline fell. A height map applies no mask, so the stamper
measures the coast (the 99.5th percentile of the land's distance from the tile centre, so one
awash rock does not set it) and stores the value that makes the formula return it. The component
field carries nothing else on a height-map terrain.

## 11. Landmark sites

The harbour crane's landing (Ridgeback) and Stacks Light (Stacks) were placed on whatever the
noise left, then surveyed with Jolt rays: the landing's deck meets ground at 1.31-1.49 m with
3.21 m of water under the hook; the lighthouse's stair foot was meant to meet rock at 28.7 m and
stood on 30.95 m. `recipes.ridgeback_sites` and `recipes.stacks_sites` restate those surveys as
ground the new islands must have, applied after erosion so nothing wears them away.

The landing is stamped with `max(height, apron)` carried inland until it meets the island's own
slope. A first version stamped a fixed rectangle, and the warped coast left it standing in open
water.

## 12. A height-map terrain off the GPU

Two defects sat on the path no scene had used:

- `Scene.cpp` handed `TerrainComponent::m_HeightmapPath` to the loader as stored. A
  project-relative `Assets/...` path was read against the working directory and not found.
- The decode lived inside `TerrainData::LoadFromFile`, which uploads. A dedicated server or a
  functional test never built CPU heights for a height-map terrain, so it had no collision body:
  `Scene::CpuTerrainHeights` returned false and the builder logged a warning and skipped.

`TerrainData::DecodeHeightmapFile` is the decode alone. The GPU height map is also clamp-to-edge
now: under repeat, the filter at u = 0 took half its weight from the u = 1 edge, which grew a 12 m
wall along the low edge of a test terrain whose edges differed.

## 13. Terrain shadow casters follow the main view's detail

Drift's sun cast shadows to 35 m, so from the boat no island was ever shadowed. Raising the range
showed where terrain shadows cost. Measured in the editor on OpenGL at 1920x1080, six 1024 px
islands, clear morning, with the pass's own `Csm/TerrainCasters` GPU brackets:

| Sun shadow range | Terrain casters before | after |
|---|---|---|
| 35 m | 0.2-1.3 ms | 0.1-0.4 ms |
| 300 m | 4.3-8.5 ms | 0.4-0.9 ms |
| 600 m | 9-15 ms | 0.4-1.1 ms |

Three things were wrong, and the order they were found in is the lesson.

1. **The fixed tessellation.** The shadow submit drew every chunk at a tessellation factor of 4.
   A chunk mesh already has a vertex on every height texel, so that was sixteen times the
   triangles for no added shape. Dropping it to 1 barely moved the time (5.8-17 ms to 4-15 ms),
   which said the cost was not triangles.
2. **The level of detail.** A whole-tile proxy picked by the cascade's texel size was tried and
   never selected: the cascades are 4096 px, so even the last one resolves 0.29 m at a 600 m
   range, finer than the chunks' 0.41 m. Texel size was the wrong yardstick. What bounds useful
   caster detail is the receiver: the main view tessellates terrain to `TargetTriangleSize`
   pixels, so at 118 m its triangles are already 0.76 m long. `TerrainShadowLod::Select` picks,
   per chunk, the coarsest of four index sets (strides of 1, 2, 4 and 8 quads over the same vertex
   buffer) whose quads are no larger than the main view's triangles at the chunk's nearest
   point. That took a fifth off, no more.
3. **The per-draw state.** Each of a terrain's 256 chunks re-uploaded the terrain's 2.3 KB uniform
   block, its instance record and its heap offsets, all identical. Sending them when they change
   is what took 9-15 ms to about 1 ms. A GPU bracket round a draw-call-heavy loop measures the
   submission as much as the drawing.

The levels are not applied under the virtual shadow map, whose cached pages would keep the level
they were drawn at as the camera closed in, nor under displacing snow, which is finer than the
height field.

## 14. The quadtree split on the wrong size

`TerrainQuadtree::CalculateScreenSpaceError` (and its transcription in `TerrainNodeSelect.comp`)
projected a node's whole world extent to pixels and compared that against `TargetTriangleSize`.
A node is a 64-quad grid, so a node "under 6 pixels" has quads a tenth of a pixel long: every
node split until the tree ran out of levels. Every terrain in view was drawn at its deepest level
at every distance, and the authored target did nothing: 6, 10 and 16 px all measured the same
8.8 ms of depth prepass plus scene pass near a 1024 px island, against 5.4 ms for the 512 px island
it replaced. Cost followed the square of the height map's resolution.

The scale had a second fault. It was `viewProjection[1][1] * viewportHeight / 2`. That element is
the projection's `cot(fovY/2)` times the camera's up axis along world Y, so it shrank as the
camera pitched and was zero looking straight down, where the terrain collapsed to its root node
(the top-down captures were the cheapest frames measured, for this reason). Under Vulkan's flipped
Y it is negative, and a negative error is below every threshold.

Nothing looked wrong, which is why it lasted: over-refined terrain is correct terrain.
`TerrainGPUQuadtree.GpuDescentSelectsTheSameNodesAsTheCpuQuadtree` held the two descents to the
same answer, and they agreed.

`TerrainLodMetric::QuadProjectionScale` is now the one scale: the length of the view-projection's
second row, divided by the node's quads per edge. With it, near Ridgeback at a 6 px target the
depth prepass went 4.3 to 2.2 ms and the scene pass 4.5 to 2.4 ms.

## 15. Fog was computed to the seabed

Each island stood on a straight-edged patch of differently coloured water, strongest in the storm.
The first explanation, a seabed layer paler than the seafloor plane, was wrong: the colours were
matched and the patch stayed. What found it was capturing the frame between passes
(`olo_render_capture_target`): after the water pass the sea was uniform; after the fog pass it was
not.

Water is drawn blended and writes no scene depth. The fog pass rebuilds each pixel's world
position from scene depth, so over the sea it rebuilt the seabed and fogged that. Inside a terrain
tile the floor is near under the surface and the fog was light; outside, the ray ran on to the far
plane and the fog was at its limit. The edge of the patch was the edge of the tile.

`WaterRenderPass` already captures the surface's own depth for the underwater fog. The fog pass
binds it and `include/FogSurfaceDepth.glsl` takes the nearer of the two depths, in the fog shader
and in its upsample. When no water was drawn the pass binds a white texture (depth 1.0), never the
null resource, whose samples read 0 and would put the whole frame at the near plane.

## 16. Lighting that does not know the level of detail

Once the quadtree drew terrain at its authored triangle size, the relief lighting softened with
distance: the terrain's normal came from the evaluation stage, from height differences at the mip
matching the vertex spacing, interpolated across each triangle. It would also have stepped as a
node changed level.

The fragment stage now reads the height map itself (`include/TerrainPixelNormal.glsl`), at a
fractional mip matching the pixel's footprint, with the differences taken one texel of that mip
apart. The instance's normal matrix reaches it as three flat varyings rather than a block, because
the terrain fragment stage is at the driver's limit of uniform blocks on the bindless route.

## 17. Binary maps in history

The six islands' maps are 8 MiB and the layer library 31 MiB, all plain blobs (not LFS). An art
pass regenerates them many times. Iterate with the maps uncommitted and commit the final set with
the scene that names them.
