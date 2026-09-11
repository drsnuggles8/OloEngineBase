# Virtual geometry is ray-traced through a fixed proxy mesh, not a per-frame BLAS

Issue #1144. Virtualized (Nanite-style) geometry never reached GPU Scene, therefore never reached
the TLAS, therefore was invisible to every ray-traced effect in the engine: the hybrid RT shadows
(#1056), the reflection hierarchy (#1057), the GPU reference path tracer (#1055) and ReSTIR DI
(#1140) all traced a scene with the virtualized content missing from it. That also violated an
explicit #979 acceptance criterion — *raster, hybrid and PT modes consume the same canonical
instance/material/light identities*.

**Decision: each virtual-mesh part gets one fixed proxy mesh, built from the DAG's coarsest cut and
registered in GPU Scene as ordinary rigid geometry.** Rays trace the proxy. Nothing in the
acceleration-structure manager (#978) changes.

---

## 1. Why a cluster DAG cannot be a BLAS

A bottom-level acceleration structure is built from **one** index buffer over **one** vertex buffer.
Virtual geometry has no such thing. It has a DAG of ~128-triangle clusters, and the set of clusters
that represents the surface — the *cut* — is chosen on the GPU, per view, per frame, from the
camera-projected error of each cluster's group. The cut for the main camera differs from the cut for
cascade 2 of the shadow map, which differs again next frame.

So there are exactly three things one can trace, and this ADR picks the first:

1. **A fixed low-detail proxy.** Build one index buffer from a cut that does not depend on the
   camera, once, and trace that.
2. **A dynamic BLAS from the live cut.** Rebuild or refit an acceleration structure every frame from
   whatever the cull selected.
3. **Nothing, loudly.** Keep the exclusion and surface it so a user is told why their Nanite mesh
   casts no ray-traced shadow.

## 2. Why the coarsest cut is the proxy

The DAG's **coarsest cut** is its root clusters: every cluster whose member group is terminal
(FLT_MAX error). It is not an approximation invented for this feature — it is a cut the existing
selection rule already produces, at its limit, and it inherits three properties from the builder
that a hand-rolled decimation would each have to earn separately:

- **Watertight.** The builder locks group-boundary vertices, so any cut partitions the surface
  exactly. A hole in a proxy is a hole a shadow ray leaks through, and the symptom would be a soft
  grey patch nobody would attribute to the acceleration structure.
- **Small, and bounded by construction.** Each DAG level halves the triangle count, so the root cut
  of a million-triangle mesh is a few thousand triangles. The BLAS is cheap to build and cheap to
  trace, which is the entire reason not to trace the source mesh.
- **View-independent.** The cut does not move, so the geometry classifies as `Static` and #978's
  build-once policy applies unchanged — no new refit class, no per-frame acceleration-structure
  work, no new failure mode in a subsystem that already has tests.

The one thing given up is exactness: a close-up reflection of a virtual mesh reflects its
silhouette, not its micro-detail. For shadows, ambient occlusion and rough reflections —
overwhelmingly what these rays are for — approximately-right geometry is what was needed. This is
also what Unreal does for Nanite, for the same reasons.

## 3. Why not the dynamic BLAS

Option 2 traces exactly what was drawn, and that is genuinely better *if* you need it. It costs:

- an acceleration-structure rebuild or refit **every frame, per instance**, against a cut whose
  triangle count and topology both change — a refit cannot re-point at different triangles, so most
  frames are full rebuilds, which is the expensive kind;
- a new geometry class in #978 with its own refit policy, invalidating the "one BLAS per unique
  geometry" identity the manager is built on — a cut is per instance, so N instances of one mesh
  become N structures rather than one;
- a readback or a GPU-driven build path, because the cut is decided on the GPU and the CPU-side
  builder does not know it.

That is a large, permanent per-frame cost to fix a class of error that shows up only in a mirror
held close to a Nanite mesh. It stays available as a later tier if a use case demands it; it is not
what closes #979's shared-identity criterion.

## 4. What stays loud

The exclusion this replaces was **countable**, tallied in `GPUSceneUnsupportedCategory::Virtualized`
— which is what `CLAUDE.md` asks of a path that cannot do its job, and it must not be traded for a
silent partial success. So:

- The category survives, and is now reported **per part** rather than per entity — a part is what
  becomes one GPU Scene instance and one BLAS, so it is the unit the RT stats already speak in. It
  counts exactly the parts that got no proxy: a DAG with no usable coarse cut, a buffer that could
  not be created, and every virtual mesh on a non-Deferred path (where the mesh draws nothing at
  all, so its absence from the TLAS matches its absence from the frame).
- `VirtualMeshRegistry::SubmissionDiagnostics` gains `ProxyParts` / `ProxylessParts`, surfaced in
  the editor's Statistics panel and in `olo_virtual_geometry_stats`, so "N of M virtual parts got a
  proxy" is answerable without inference. Both surfaces print the **ray-tracing capability**
  beside it, because the counter measures staging and not tracing: on OpenGL every part stages and
  none of them is in a TLAS, since there is no TLAS, and a bare "25 / 25" there would report this
  issue's own failure as a success.
- A proxy that dropped a cluster or a triangle to an out-of-range reference **warns**, naming the
  count and saying the proxy is not watertight, rather than shipping a plausible mesh with a hole.

## 5. Known limitation, named on purpose

The path tracer's **area-light (next-event estimation) table does not gather virtual geometry.** It
walks a `MeshSource` submesh's triangles, and those are the full-resolution triangles, not the proxy
the TLAS holds; emitters that are not the traced surface make NEE aim rays at geometry that is not
there. So an emissive virtual mesh *is* hit by rays and shades correctly, but is not sampled as a
light source. `Renderer3D::SubmitVirtualMesh` warns once per mesh when it sees one. Closing that
means gathering emitters from the proxy, which is a separate change to
`EmissiveTriangleTable::QueueSubmesh`.

Skinned, cloth and particle geometry remain excluded and remain counted. Their problem is a
deformed-vertex stream, which is a different shape of work (#1150 tracks skinned virtual geometry).

## 6. Consequences

- `#654`'s Tier 3 "ray tracing / Lumen fallback mesh" entry is done, not deferred.
- The #979 criterion holds for virtualized content: one instance identity, one material record,
  shared by raster and by every ray.
- Ray-traced evidence captured in a scene with virtualized content is now trustworthy. It was not
  before, and `olo_rt_scene_stats` reporting a near-empty TLAS was the tell.
- A virtual mesh's proxy costs one small vertex/index buffer pair per part, for the process
  lifetime, on every backend — including OpenGL, which has no ray tracing. That is deliberate: the
  proxy is part of the canonical scene description, and gating it on the active backend would make
  GPU Scene's contents depend on the RHI.
