# GPU Instancing — Future Improvements

Originally a follow-up list for issue #173. Most items shipped; this
doc now only tracks **what's still open** and why. Items that landed
(auto-batching of `DrawMeshCommand` groups, `InstancedMeshComponent`,
GPU-side frustum culling, shadow auto-batching, per-instance motion
vectors, Color/Custom wiring, the `InstancePlacementAsset` merge cache,
the scatter-brush editor panel with slope filter / variants / undo /
bake) are documented in the source and pinned by tests
(`InstanceDataLayoutTest`, `CommandBucketBatchTest`,
`GPUFrustumCullParityTest`, `ScatterBrushMathTest`).

The three items below are deferred with concrete blockers — not
oversights. Each waits on adjacent work (BVH, mesh asset format change,
crowd-content authoring) that doesn't make sense to build
speculatively.

---

## §1.2 — Scatter Brush: Mesh-Surface Raycast

**Current**: the brush raycasts onto the terrain heightmap only.
[`InstanceScatterBrushPanel`](../OloEditor/src/Panels/InstanceScatterBrushPanel.cpp)
already consumes a `(hitPos, normal, hasHit)` triple from `EditorLayer`
— forward-compatible the moment a mesh ray-cast exists.

**Blocker**: no `BoundingVolumeHierarchy` ray-vs-mesh capability today.
An AABB-only fallback would return bad surface normals (slope filter
can't work) and float placements above the actual geometry. The right
unblock is the BVH itself, used by both the brush and any future tool
that needs world raycasts (gizmo snapping, click-to-select on mesh
backings, debug raycast viz).

**Integration after BVH lands**: extend `EditorLayer`'s per-frame
raycast loop in the scatter-brush branch to query the BVH after the
terrain pass; surface the closer hit (plus the triangle normal) to
`InstanceScatterBrushPanel::OnUpdate`. Nothing inside the panel
changes.

---

## §4 — Multi-Mesh / LOD Batching (Full Multi-Draw Indirect)

**Current**: `SelectLODMesh()` resolves to a single `Ref<Mesh>` before
the `DrawMeshCommand` is built, so the auto-batcher already collapses
**same-LOD** entities of the same source asset (`(meshHandle,
materialDataIndex, renderStateIndex)` grouping key). A 100-tree forest
split 30 / 40 / 30 across three LODs collapses to 3 draws.

**Blocker**: collapsing **across** LODs into one
`glMultiDrawElementsIndirect` would require all LODs of a source asset
in a single VAO with per-LOD `(indexOffset, indexCount, baseVertex)`
ranges. Today `LODGroup::Levels[i]` holds a distinct `AssetHandle`,
each pointing at its own `MeshSource` / VAO / IBO. That's a **mesh
asset format + import-pipeline change**, not a renderer change.

**Why not speculate now**: the codebase has the `LODGroupComponent`,
`LODGroup`, and `SelectLODMesh()` plumbing, but no shipped scene
meaningfully drives them — `LODTest.cpp` is the only consumer. The
3-draws-per-source state is unprofiled; the win is undefined.

**Renderer-side prereqs** already in place from §3 work:

- Single-draw indirect (`RendererAPI::DrawElementsIndirectRaw`).
- `gl_InstanceIndex` already accounts for `baseInstance`, so multi-draw
  indirect with per-LOD `baseInstance` Just Works.

**Concrete sketch when the time comes**:

- Mesh asset gains an optional `LODRanges` table:
  `vector<{u32 indexOffset, u32 indexCount, u32 baseVertex}>` per LOD
  level, with all geometry merged into one VAO at import.
- `LODGroup::Levels[i]` switches from `AssetHandle MeshHandle` to
  `(AssetHandle SourceMesh, u32 LODIndex)`.
- New `RendererAPI::MultiDrawElementsIndirectRaw(vaoID, indirectBufferID, drawCount)`
  wraps `glMultiDrawElementsIndirect`.
- Per-instance LOD index lives in a **parallel SSBO** (not in
  `InstanceData` — the 224-byte std430 layout is pinned by
  `InstanceDataLayoutTest`).
- `CommandBucket::BatchCommands` buckets by LOD, sorts contiguous,
  emits one `DrawElementsIndirectCommand` per LOD bucket into a
  multi-draw command buffer, and calls `MultiDrawElementsIndirectRaw`
  once.

---

## §8 — Per-Instance Skinned Mesh Batching

**Current**: the `isAnimatedMesh` skip at
[`CommandBucket.cpp:602`](../OloEngine/src/OloEngine/Renderer/Commands/CommandBucket.cpp)
intentionally blocks animated draws from auto-batching. Each animated
entity uploads its own 100-bone palette to `UBO_ANIMATION`.

**Blockers**:

- **Shader rewrite required**. `PBR_Skinned.glsl`,
  `PBR_MultiLight_Skinned.glsl`, and `PBR_GBuffer_Skinned.glsl` all
  index bones as `u_BoneTransforms[a_BoneIDs[i]]` from a `mat4[100]`
  UBO at binding 4. Batched draws would need
  `u_BoneTransforms[gl_InstanceIndex * 100 + a_BoneIDs[i]]` against an
  SSBO sized at `(maxInstances × 100)` — new shader variants, not a
  swap-in change. The parallel `UBO_ANIMATION_PREV` (TAA motion
  vectors) needs the same migration or batched draws silently break
  reprojection.
- **Bandwidth cost**. 100 bones × 64 B = 6.4 KB / instance →
  6.4 MB / frame for 1000 instances, plus prev-bones. The bone-matrix
  UBO budget caps at `GL_MAX_UNIFORM_BLOCK_SIZE` (≈16 MB on most
  hardware), so an SSBO-based path is required anyway.
- **No shipped use case**. `fox.olo` is a single character;
  `AnimationIKTest` and `InstancingDemo` don't crowd animated bodies.

**Two future paths** (in increasing order of investment):

1. **Bone-palette archetypes** (no shader changes — the right starting
   move when crowd content lands). K pre-computed pose snapshots
   (`idle`, `walk_cycle_phase_0`, …); each character picks one
   archetype's `boneBufferOffset`. Identical archetypes auto-batch
   with the existing static path; no per-instance bone storage needed.
   Requires authoring tooling and a `BoneArchetypeAsset` type.
2. **Per-instance bone SSBO** (the doc's original §8 sketch). Replace
   `UBO_ANIMATION` / `UBO_ANIMATION_PREV` with two SSBOs sized at
   `N_instances × 100 mat4`, rewrite all three skinned shaders to
   index by `gl_InstanceIndex * 100 + boneID`. Generalises to
   arbitrary per-instance animation but pays the full bandwidth +
   shader rewrite cost.

Path (1) is what most crowd games actually use — background actors
share poses; only hero characters get unique skeletal sims.
