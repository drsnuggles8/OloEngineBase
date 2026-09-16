# A ray tracer can only see geometry that is in memory, and skinning is not

**Before putting animated geometry in an acceleration structure, find out where its deformed
vertices live. In this engine they did not live anywhere.** From issue #1229. Read
[vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) for
the structure rules this extends and
[skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) for the producer it
borrows.

Code: `Renderer/RayTracing/DeformedSurfaceCache.{h,cpp}`,
`Passes/SkeletalDeformPass.{h,cpp}`, `assets/shaders/compute/SkeletalDeformToBuffer.comp`,
`RayTracingScene::Classify` and `DecideBuild`.

## 1. The rule, and why it is not obvious

Every raster consumer of a skinned vertex deforms it **inside its own vertex stage and keeps
nothing**. That is correct and cheap, and it means a skinned mesh's vertex buffer holds the *rest*
surface at every instant — the pose exists only as registers inside a shader invocation that has
already retired.

A BLAS is built from a buffer. So "put the character in the TLAS" cannot be done by pointing a build
at the mesh's vertex buffer, however animated the entity is. Something has to **write the pose to
memory first**, and before #1229 nothing in this engine ever had.

The trap is that pointing the build at the rest buffer *works*: a valid structure, a successful
build, legal API usage, healthy counters, and a T-posed character in every ray-traced effect. See
§4.

## 2. Reuse the producer; the storage is the only thing allowed to differ

The deformation itself comes from `include/SkeletalDeformation.glsl` through
`OLO_DEFORM_EXTERNAL_PALETTE`, exactly as virtualized geometry reaches it — only the palette's
**storage** differs (a device address here, a per-draw UBO there).

#1226 removed seven copies of linear-blend skinning that had drifted; a copy in a compute shader
would drift the same way, and the symptom would be a ray-traced silhouette disagreeing with the
rasterized one — visible only as a shadow whose outline is subtly wrong.

`SkeletalDeformationContractTest`'s `kSkinnedConsumers` enforces it — and **that list was not closed
until this issue**. Its own comment promised a test that nothing outside it reaches the producer, and
no such test existed; every other case iterates the list, so an unlisted consumer was invisible to
all of them. `NoShaderOutsideTheListReachesTheProducer` closes it, and asserts the scan found as many
shaders as the list names, because "nothing unlisted" is also what a broken walk reports.

## 3. Two things the compute shader must do that the vertex stages do not

**Divide by w.** `OloDeformedSurface::Position` is homogeneous and its `w` is the *total influence
weight*, deliberately not 1. Raster consumers carry that `w` into `gl_Position` and let the
perspective divide absorb it; a vertex buffer has no such divide. For affine `M` and linear `VP`,
`VP * M * vec4(p, w) == w * (VP * M * vec4(p/w, 1))`, and a uniform scale of clip coordinates
vanishes in the divide — so **`p/w` is exactly the point raster draws**. Skipping it makes RT and
raster disagree on every vertex whose weights a cook did not normalize.

**Normalize the normal.** The rest stream holds unit normals and so must the deformed one; the
linear-blend matrix carries both the weight sum and any bone scale, so a renormalize is right here
and a divide by `w` is not. Both operations fall back to the rest value on a degenerate result: a NaN
vertex does not fail a build, it poisons the tree for the whole surface.

## 4. Ask the two records, not an exclusion someone else owns

The rest-pose-BLAS defect that motivated this work is written up where the classification lives —
[vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md), *"a
corollary that cost a merge to learn"*. The rule it leaves behind governs this file too:

`Classify` asks the two records **directly** — the instance flag for "does this surface deform", the
geometry flag for "is this stream the deformed one" — rather than trusting an upstream exclusion it
does not own. `ResidentCounters::AnimatedInstancesRefused` makes the refusal visible, because a
refusal nobody can see is the same silence in a different shape.

## 5. One GeometryClass slot, two independent properties

An alpha-masked character is **both** `Masked` and `Deformed`. `MostRestrictive` picks `Deformed`,
so that is what `Classify` returns — and an instance whose opacity was derived from the returned
class would therefore be forced opaque and trace its hair cards and leaves as solid quads.

Opacity is read from the **material** instead — where it lives, and what the BLAS geometry flag
comment has said since #978. For a rigid instance that is bit-for-bit what
`RequiresCandidateConfirmation(class)` already computed. The general shape: when an enum has one slot
and a record has two orthogonal properties, derive each consumer's answer from the property it
depends on, not from the enum that had to pick one.

## 6. The geometry KEY moves with the stream, and that is what gives per-instance structures

A BLAS is keyed per GPU Scene **geometry**, which is correct for rigid meshes and fatal for deforming
ones: two characters sharing a skinned asset share the rest buffer, so they shared a record — and
they hold different poses.

The fix needs no second key space. A geometry key is `(vertex buffer, index buffer, submesh)` and a
deformed stream belongs to one entity, so staging the record against the **deformed** buffer makes
the key per-surface for free and a per-surface BLAS falls out. Three consequences:

