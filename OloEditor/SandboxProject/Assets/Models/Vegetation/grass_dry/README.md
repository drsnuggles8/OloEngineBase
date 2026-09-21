# grass_dry - imported vegetation

Free 4K model of a medium grass tuft - dense green-brown blades, varied strands and geometry-nodes driven scatter system for natural ground cover.

Licence and full provenance: [LICENSE.md](LICENSE.md).

## What is here

| file | what it is |
|---|---|
| `grass_dry.obj` | the plant, 1,215 triangles |
| `grass_dry.mtl` | one material per submesh, so each gets its own albedo |
| `Textures/` | albedo maps; the foliage one carries the alpha cutout |

## Numbers that matter

- **total triangles**: 1,215 (from 1,215 scanned)
- **committed size**: 0.52 MB
- **real-world height**: 0.23 m
- **blades triangles**: 1,215 (from 1,215)
- **blades coverage at cutoff 0.5**: 87.1%
- **flat-card coverage**: 6.1%

`Coverage at the authored cutoff` is the number issue #1398 asked to be chosen
deliberately rather than inherited: the fraction of the albedo that survives the
alpha test, measured over the texels the mesh actually samples.
