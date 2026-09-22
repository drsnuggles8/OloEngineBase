# broadleaf - imported vegetation

Free 8K tree model with twisted gnarled trunk, high-detail bark, exposed roots and a dense, wind-sculpted coastal canopy of small green leaves.

Licence and full provenance: [LICENSE.md](LICENSE.md).

## What is here

| file | what it is |
|---|---|
| `broadleaf.obj` | the plant, 9,999 triangles |
| `broadleaf.mtl` | one material per submesh, so each gets its own albedo |
| `Textures/` | albedo maps; the foliage one carries the alpha cutout |

## Numbers that matter

- **total triangles**: 9,999 (from 1,599,403 scanned)
- **committed size**: 3.73 MB
- **real-world height**: 5.03 m
- **trunk triangles**: 5,147 (from 34,787)
- **trunk coverage at cutoff 0.5**: 100.0%
- **foliage triangles**: 4,852 (from 1,564,616)
- **foliage coverage at cutoff 0.5**: 44.8%
- **flat-card coverage**: 40.5%

`Coverage at the authored cutoff` is the number issue #1398 asked to be chosen
deliberately rather than inherited: the fraction of the albedo that survives the
alpha test, measured over the texels the mesh actually samples.