- **Nothing downstream changed.** The BLAS build, the hit-triangle fetch and the alpha-cutout
  confirmation all follow `GPUSceneGeometry::VertexAddress`, so moving that one field moved
  traversal, shading and masking together. The deformed stream is byte-for-byte an
  `OloEngine::Vertex` array for exactly this reason — a different layout would have made
  `OLO_RT_VERTEX_STRIDE` per-geometry data.
- **The raster path is untouched**, not branched around. Nothing outside the ray tracer reads a
  geometry record's vertex address; the draw still binds the mesh's own vertex array and skins in
  the vertex stage. On OpenGL the cache is never enabled, so the record staged is the record that
  was staged before this issue.
- **An LOD switch is free.** It hands the entity a different `MeshSource`, so the key changes, the
  old record dies, the new one is a first build — which is the correct answer for a vertex-count
  change, reached structurally rather than by a special case.

## 7. A pose change is invisible to a geometry fingerprint

`FingerprintGeometry` hashes addresses, counts, offsets and formats. **A deformation rewrites the
vertex bytes in place and moves none of them.** So a fingerprint-driven policy either refits every
animated surface every frame or never refits one at all.

**The skeleton's deformation revision cannot answer it either, and that is the trap.** It is the
obvious candidate — #1228 minted it and predicted #1229 would read it — but #1226's frame-boundary
rule advances bone history once per frame for *every* skinned entity **whether or not it animated**,
so it ticks for a character standing perfectly still. Driving the refit from it was measured on the
live fox scene in edit mode, pose fixed, clip stopped: **a refit every frame and a full rebuild
every eighth**, for an animal that never moved. The counters said `dispatched=1, skipped=0` on every
sample.

What answers it is the **producer**, because only the producer knows whether it rewrote the
vertices. `DeformedSurfaceCache` hashes the bone palette it was handed — bytes, not floats, because
this is a "same pose?" question and an epsilon would let a slow drift accumulate under a structure
that was never refitted for it — and advances a `DeformedContentRevision` only when it actually
dispatches. That lane rides the instance record in what used to be `DeformationPad0`, so it costs no
bytes and moves no offset. Re-measured after the change: `dispatched=0, skipped=1, refits=0,
builds=0` while idle, and `dispatched=1, skipped=0, refits=1` the moment the clip plays.

The general form: **a counter that advances with the FRAME cannot answer a question about the
DATA.** Two things both called a "revision" can differ in exactly that way, and the difference is
invisible until something expensive is keyed on the wrong one.

`DecideBuild` takes the producer's answer as `deformationAdvanced`, and:

- **the pose test comes before the refit-budget heuristic.** Reversed, a character standing still at
  a run length past the budget is handed a full rebuild every frame *for not moving* — the most
  expensive possible answer to "nothing happened";
- **a first build ignores it.** A new surface legitimately sits at content revision 0, and
  `HasDeformation` is what separates "built at revision 0" from "never built";
- **the recorded revision advances only when a build was actually recorded.** Storing it
  unconditionally marks a surface current on a frame whose build was skipped or failed, and it then
  holds the previous pose forever while the policy believes it current.

An idle crowd costs zero acceleration-structure work per frame. That is the difference between
animated surfaces being *supported* and being *affordable*.

## 8. The producer writes, the consumer builds, and the barrier is the producer's

`SkeletalDeformPass` is a separate node immediately before `RayTracingScenePass`, and the latter
declares `DependsOnPass("SkeletalDeformPass")` rather than relying on registration order — an
ordering that holds because two `AddNode` calls are adjacent is invisible to the graph.

The barrier is **compute write → AS build read**, and its destination access is `SHADER_READ`, not
`ACCELERATION_STRUCTURE_READ`: a build reads its *input* geometry as shader storage (the Vulkan
backend's instance-buffer barrier makes the same distinction). It is emitted by the node that did the
writing — a barrier emitted by the reader sits after its own hazard the moment anything is recorded
between them.

## 9. Do not add a counter you never write

`RayTracing::FrameCounters` has `BlasBuildGpuNs` and `TlasBuildGpuNs`, both declared by #978, both
read by the Statistics panel, and **neither ever written**. They read zero forever, and the panel
hides them behind `if (> 0)`, so nothing says so.

So the deformation pass has no GPU-time field of its own: it brackets its dispatches with
`GPUPassTimerPool::BeginSubPass("SkeletalDeformToBuffer")` and its cost arrives through the same
per-pass channel as every other pass. A field that is always zero is worse than an absent one — it
answers the question wrongly instead of sending the reader somewhere that can.

## Related

- [vulkan-ray-tracing-acceleration-structures.md](vulkan-ray-tracing-acceleration-structures.md) —
  the structure rules, the classification, and §4's stale-exclusion story from the other side.
- [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) — the one producer
  this consumes, and the call-site-scan test this issue's closed-list check is modelled on.
- [animated-surface-records.md](animated-surface-records.md) — the deformation revision, minted for
  the question §7 asks.
- [morph-and-lod-in-the-animated-surface.md](morph-and-lod-in-the-animated-surface.md) — why the
  rest surface this reads is already morphed, and why an LOD level is a different surface.
- [no-silent-fallbacks.md](no-silent-fallbacks.md) — the rule §4 is an instance of.
