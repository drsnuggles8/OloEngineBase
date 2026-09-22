# grass - imported vegetation

Free 8K grass model: dense medium tuft with varied green blades, natural meadow tones and wispy outer fronds for realistic ground cover. Geometry nodes scatter system.

Licence and full provenance: [LICENSE.md](LICENSE.md).

## What is here

| file | what it is |
|---|---|
| `grass.obj` | the plant, 1,257 triangles |
| `grass.mtl` | one material per submesh, so each gets its own albedo |
| `Textures/` | albedo maps; the foliage one carries the alpha cutout |

## Numbers that matter

- **total triangles**: 1,257 (from 1,257 scanned)
- **committed size**: 1.07 MB
- **real-world height**: 0.18 m
- **blades triangles**: 1,257 (from 1,257)
- **blades coverage at cutoff 0.5**: 92.2%
- **flat-card coverage**: 12.0%

`Coverage at the authored cutoff` is the number issue #1398 asked to be chosen
deliberately rather than inherited: the fraction of the albedo that survives the
alpha test, measured over the texels the mesh actually samples.
