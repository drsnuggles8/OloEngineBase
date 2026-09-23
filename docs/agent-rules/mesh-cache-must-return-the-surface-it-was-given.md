# A mesh cache must return the surface it was given

**Anything that addresses a mesh by triangle index or triangle corner needs the cold import, the
warm `.omesh` load and the asset-pack load to produce the same index buffer, corner for corner.
Test it with `AnimatedModelCacheSurfaceTest`, not with vertex and index counts.**

The groom binding (#1249) is one such consumer. It records a triangle and barycentrics for every
root, and it refuses a body whose topology hash has moved. That refusal is correct: re-snapping
would produce a coat that looks authored and is wrong. So a surface that comes back from the cache
with different corner order is not a cosmetic difference. It detaches the coat.

## What happened (#1223)

The epic's acceptance scene opened in the editor with one horse's coat standing at its bind pose
while the horse walked. The log said `Groom binding refused ... the target mesh is not the surface
this binding was built against`. The binding had been cooked against a cold import of `Horse.gltf`
in one process. The editor loaded the same file, and in between, the `.omesh` cache had become
warm.

Two independent causes, each enough on its own:

1. **The warm load optimized twice.** `AnimatedModel` built each mesh, and `Build()` ran
   `OptimizeMesh` on it, but it never marked the mesh pre-optimized. The combined cache was
   therefore written without `FlagPreOptimized`, and the warm split ran `OptimizeMesh` again.
   meshoptimizer's vertex-cache and fetch passes are not idempotent, so the order changed.
   `Model` (static meshes) sets the flag; `AnimatedModel` did not, although a comment in `Model.cpp`
   claimed it did.
2. **The index codec rotates corners.** `.omesh` and the asset pack both store indices through
   `meshopt_encodeIndexBuffer`. It keeps every triangle, its order and its winding, and is free to
   rotate which corner comes first. With the double optimization removed, the warm buffer still
   differed at index 3.

Fixes: `AnimatedModel` counts a built source as optimized. `OptimizeMesh` puts the live buffer
through the codec round trip, so a cold import is already in the form every later load decodes.
`.omesh` moved to v9 to invalidate the caches written before.

## Why nothing caught it

`ModelWarmCacheGeometryIdentityTest` is named for byte identity but compares vertex counts, index
counts and submesh windows. Every one of those survives a corner rotation and a re-optimization.
The acceptance suite built each subject from a fresh import, so its bindings always matched. The
first thing to load a body twice, through two paths, was the editor.

## How to apply

- A new consumer that addresses geometry by index: add its mesh to
  `AnimatedModelCacheSurfaceTest`, or write the equivalent cold-versus-warm comparison of the index
  buffer itself.
- A new mesh writer or reader: it must not reorder, rotate or re-optimize what it is given, or the
  loaded surface is a different mesh as far as a binding is concerned. If the format changes what
  comes back, bump the version so existing caches re-import.
