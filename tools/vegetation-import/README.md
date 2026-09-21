# vegetation-import — CC0 scans to committed vegetation art

Rebuild every plant under `OloEditor/SandboxProject/Assets/Models/Vegetation/` from its Poly Haven
source. Edit a budget in [recipes.json](recipes.json) and re-run; the committed asset is an output,
not something anyone hand-edits.

```bash
python tools/vegetation-import/import_vegetation.py            # every species
python tools/vegetation-import/import_vegetation.py pine       # one
python tools/vegetation-import/import_vegetation.py --list

python tools/vegetation-import/preview.py --all                # look at the result
python tools/vegetation-import/measure_coverage.py --before-after
```

Needs `numpy` and `Pillow`, nothing else. Sources download on demand into
`.../Vegetation/_raw/` (git-ignored); `scripts/Fetch-Assets.ps1 -Tag vegetation` pre-fetches the two
heavy hero scans with checksums instead.

## Why a tool and not a download

Ground cover imports almost directly. Trees do not, and the reason is worth knowing before changing
anything here.

A Poly Haven hero conifer is **17.4 million triangles in a 949 MB buffer**. Two properties make that
un-importable by any simple route:

- **The mesh does not shrink with the texture tier.** "1k", "2k" and "4k" select TEXTURES; the
  buffer is byte-identical at every tier. The API says so out loud — the *1k* package's include list
  points at the **8k** directory's `.bin`.
- **The canopy cannot be decimated.** Those triangles are several hundred thousand individually
  modelled needles, and 91.7% of that surface passes its own alpha test, because the alpha map only
  trims a needle EDGE. Collapse a thin needle strip and you get a sliver, not a needle.

So the two halves of a tree get opposite treatments, and that split is the whole design:

| part | mode | what happens |
|---|---|---|
| trunk, bark | `solid` | grid vertex-cluster decimation to a triangle budget ([decimate.py](olo_veg/decimate.py)) |
| canopy | `foliage` | rebuilt as cross-cards, textured from an atlas **baked out of the scan** ([cards.py](olo_veg/cards.py)) |
| ground cover | `direct` | taken as scanned; blades and fronds are already the right size |

### The bake is the point

A foliage card's texture is made by projecting a cluster of real needles onto its own dominant plane
and recording what is there ([raster.py](olo_veg/raster.py)). The transparency in the result is real
geometry coverage — a texel is clear because the scan has no surface at that point.

That is the difference between this and the defect [#1398](https://github.com/drsnuggles8/OloEngineBase/issues/1398)
describes, where one 512×512 grass cutout passing 17.9% of its texels textured every species
including the pines.

## Two traps that cost real time here

**glTF UV origin is the TOP-LEFT, v increasing downward.** OBJ's is the bottom-left. Everything in
this package works in glTF convention and flips exactly once, in
[wavefront.py](olo_veg/wavefront.py). Getting it wrong is not a subtle shading difference:
`pine_tree_01`'s twig atlas has needles in the lower half and pine cones in the upper, so a flipped
v bakes brown cones onto every card and the result looks like a plausible dead tree.

**Measure the surface, not the sheet.** Averaging alpha over a whole texture answers a question
nobody asked. A UV atlas has unused space, and on a scanned plant that space is *opaque*:
`pine_tree_01`'s twig sheet reads 69.5% flat, while the canopy that samples it reads 54.4%. Only the
second number describes anything a viewer sees, so `measure_coverage.py` samples over the mesh's own
triangles, area-weighted.

## What a species directory contains

```
pine/
  pine.obj              one usemtl group per submesh — that IS the per-submesh albedo
  pine.mtl              map_Kd per material; FoliageRenderer reads it via GetAlbedoMap()
  Textures/
    pine_trunk.png      opaque bark (RGB)
    pine_bark.png
    pine_foliage.png    the baked canopy atlas (RGBA, real cutout)
    pine_card.png       the billboard the far LOD rung draws, baked from this plant
  LICENSE.md            source, author, exact URLs, and EVERY modification made
  README.md             triangle counts, committed size, coverage numbers
```

Meshes are normalized to the foliage authoring frame FoliageRenderer assumes — **base at the origin,
unit height**. It rescales nothing and only *warns* when a mesh is not, so
`VegetationAssetContractTest` asserts it instead.

`AlbedoPath` on a layer is the **card** texture, never a source atlas. The far rung draws one quad;
pointing it at a scanned plant's UV sheet puts cones and bark swatches on the billboard, which is
the same class of mistake as the defect being fixed.

## Adding a species

1. Find a CC0 plant at <https://polyhaven.com/models> (`https://api.polyhaven.com/files/<id>` lists
   its maps and the real file sizes).
2. Add a recipe. The material names come from the source glTF; print them with:
   ```bash
   python -c "import sys; sys.path.insert(0,'tools/vegetation-import'); from olo_veg.gltf import Gltf; \
   g=Gltf('OloEditor/SandboxProject/Assets/Models/Vegetation/_raw/<id>/<id>_1k.gltf'); \
   print([m.get('name') for m in g.doc['materials']])"
   ```
3. Run the import, then `preview.py <name>` and **look at it**. A coverage percentage cannot tell
   you whether a tree reads as a tree; that is how #1398 happened.
4. If a scene uses it, re-run `python tools/benchmark/reference_assets.py --write-hashes` so the
   benchmark manifests' provenance records match the bytes.

## Licence

Poly Haven is **CC0 1.0** (<https://polyhaven.com/license>): commercial use, redistribution and
modification all permitted, attribution *not* required. Attribution is recorded per asset anyway —
it costs nothing and keeps the audit trail. Because modification and redistribution are permitted,
committing the heavily-derived result is the intended path, not a workaround.
