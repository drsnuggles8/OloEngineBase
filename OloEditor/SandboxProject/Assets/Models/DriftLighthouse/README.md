# Stacks Light

A rock light for Drift. Two descending points in the oxide-red daymark echo the
split basalt stacks. A narrow red band directly below the gallery stays visible
on a grazing approach. On the keeper's side, eight treads descend from the
boarded door across a masonry arch to the rock. A shallow hollow runs down the
middle of their nosings where boots have worn the stone. The stair windows rise around
the tower as they would along an internal spiral.

The foundation and tower share a continuous cylindrical UV chart. Dark granite
gives way to limewashed ashlar above the door sill. Staggered joints are deeper
on exposed stone, salt collects near the plinth, and iron runoff starts at the
gallery. Its copper lantern has twelve glazed bays, storm braces, a standing-seam
hood and a capped ventilator. A luminous optic and a warm point light share a
scene entity inside the lantern. Dusk uses Drift's own atmosphere.

## Rebuild

From the repository root, using Blender 5.3.0 (build `02f0899d78e3`):

```powershell
$model = 'OloEditor/SandboxProject/Assets/Models/DriftLighthouse'
blender -b --factory-startup --python-exit-code 1 --python "$model/build_lighthouse.py" -- "$model"
```

Use the full path to the installed Blender executable if it is outside PATH.
The script uses Blender's bundled NumPy and a fixed seed (5501). It writes
`StacksLight.gltf`, `StacksLight.bin`, and two 2048 × 2048 RGB maps,
`StacksMasonry.png` (sRGB base colour) and `StacksMasonryNormal.png` (linear
tangent-space normals). Keep the four files together. After regeneration,
reopen the project or reimport the textures and mesh before judging a frame.
The generator, geometry and textures are original to this repository. No
external models, textures or reference images were used.

## Placement

`Stacks Light` and `Stacks Light - optic` are two roots in `Scenes/Drift.olo`.
The tower is at **(−419, 29.9, −465)** on the outer western stack, with 15°
of yaw. The bottom tread meets the shelf near (−417.74, 28.7, −469.71).
When moving the tower, place the optic 20.30 m directly above its origin. This
gives the renderer the light's world position. One glTF unit is one metre, Y is
up and the tower origin is the upper landing. The finial rises 24.12 m above
it and the footing extends 14 m below it. The imported mesh has 20,492
triangles across eight material primitives.
The scene adds a sphere for the luminous optic.
