# flower - imported vegetation

Free 8K model: dense celandine groundcover with glossy heart-shaped green leaves and scattered small yellow flowers, detailed natural meadow foliage. Geometry nodes scatter system.

Licence and full provenance: [LICENSE.md](LICENSE.md).

## What is here

| file | what it is |
|---|---|
| `flower.obj` | the plant, 4,301 triangles |
| `flower.mtl` | one material per submesh, so each gets its own albedo |
| `Textures/` | albedo maps; the foliage one carries the alpha cutout |

## Numbers that matter

- **total triangles**: 4,301 (from 4,301 scanned)
- **committed size**: 1.38 MB
- **real-world height**: 0.18 m
- **blooms triangles**: 4,301 (from 4,301)
- **blooms coverage at cutoff 0.5**: 91.1%
- **flat-card coverage**: 10.4%

`Coverage at the authored cutoff` is the number issue #1398 asked to be chosen
deliberately rather than inherited: the fraction of the albedo that survives the
alpha test, measured over the texels the mesh actually samples.
