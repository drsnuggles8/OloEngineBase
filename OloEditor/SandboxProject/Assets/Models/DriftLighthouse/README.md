# Stacks Light

A rock lighthouse on the western approach to Stacks in Drift. Its forked oxide-red
daymark echoes the split basalt stacks. The keeper enters through a boarded oak
door beneath a leaded fanlight. A copper conductor drains the gallery, and the
worn granite stair crosses a masonry arch to the rock.

## Surface construction

The tower carries the outline of the daymark in its mesh. Limewash, red paint
and exposed foundation share tangent-space relief from the same 3 m masonry
scan. At 2,048 texels per tile the wall has about 683 texels per vertical metre.
Successive lifts shift horizontally at a mortar bed. Separate finishes shape
paint loss on split faces, salt on the plinth, and runoff beneath the gallery
knees. The foundation courses have individual chipped edges and recessed mortar.

Wood grain follows the individual door planks. Tread roughness changes across
the walking line, and enamel letters stand proud of the bronze-rimmed station
plate. The twelve copper roof panels have UVs measured along their sloping
surface. Four panel patterns place verdigris against the folded seams and lower
hem. Inside the lantern, open bronze hoops and narrow glass prisms surround
the luminous core. The gallery floor has raised cast lozenges.

The glTF contains 82,975 triangles in 18 material primitives. The 27 shared
textures comprise eight 2048², thirteen 1024² and six 512² images. The packed
data maps store occlusion, roughness and metallic in R, G and B. The engine
generates tangents from the exported normals and UVs. The scene adds the
emissive sphere and point light.

## Rebuild

Use Blender 5.3.0 (build `02f0899d78e3`). From the repository root:

```powershell
$model = 'OloEditor/SandboxProject/Assets/Models/DriftLighthouse'
blender -b --factory-startup --python-exit-code 1 --python "$model/build_lighthouse.py" -- "$model"
```

`build_lighthouse.py` makes the geometry. Its sibling `lighthouse_surfaces.py`
bakes the material finishes with Blender's bundled NumPy and the images in
`SurfaceSources.zip`. A rebuild works offline. To compare a clean generation,
pass a different output directory after `--`; the generator reads its inputs
beside the script. The generator bakes into a temporary directory and checks the
UV charts, buffer and each finish's colour, normal and packed map references.
`lighthouse_package.py` then replaces changed payloads and publishes the glTF
last. A failed replacement restores the previous files. The destination-local
`.pending` and `.previous` files have unregistered extensions, so staging also
works when the system temporary directory is on another volume. If none of the
normal temporary locations is writable outside the watched project, set `TMPDIR`
to a writable directory outside it.

**Interrupted publication.** The error names the output directory's
`.stacks-light-recovery` folder. First verify that no Blender process is still
publishing to that destination. Keep a copy of the recovery folder. If it has
no `transaction.plan`, no unfinished replacement needs rollback. Otherwise, copy each file
listed under `existing` from its `.previous` backup to its original name, then
remove only the live files listed under `added`. For an interrupted build into
an empty directory, `existing` is empty. Once the original state is restored,
move the recovery folder outside the project and retry the rebuild. From the
repository root, `python -m unittest discover -s "$model" -p test_lighthouse_package.py`
exercises interrupted publication and rollback without launching Blender.

Keep the glTF, binary buffer and its 27 referenced PNGs
together for delivery. Reopen or reimport the model and textures before comparing
frames.

## Scan credits

The architecture, optic, UV charts and finish recipes were authored for Drift.
The archive contains the original diffuse, OpenGL normal and packed material
JPEGs from four [CC0 Poly Haven scans](https://polyhaven.com/license).

| Source | Artist | Resolution used |
| --- | --- | --- |
| [Sandstone Blocks 05](https://polyhaven.com/a/sandstone_blocks_05) | Rob Tuytel | 2048² |
| [Granite Tile](https://polyhaven.com/a/granite_tile) | Charlotte Baglioni | 1024² |
| [Weathered Brown Planks](https://polyhaven.com/a/weathered_brown_planks) | Dimitrios Savva (photography), Rico Cilliers (processing) | 2048² |
| [Rusty Metal 03](https://polyhaven.com/a/rusty_metal_03) | Amal Kumar | 1024² |

## Placement

`Stacks Light` and `Stacks Light - optic` are two roots in `Scenes/Drift.olo`.
The tower stands at **(−419, 29.9, −465)** on the western shelf, with **15°**
of yaw. The bottom tread meets the rock near (−417.74, 28.7, −469.71): the
stack's top is flat at 28.68–28.70 m across the tower's footprint (Jolt rays),
because it is a site in Stacks' recipe (`stacks_sites` in
`tools/drift-islands/olo_islands/recipes.py`). Survey it again after changing
that island.
The optic sits 20.30 m above the tower origin. When moving the tower, update
the optic's world position as well. One glTF unit is one metre, Y is up, the
finial rises 24.12 m above the entry landing and the pier extends 14 m below it.
