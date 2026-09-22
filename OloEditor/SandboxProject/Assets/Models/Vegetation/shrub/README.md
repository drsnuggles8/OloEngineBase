# shrub - imported vegetation

Free 4K shrub model with delicate branching stems and narrow lanceolate leaves, realistic low-growing groundcover with subtle color variation and fine detail.

Licence and full provenance: [LICENSE.md](LICENSE.md).

## What is here

| file | what it is |
|---|---|
| `shrub.obj` | the plant, 5,242 triangles |
| `shrub.mtl` | one material per submesh, so each gets its own albedo |
| `Textures/` | albedo maps; the foliage one carries the alpha cutout |

## Numbers that matter

- **total triangles**: 5,242 (from 5,242 scanned)
- **committed size**: 1.30 MB
- **real-world height**: 1.19 m
- **foliage triangles**: 5,242 (from 5,242)
- **foliage coverage at cutoff 0.5**: 92.7%
- **flat-card coverage**: 10.6%

`Coverage at the authored cutoff` is the number issue #1398 asked to be chosen
deliberately rather than inherited: the fraction of the albedo that survives the
alpha test, measured over the texels the mesh actually samples.
