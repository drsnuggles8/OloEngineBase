# drift-islands: the six Drift islands, generated

The islands in `Scenes/Drift.olo` are outputs of this tool. Change a recipe and re-run; never
edit a map or an island's scene block by hand.

```bash
python tools/drift-islands/build_islands.py                  # all six: maps only
python tools/drift-islands/build_islands.py Stacks Mesa      # some
python tools/drift-islands/build_islands.py --stamp-scene    # maps, then the entities in Drift.olo
python tools/drift-islands/build_islands.py --only-stamp     # entities only, from the maps on disk
python tools/drift-islands/build_islands.py --layers         # also rebuild the shared layer textures
python tools/drift-islands/build_islands.py --preview out/   # also write preview PNGs
python tools/drift-islands/build_islands.py --check          # regenerate, compare, write nothing
```

Needs Python 3.10+ with the packages in [requirements.txt](requirements.txt) (numpy, scipy,
Pillow). An island takes about half a minute.

## What it writes

Under `OloEditor/SandboxProject/Assets/Terrain/DriftIslands/`:

| file | content |
|---|---|
| `<Island>_Height.png` | 16-bit grey, 1024 x 1024. 0 is the tile floor, 24 m under the sea; 65535 is floor + `HeightScale`. |
| `<Island>_Splat0.png` | RGBA8 weights of layers 0-3: Seabed, Sand, Shingle, Turf. |
| `<Island>_Splat1.png` | RGBA8 weights of layers 4-7: Heath, Rock, RockBedded, Scree. |
| `<Island>_Macro.png` | RGBA8 colour multiplier (128 = neutral) and dryness in alpha (255 = dry). |
| `Layers/<Layer>_{Color.jpg,Normal.jpg,ARM.png}` | The eight shared layers (`--layers`). ARM alpha is height. |

Image row 0 is the tile's z = 0 edge and column 0 its x = 0 edge, the order the engine reads a
height map in. `--stamp-scene` rewrites each island's entity in `Drift.olo`, from its header
comment to the end of its `FoliageComponent`: paths, tile size, height scale, layer tiling, the
discovery box, the landfall radius the weather director reads, and the foliage layers.

## How an island is made

1. **Landform** ([recipes.py](olo_islands/recipes.py)). One function per island draws its
   character in metres: where the spine runs, where a face stands, where sand lies.
2. **Erosion** ([erosion.py](olo_islands/erosion.py)). The droplet model the engine's own
   `TerrainGenerator::ApplyErosion` uses, plus a hardness map (a cliff resists, a dune does not),
   the sea (a droplet drops its load at the waterline), and a flow map. Then thermal slumping to
   the angle of repose on soft ground.
3. **Shore** ([shaping.py](olo_islands/shaping.py)). Sand coasts are pulled to an equilibrium
   beach profile (7 degree foreshore, 1.6 m berm); rock coasts get a wave-cut platform.
4. **Sites.** The landmarks' ground: the lighthouse's stack top and the landing's hard standing
   and berth. Each restates a Jolt ray survey of the terrain the landmark was built against.
5. **Border.** Every border texel is driven to the tile floor exactly, so the tile edge is seabed.
6. **Materials** ([materials.py](olo_islands/materials.py)). Splat weights from slope, height,
   shore distance, drainage and exposure; the macro map carries what is bigger than a texture
   tile: the island's own rock and sand colour, the tide band, lichen, guano, bedding, damp
   gullies and baked occlusion.

`preview.py` renders a shaded map and four views from the sea in software, so a recipe can be
judged without building the engine.

## Rules that are easy to break

- **A recipe's peak must fit its `HeightScale`** (`recipes.TILES`); the tool refuses otherwise.
  Changing a height scale changes every foliage altitude band, which are metres above the tile
  floor: sea level is 24.
- **Keep land at least 30 m inside the tile edge.** The border ramp lowers anything closer.
- **Moving a coast moves a landmark's ground.** After changing Ridgeback or Stacks, re-survey
  the sites (the crane and lighthouse READMEs name the points) and update those READMEs.
- **The landfall radius must fit `DriftWeatherDirector.lua`'s formula**, `(radius + 0.5) * 0.5 *
  size`: the stamper computes it from the island's farthest land and fails if it does not.
- **Albedo is physical.** `materials.PALETTES` are linear albedos: grass reflects about a tenth
  of the light, wet basalt a twentieth. A value chosen by eye from a colour picker is too bright
  and renders pastel.
- **Layer sources are not committed.** They download into `.island-source/` at the repo root
  and are verified against [sources.json](sources.json); `--refresh-sources` rewrites it.

## Layer credits

All eight scans are [CC0](https://polyhaven.com/license) from Poly Haven. Each albedo's mean is
normalised to `materials.NEUTRAL`, the maps are resampled to 1024 px, and the displacement is
packed into the ARM alpha.

| layer | scan | credit |
|---|---|---|
| Seabed | [Coast Sand 04](https://polyhaven.com/a/coast_sand_04) | Rob Tuytel (photography, processing), Dario Barresi (processing) |
| Sand | [Sandy Gravel 02](https://polyhaven.com/a/sandy_gravel_02) | Dario Barresi |
| Shingle | [Ganges River Pebbles](https://polyhaven.com/a/ganges_river_pebbles) | Amal Kumar |
| Turf | [Sparse Grass](https://polyhaven.com/a/sparse_grass) | Amal Kumar |
| Heath | [Brown Mud Leaves 01](https://polyhaven.com/a/brown_mud_leaves_01) | Rob Tuytel |
| Rock | [Rock Face 03](https://polyhaven.com/a/rock_face_03) | Dario Barresi (photography), Rico Cilliers (processing) |
| RockBedded | [Cliff Side](https://polyhaven.com/a/cliff_side) | Dario Barresi, James Ray Cock (photography), Jenelle van Heerden (processing) |
| Scree | [Gray Rocks](https://polyhaven.com/a/gray_rocks) | Dimitrios Savva |
