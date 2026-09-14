# A bound that only ever REJECTS may be tightened per cluster; a bound a hierarchy's invariants rest on must grow by one uniform scalar

When geometry deforms at runtime and its culling data was computed against a rest pose, work out
which of the two kinds each bound is before you widen it:

- **Rejection bounds** — a frustum sphere, a Hi-Z box, a size threshold. These must still CONTAIN
  their geometry; an undersized one rejects what is on screen, which is the whole failure this
  document is about. What is cheap here is the other direction: an oversized one costs only a wasted
  draw, no invariant depends on it, so tighten it as far as the deformation data safely allows, per
  cluster.
- **Selection bounds** — anything a hierarchy's invariants are stated over: the LOD sphere and error
  of a cluster DAG, a quadtree node's extent, an impostor's switch distance. Being wrong costs a
  **crack**, because the invariant (nesting, monotonicity) is what made neighbouring pieces agree on
  which level to draw. Grow every one of these by the **same scalar** and scale every error by the
  **same factor**. A uniform transform preserves containment and ordering exactly; a per-node bound
  computed independently does not, and nothing tells you it stopped.

Derive the scalar, never estimate it. For linear-blend skinning the derivation is three lines and it
is in `OloEngine/src/OloEngine/Renderer/VirtualGeometry/VirtualSkinningBounds.h`; the key is that a
skinned position

    p'(v) = SUM_b w_b * M_b * v ,   w_b >= 0 ,   SUM_b w_b = 1

is a **convex combination**, so it never leaves the convex hull of `{ M_b * v }`. A set containing
every `M_b * v` therefore contains `p'(v)` **without knowing a single weight** — only which bones are
involved. That is what makes a per-cluster bound computable from data the cook can store (a bone list
per cluster) and a per-instance bound computable in O(bones) per frame (a rest sphere per bone).

## The Lipschitz constant is where a "cheap" bound stops being a bound

Every one of these bounds multiplies a radius by "how far this transform can stretch a vector" —
the operator 2-norm of its linear part. The obvious cheap stand-in, **the length of the longest
column, is not an upper bound on it**. It is exact for a rotation times a scale, which is what a
bone matrix usually is, and it under-estimates for a sheared one — which a non-uniformly scaled
parent bone composed with a rotation produces. A bound that is only wrong for unusual rigs is a
bound that holds everywhere it gets tested.

Use `sqrt(||A||_1 * ||A||_inf)` (max absolute column sum times max absolute row sum). It is sound
for every matrix, costs nine absolute values, and is exact for the identity. And take the norm of
`(A - I)`, not of `A`, wherever the quantity is a DISPLACEMENT — that is what makes the rest pose
cost exactly zero instead of always at least twice the radius.

## Two corollaries that are easy to miss

**A normal cone is not a bound — it is a claim about normals, and deformation rotates them.** Skip
the backface-cone test entirely for a deforming instance. Dropping a cull is always safe; keeping one
whose premise has expired drops clusters that are facing the camera.

**An error threshold and an error scale are interchangeable.** If every LOD error should be multiplied
by `s`, dividing the instance's pixel threshold by `s` on the CPU is exactly equivalent — both cut
rules compare a projected error against the threshold and the projection is linear in the error. That
is worth knowing when the GPU record has no spare lane: it buys the same cut for no new field and no
arithmetic in the cull's inner loop.

## What this looked like when it was wrong

Issue #1150 lifted `VirtualMeshBuilder`'s rejection of skinned sources. The first shape that suggests
itself is "deform every bound the same way, per cluster and per group, from each one's own bone set" —
it is tighter everywhere and it is wrong in one place. Group LOD spheres deformed independently stop
being nested, the projected error stops being monotone along DAG edges, and the cut is no longer
watertight: adjacent clusters choose different levels and the surface splits along the seam. The
symptom is not a hole with an obvious cause; it is a seam that opens and closes as the character
moves, on a path where "the LOD popped" is a much more available explanation than "the bound stopped
nesting".

The same asymmetry is why the cluster cull spheres *are* deformed per cluster: they only ever reject,
so no invariant is stated over them, and leaving them at the instance-wide padding would have made
frustum and Hi-Z culling useless on exactly the meshes that need it.

## Pinning it

Containment is checkable and cheap to check, so check it rather than looking at it:

- sweep an animation **range**, not a frame. A bound that holds only in the rest pose passes a
  single-pose test trivially — and the rest pose is where the cook computed it;
- assert per **vertex**, inside its own cluster's bound, for every pose;
- include an identity-palette control, or a bound that is merely enormous passes everything;
- include a case with **scale** in it. A rotation-only sweep never exercises the Lipschitz term, which
  is the term that is easiest to leave out.

`OloEngine/tests/Rendering/VirtualSkinnedBoundsTest.cpp` does all four, plus a test that the
per-cluster bound is actually *tighter* than the instance-wide fallback for most clusters — without
that one, the whole per-cluster mechanism could be dead weight and every other test would still pass.

## And the trap that costs an afternoon: a pose that maps the fixture onto itself

A *visual* test of a deformation needs a pose whose result is **distinguishable from the rest pose in
the thing the test measures**. That is not automatic, and the natural first fixture gets it wrong:
rotating each bone about the MODEL ORIGIN with a sphere centred there moves every vertex and leaves
`|p|` unchanged, so the silhouette — and any pixel count, centroid or golden taken from it — is
byte-identical. The implementation can be perfect and the test still reads "it did not deform".

Pivot the bones at a JOINT instead (for a unit sphere, `(0, -1, 0)`), which is what a real bone chain
does anyway. Bounds tests are immune — they check containment, not shape — but anything that looks at
the result is not.

Diagnosing this is cheap if you remember that **shaders are runtime assets**: no rebuild is needed to
bisect one. Return early from the posing function with a probe value (`restPosition * 2.0`, then the
unpacked weight sum, then a bone id, then one palette matrix applied directly, then the real
displacement as a vertical offset) and re-run the test between edits. Five one-minute iterations
walked the whole chain — flag, base, weights, ids, palette, arithmetic — and proved every link
correct, which is what left the fixture as the only remaining suspect.

## Where the data goes when there is no binding left

The SSBO binding namespace here is full below Mesa's ceiling of 80
([ssbo-binding-cap-is-80-on-mesa.md](ssbo-binding-cap-is-80-on-mesa.md)), so neither of the two new
streams could have one. Both ride existing buffers as packed tails, and the choice of WHICH buffer is
a **lifetime** decision, not a capacity one:

- static, per-vertex and per-cluster data (skin bindings, cluster bone sets) rides the cluster vertex
  arena, which is device-local and filled by page loads;
- per-frame data (the bone palettes) rides the instance buffer, which is restaged every frame anyway.

Putting the palette in the vertex arena would have meant writing per-frame data into a device-local
buffer that streaming owns. Putting the bindings in the instance buffer would have meant re-uploading
static geometry every frame. See `VirtualSkinningPacking.h` for the layout and
[`VirtualLightmapUVPacking.h`](../../OloEngine/src/OloEngine/Renderer/VirtualGeometry/VirtualLightmapUVPacking.h)
for the same route taken by issue #867.
