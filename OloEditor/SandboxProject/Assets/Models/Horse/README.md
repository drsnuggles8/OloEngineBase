# Horse: the reference furless animal body

This is the body the groom epic's acceptance coats are grown on (issue #1223). It is a realistic,
rigged, textured draft horse under CC0. The coat is not part of this asset: the fur comes from a
groom bound to this mesh. The body sets the silhouette under the coat, the surface the roots sit on,
and the motion.

## Why this model

Before this, the only rigged animal in the repository was the Khronos Fox: 576 flat-shaded triangles
with its fur painted into a 1K texture. A coat grown on it follows those facets. Poly Haven, the
usual CC0 source here, has no rigged animals. Every realistic rigged quadruped found in other free
libraries was either low-poly or a CC-BY download that needs an account. This horse is the
exception: realistic anatomy, a 2K albedo and normal map, and a working skeleton.

## Files

| file | what it is |
|---|---|
| `Horse.gltf` + `Horse.bin` | skinned mesh (3.7k vertices, body plus eyes), 19 bones, clips `Walk` (1.2 s), `Trot` (0.72 s), `Idle` (4 s) |
| `HorseAlbedo.jpg` | 2048² albedo |
| `HorseNormal.png` | 2048² tangent-space normal map |
| `prepare_horse.py` | the Blender 5.x script that produces all of the above from the upstream `.blend` |

Units are metres, Y-up, and the head faces +Z, the same convention as `Fox.gltf`. The hooves stand
on y = 0. The scale is baked into the vertices AND the bones, and every node transform is the
identity. A scale left on the armature node is applied twice by the engine's importer: the mesh
arrives in metres and the skinning matrices still carry the factor, so anything bound to the body
lands in the wrong place.

## What was changed from upstream, and why

All of it is in `prepare_horse.py`, so it can be re-run and reviewed:

1. **The mane and tail cards are removed.** They are polygon strips with a hair texture, the exact
   thing a groom replaces. A groom mane on top of a card mane is two manes.
2. **The eyes are joined to the head bone.** Upstream they are unparented spheres and stay behind
   when the head moves.
3. **Targetless IK constraints and envelope deformation are removed.** Blender would bake both into
   the exported clips, and no engine runtime reads either. After removal, what Blender previews is
   what the engine skins.
4. **An orphan weight group** (`Bone.005`, a secondary neck weight with no bone behind it) is folded
   into the neck bone.
5. **The material is rebuilt** as a Principled BSDF from the packed albedo and normal maps. The 2012
   material exports as a flat colour.
6. **Three in-place clips are authored**, because the source has none. The walk is a four-beat
   lateral sequence, and the trot moves diagonal pairs together. The swing and flex directions are
   measured from the rig, not assumed, so a bone roll cannot silently reverse a gait.

## Known limitations

- The rig has no fetlock or pastern bones, so the hoof stays rigid with the cannon. At a walk this
  is hard to see; at a trot the hoof does not snap back.
- The tail is a two-bone stub. The groom tail carries the motion.
- No root motion: the clips play in place, and an `AnimalPathComponent` moves the entity.

## Regenerating

The source `riggedHorse.blend` is not committed: it is 20 MB and only this script reads it. Download
it from https://opengameart.org/content/rigged-horse and check it against the SHA-256 recorded in
[LICENSE.md](LICENSE.md):

```
sha256sum ~/Downloads/riggedHorse.blend
# 9cca670b93a74d50e89263e50d55ab035a6c46aa7d2b21e354bdac6987037f4a
```

Then, from the repository root, write the glTF, textures and clips into this directory:

```
blender -b ~/Downloads/riggedHorse.blend --python OloEditor/SandboxProject/Assets/Models/Horse/prepare_horse.py -- OloEditor/SandboxProject/Assets/Models/Horse
```

To also render preview frames, pass a second directory:

```
blender -b ~/Downloads/riggedHorse.blend --python OloEditor/SandboxProject/Assets/Models/Horse/prepare_horse.py -- OloEditor/SandboxProject/Assets/Models/Horse build/horse-preview
```

The preview directory receives four frames of each gait, rendered with Blender's Workbench engine.
Look at them before committing. Three separate mistakes in this script each bent every knee into a
metre-long band while the rest pose looked perfect: `transform_apply` on the rig, baked-in IK and
envelope deformation, and re-reading a connected bone's head after its parent's tail had moved it. A
rest pose cannot show any of them; a bent knee shows all three.
