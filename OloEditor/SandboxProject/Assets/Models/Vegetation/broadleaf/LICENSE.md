# Island Tree 01 - licence and provenance

Source: **Poly Haven** - <https://polyhaven.com/a/island_tree_01>

Licence: **CC0 1.0 Universal (public domain dedication)** - <https://polyhaven.com/license>

CC0 places the work in the public domain: commercial use, redistribution and
modification are all permitted and **attribution is not required**. It is recorded here
anyway, because Poly Haven asks for it as a courtesy and because a provenance trail
costs nothing to keep.

Original author(s): **Rob Tuytel (scanning, processing), Rico Cilliers (cleanup, processing)**

Imported 2026-09-22 at the 1k texture tier by
`tools/vegetation-import/import_vegetation.py`.

## Downloaded from

- <https://dl.polyhaven.org/file/ph-assets/Models/gltf/1k/island_tree_01/island_tree_01_1k.gltf>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_nor_gl_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_diff_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_arm_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_leaves_nor_gl_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_leaves_diff_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_leaves_arm_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_branches_nor_gl_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_branches_diff_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/jpg/1k/island_tree_01/island_tree_01_branches_arm_1k.jpg>
- <https://dl.polyhaven.org/file/ph-assets/Models/gltf/8k/island_tree_01/island_tree_01.bin>
- <https://dl.polyhaven.org/file/ph-assets/Models/png/1k/island_tree_01/island_tree_01_diff_1k.png>
- <https://dl.polyhaven.org/file/ph-assets/Models/png/1k/island_tree_01/island_tree_01_branches_diff_1k.png>
- <https://dl.polyhaven.org/file/ph-assets/Models/png/1k/island_tree_01/island_tree_01_leaves_alpha_1k.png>
- <https://dl.polyhaven.org/file/ph-assets/Models/png/1k/island_tree_01/island_tree_01_leaves_diff_1k.png>

## Modifications made to the original

Everything in this directory is a **derivative** of the files above, not a copy of
them. The scan is hundreds of megabytes and millions of triangles; what is committed
here is a game-ready plant built from it.

- `trunk`: decimated from 34,787 to 5,147 triangles by grid vertex clustering, seam-preserving (see tools/vegetation-import/olo_veg/decimate.py).
- `foliage`: the scanned canopy (1,564,616 triangles of individually modelled foliage) was clustered into 1,213 groups and rebuilt as cross-cards (4,852 triangles). The card atlas was baked from the scan itself by orthographic projection of each cluster, so its transparency is that cluster's real geometry coverage.
- Baked `Textures/broadleaf_card.png`, the flat billboard the layer draws beyond MeshViewDistance, by projecting the finished plant over the card quad's own frame.
- Recentred on the trunk and rescaled to the foliage authoring frame (base at the origin, unit height). The plant is **5.03 m** tall in the original, which is the figure a layer's MinHeight/MaxHeight should reflect.
- Textures rebuilt as RGBA png from the source diffuse plus its separate alpha map (the glTF package ships jpg, which cannot carry a cutout).

Re-run the import to reproduce this directory exactly:

```
python tools/vegetation-import/import_vegetation.py broadleaf
```
