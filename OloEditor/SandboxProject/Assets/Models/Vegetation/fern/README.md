# fern - imported vegetation

Free 8K model of four realistic fern clumps featuring detailed serrated fronds, varied silhouettes and rich dappled green foliage.

Licence and full provenance: [LICENSE.md](LICENSE.md).

## What is here

| file | what it is |
|---|---|
| `fern.obj` | the plant, 2,384 triangles |
| `fern.mtl` | one material per submesh, so each gets its own albedo |
| `Textures/` | albedo maps; the foliage one carries the alpha cutout |

## Numbers that matter

- **total triangles**: 2,384 (from 2,384 scanned)
- **committed size**: 1.05 MB
- **real-world height**: 0.43 m
- **fronds triangles**: 2,384 (from 2,384)
- **fronds coverage at cutoff 0.5**: 52.5%
- **flat-card coverage**: 28.7%

`Coverage at the authored cutoff` is the number issue #1398 asked to be chosen
deliberately rather than inherited: the fraction of the albedo that survives the
alpha test, measured over the texels the mesh actually samples.
