# Generated island height maps: an island is a landform, not a mask over noise

Applies to: `tools/drift-islands/**`, the six `Island - *` entities in `OloEditor/SandboxProject/Assets/Scenes/Drift.olo`, `OloEditor/SandboxProject/Assets/Terrain/DriftIslands/**`, and any terrain that is a height map with textured layers. From issue #1721 (the *Drift* island art review).

## Rules

1. **Change an island by changing its recipe and re-running `python tools/drift-islands/build_islands.py --stamp-scene`.** The maps, each island's entity block in `Drift.olo` and the scene header's LAYOUT section are outputs; an edit made by hand is lost on the next run. [Details](generated-island-height-maps-details.md#1-the-scene-block-is-derived-data)
2. **Shape an island as a landform in metres, then derive erosion, shore and materials from it; do not tune the runtime generator's knobs.** One radial mask over uniform noise gave six discs whose coast was a 45-60 degree ramp, whatever the seed. [Details](generated-island-height-maps-details.md#2-what-the-radial-mask-could-not-make)
3. **Drive every border texel of a height map to exactly the tile floor, and keep the summit under the height scale.** `DriftIslandSplatCoverage.TheTileBorderIsTheSeabedFloorAndTheSummitFits` pins both on the shipped maps; the tool refuses a peak that does not fit. [Details](generated-island-height-maps-details.md#3-border-and-summit)
4. **Pull a sand coast to a beach profile and give a rock coast a platform; check the slope at the waterline as a number.** A real foreshore is 5-10 degrees. `TheCoastIsAShoreNotARamp` bounds the median slope within 1.5 m above the sea. [Details](generated-island-height-maps-details.md#4-the-coast-is-shaped-not-left-over)
5. **State ground albedo as linear reflectance, not as a display colour.** Grass reflects about a tenth of the light, wet basalt a twentieth, dry sand two fifths. A value picked from a colour picker is consumed as linear and renders pastel. [Details](generated-island-height-maps-details.md#5-albedo-is-physical)
6. **Put colour that is bigger than a texture tile in the macro map, and keep the shared layer textures neutral.** Each layer's albedo mean is normalised; the island's rock and sand colour, tide band, lichen, bedding and damp gullies are macro texels. [Details](generated-island-height-maps-details.md#6-one-layer-library-six-islands)
7. **Give every splat texel a whole of layer weight.** A texel no layer claims is not empty: the height blend gives all eight layers a share there. The bake raises on one, and `EveryTexelIsClaimedAndMoreThanOneMaterialShows` checks the shipped maps. [Details](generated-island-height-maps-details.md#7-an-unclaimed-texel-is-eight-textures-averaged)
8. **Write all four maps with image row 0 on the tile's z = 0 edge.** The engine reads the height map, both splatmaps and the macro map unflipped. [Details](generated-island-height-maps-details.md#8-one-orientation-for-four-maps)
9. **Set a layer's `TilingScale` to the tile size divided by the metres one repeat should cover.** It is repeats across the tile on the planar path, on all three triplanar projections and on voxel terrain. [Details](generated-island-height-maps-details.md#9-tiling-is-repeats-across-the-tile)
10. **Let the stamper write the coast radius; never type it.** `DriftWeatherDirector.lua` reads it back from `ShapingIslandFalloffRadius`, and `TheLandfallRingHoldsTheIsland` fails when a map's coast moved without a re-stamp. [Details](generated-island-height-maps-details.md#10-the-landfall-radius)
11. **Restate a landmark's ground as a site in the recipe, and re-survey it after the coast moves.** The crane landing and Stacks Light were built against Jolt ray surveys; their READMEs name the points. [Details](generated-island-height-maps-details.md#11-landmark-sites)
12. **Resolve a scene-stored terrain path with `ResolveContentPath`, and decode a height map on the CPU for a headless run.** `TerrainData::DecodeHeightmapFile` is the one decoder; `Scene::CpuTerrainHeights` uses it, so a server's collision and a client's surface are the same field. [Details](generated-island-height-maps-details.md#12-a-height-map-terrain-off-the-gpu)
13. **Cast a distant terrain's shadow from a mesh as coarse as the ground the main view draws there (`TerrainShadowLod`): no finer, and no coarser.** A chunk mesh has a vertex on every height texel, which a far cascade pays for and cannot show; a caster blunter than the drawn ground puts the shadow's edge in the wrong place. [Details](generated-island-height-maps-details.md#13-terrain-shadow-casters-follow-the-main-views-detail)
14. **Measure a terrain's level of detail as the on-screen size of one quad of a node, never of the node.** `TerrainLodMetric::QuadProjectionScale` is the one projection scale both descents use; it is the length of the view-projection's second row, not the element `[1][1]`, which carries the camera's pitch and Vulkan's sign. [Details](generated-island-height-maps-details.md#14-the-quadtree-split-on-the-wrong-size)
15. **A pass that rebuilds a position from scene depth is reading the seabed under water; take the nearer of it and the water surface depth.** Water writes no depth. `include/FogSurfaceDepth.glsl` is the fog's answer, and `WaterVisualEvidenceTest.TheFogOnTheSeaDoesNotDependOnTheSeabed` holds it. [Details](generated-island-height-maps-details.md#15-fog-was-computed-to-the-seabed)
16. **Light terrain by a per-pixel normal from the height map, not by the vertex normal.** The vertex normal's detail is the tessellation's, so lighting changes with the level a node is drawn at. `include/TerrainPixelNormal.glsl`. [Details](generated-island-height-maps-details.md#16-lighting-that-does-not-know-the-level-of-detail)
17. **Commit regenerated maps once per change, not once per iteration.** The six islands and the layer library are about 40 MiB of plain blobs; every committed iteration stays in history. [Details](generated-island-height-maps-details.md#17-binary-maps-in-history)
18. **Read a height map as samples and a splatmap or macro map as areas.** A height map's first and last texels sit ON the tile's edges, N - 1 intervals apart: fetch it in a shader through `oloTerrainHeightUV`, never by the bare tile UV, and write it from a grid of `i * size / (N - 1)`. A painted map's texel j is the j-th N-th of the tile, read at its centre. [Details](generated-island-height-maps-details.md#18-samples-and-areas)

## Commands and checks

```bash
python tools/drift-islands/build_islands.py --preview out/      # judge a recipe without the engine
python tools/drift-islands/build_islands.py --stamp-scene       # maps, then Drift.olo
python tools/drift-islands/build_islands.py --check             # regenerate and compare; writes nothing
build-cached/OloEngine/tests/Release/OloEngine-Tests.exe --gtest_filter=DriftIslandSplatCoverage.*
build-cached/OloEngine/tests/Release/OloEngine-Tests.exe --gtest_filter=*TerrainHeightSampleConvention*:*TerrainShadowFrom*
```

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[generated-island-height-maps-details.md](generated-island-height-maps-details.md).
