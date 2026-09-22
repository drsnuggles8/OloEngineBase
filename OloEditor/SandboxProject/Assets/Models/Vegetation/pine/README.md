# pine - imported vegetation

Free 8K model of a tall, slender pine with layered branch clusters, dense needle foliage, and a realistic textured trunk and bark.

Licence and full provenance: [LICENSE.md](LICENSE.md).

## What is here

| file | what it is |
|---|---|
| `pine.obj` | the plant, 25,146 triangles |
| `pine.mtl` | one material per submesh, so each gets its own albedo |
| `Textures/` | albedo maps; the foliage one carries the alpha cutout |

## Numbers that matter

- **total triangles**: 25,146 (from 6,946,864 scanned)
- **committed size**: 5.95 MB
- **real-world height**: 20.37 m
- **trunk triangles**: 4,828 (from 84,332)
- **trunk coverage at cutoff 0.5**: 100.0%
- **bark triangles**: 13,770 (from 23,710)
- **bark coverage at cutoff 0.5**: 100.0%
- **foliage triangles**: 6,548 (from 6,838,822)
- **foliage coverage at cutoff 0.5**: 54.4%
- **flat-card coverage**: 11.1%

`Coverage at the authored cutoff` is the number issue #1398 asked to be chosen
deliberately rather than inherited: the fraction of the albedo that survives the
alpha test, measured over the texels the mesh actually samples.
