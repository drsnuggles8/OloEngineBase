# GPU Instancing — Future Improvements

Follow-ups identified during the implementation of issue #173 (GPU instancing).
The core feature is shipped on `feature/gpu-instancing-issue-173`:
auto-batching of same-mesh-same-material draws into single
`DrawIndexedInstanced` calls, an explicit `InstancedMeshComponent` for
authored dense placements, per-instance `Color` / `Custom` / `EntityID`
plumbed end-to-end, `InstancePlacementAsset` for shared placement data,
C#/Lua bindings, a 500-cube `InstancingDemo.olo` test scene under
`SandboxProject/Assets/Scenes/`, and a **"Instanced Draws" tab** in the
RendererProfiler panel that shows per-call mesh handle / instance count /
contributing entity IDs (toggle "Record Instanced Draws" to start
capturing).

Items below are intentionally left for later passes. Grouped by area and
roughly ordered by impact within each section.

---

## 1. Procedural Scatter Brush (High Impact / Medium Effort)

The current `Scatter Append` button in the `InstancedMeshComponent`
inspector ([SceneHierarchyPanel.cpp](../OloEditor/src/Panels/SceneHierarchyPanel.cpp))
is a **volume-based MVP**: it drops N random `InstanceData` placements
inside an authored AABB with optional random Y rotation and a scale range.
That covers "scatter inside a clearing" but not the spec's:

> Editor scatter brush — paint instances on terrain/surfaces with
> density, randomization, and slope filtering.

A real surface scatter brush needs:

### 1.1 Viewport Tool Mode

Promote the inspector controls into a proper editor tool — same pattern as
[`TerrainEditorPanel`](../OloEditor/src/Panels/TerrainEditorPanel.cpp). A
dedicated tool mode that intercepts viewport clicks, draws a brush preview
ring on the surface under the cursor, and accumulates placements on
left-drag.

### 1.2 Surface Raycast

Replace the AABB volume with a per-frame mouse-to-world raycast against:

- Terrain (heightmap sample at the hit UV, using the same lookup the
  terrain shaders use — see `Terrain_PBR.glsl` tess-eval stage).
- Mesh entities (BVH against scene meshes — the engine already has
  `BoundingVolumeHierarchy` for occlusion culling that could be reused).

Reject placements with no hit.

### 1.3 Density Falloff

Brush radius + density slider produce **Poisson-disc-distributed** points
inside the brush footprint each tick (use Bridson's algorithm for cheap
Poisson sampling). Avoids the clumpy clusters that uniform-random scatter
produces.

### 1.4 Slope Filtering

At each candidate placement, sample the surface normal:

- Reject if `dot(normal, up) < slopeThreshold` (configurable; default
  ~0.7 = ~45°).
- Optionally bias placement orientation toward the surface normal (foliage
  bends away from gravity, debris settles flat).

### 1.5 Mesh Variation

Most foliage layers want >1 mesh variant per layer. Either:

- Multi-mesh `InstancedMeshComponent` (one entity, several
  meshes/materials, the brush picks one at random per placement). Today
  each variant needs its own entity.
- Or: variant selector on `InstancePlacementAsset` (a layer references N
  mesh assets and the per-instance `Custom` float carries a 0..1 variant
  index that the shader uses to pick a sub-mesh / sub-material).

### 1.6 Undo/Redo

The volume-scatter MVP appends in bulk and "Clear Inline" wipes the lot —
no granular undo. A real brush needs per-stroke undo (snapshot the
`Instances` vector at brush-down, restore on Ctrl+Z) integrated with the
editor's existing undo stack.

### 1.7 Persistence Path

Once the brush produces meaningful data, authors will want to **bake** a
stroke into an `InstancePlacementAsset` (the `.oloinstances` type already
exists — see [InstancePlacementAsset.h](../OloEngine/src/OloEngine/Asset/InstancePlacementAsset.h)).
Inspector button: "Bake inline → new placement asset" — produces a fresh
`.oloinstances` file in the content browser, sets the component's
`PlacementAssetHandle`, clears the inline list.

---

## 2. Per-Instance Motion Vectors for `InstancedMeshComponent` ✅ done

The `Renderer3D::DrawMeshInstanced(span<InstanceData>)` overload delegates
to the transform-only overload, which now records per-instance transform
history via `GetAndRecordPrevInstanceTransforms` keyed by
`(meshHandle, ownerKey)`. `RenderPipeline::BeginFrame` rotates
`CurrInstanceTransforms` → `PrevInstanceTransforms` so frame N+1 looks up
the array submitted on frame N. The dispatcher reads
`prevTransformBufferOffset` from the produced `DrawMeshInstancedCommand`
and writes per-slot `PrevTransform` into the `InstanceData` SSBO — TAA
and motion-blur paths get correct per-instance velocity for free.

Note: the `InstanceData.PrevTransform` field that callers can author on
`imc.Instances[i].PrevTransform` is **not** forwarded through the
submission path; the engine-managed cache is the authoritative source.
Per-instance `PrevTransform` authoring (e.g. for shader-side history
manipulation) would need a separate overload that opts out of the cache.

---

## 3. GPU-Side Per-Instance Frustum Culling (Medium Impact / High Effort)

CPU per-instance culling already happens in
`Renderer3D::DrawMeshInstanced` before the FrameDataBuffer allocation. For
huge counts (10k–100k instances) the CPU loop becomes the bottleneck. A
compute-shader cull pre-pass would:

1. Read the input `InstanceData[]` buffer.
2. Test each instance's mesh bounds against the view frustum on GPU.
3. Use `atomicCounterIncrement` to compact survivors into an output
   buffer.
4. Driver-side: indirect dispatch reads the output buffer + the survivor
   count for the actual `glDrawElementsIndirect`.

Reference: NVIDIA's "Indirect Multi-Draw with Compute Culling" GPU Gems
chapter.

---

## 4. Multi-Mesh / LOD Batching ⚠️ partial — current state covers same-LOD groups

**Current state**: LOD selection happens *before* `Renderer3D::DrawMesh`
constructs the command, so `cmd.meshHandle` already names the
LOD-resolved mesh. The auto-batcher groups by
`(meshHandle, materialDataIndex, renderStateIndex)` — so entities of the
same source asset that resolve to the *same* LOD level batch into one
draw, while different LOD levels stay as separate draws. A scene with
100 trees split 30 / 40 / 30 across three LOD levels already collapses
to 3 draws (down from 100).

**Future work**: collapsing across LOD levels — "100 trees at varying
distances → 1 draw" — requires `glMultiDrawElementsIndirect` with a
per-instance `(indexOffset, indexCount)` lookup, since each LOD level
reads a different slice of the combined index buffer. This is a
significantly larger rewrite touching the mesh asset format,
the LOD struct, and the dispatcher. Deferred until profiling shows the
2-3 draws per source asset are an actual bottleneck.

Sketch (when worth the effort):

- Mesh asset stores all LODs in one VAO with submesh offsets.
- New `InstanceData` field: `u32 LODIndex`.
- Per-LOD `(indexOffset, indexCount, baseVertex)` table emitted as a
  command buffer for `glMultiDrawElementsIndirect`.
- Auto-batcher groups by source asset rather than LOD-resolved handle;
  each per-instance `LODIndex` indexes into the indirect command buffer.

---

## 5. Shadow Auto-Batching ✅ done

`ShadowRenderPass::RenderCascadeOrFace` now groups static mesh casters by
`(drawVao, indexCount, baseIndex)` and emits one
`glDrawElementsInstanced` per group per cascade via
`RendererAPI::DrawIndexedInstancedRaw`. The `InstancingDemo` scene's
500 cubes collapse from 2000 individual depth draws (4 cascades × 500
casters) to 4 instanced draws total. Skinned / terrain / voxel casters
keep their per-caster loop because per-instance state (bone matrices,
heightmap, terrain UBO) blocks batching — see §8 for the skinned case.

---

## 6. `InstancePlacementAsset` Streaming (Low–Medium Impact / Medium Effort)

`Scene.cpp`'s per-frame merge of `imc.Instances` + `placementAsset->GetInstances()`
copies into a new `std::vector` every frame. For huge static placement
assets this is a measurable allocation. Either:

- Cache the merged vector on the component (rebuild when either source
  changes).
- Stream the asset's instances directly into FrameDataBuffer without the
  inline-merge step (allocate the streams for the asset's chunk, then
  separately for the inline chunk, then emit two `DrawMeshInstanced`
  packets that share the same SortKey so they sit adjacent).

---

## 7. Color/Custom Wiring for Auto-Batched Draws ✅ done

`DrawMeshCommand` now carries `glm::vec4 color` (default white) and
`f32 custom` (default 0.0). `CommandBucket::BatchCommands` scans the
group for any non-default value; if found, it allocates a parallel
`FrameDataBuffer::Colors` / `Customs` stream and writes per-source
values into it, setting `colorBufferOffset` / `customBufferOffset` on
the emitted `DrawMeshInstancedCommand`. All-identity groups skip
allocation (the common case) so scenes that don't use per-entity tinting
pay zero extra FrameDataBuffer pressure. Callers wiring up per-entity
tint (scripts, future `MaterialComponent` per-entity override path) set
`cmd.color` / `cmd.custom` on the `DrawMeshCommand` they construct;
batch collapse preserves both fields.

---

## 8. Per-Instance Skinned Mesh Batching (Low Impact / High Effort)

Animated mesh draws are explicitly excluded from auto-batching in
`CommandBucket::BatchCommands` (see the `isAnimatedMesh` skip). The bone
matrix array is large and per-instance, so collapsing them would require
either:

- A bone-matrix SSBO indexed by `(gl_InstanceIndex, boneIndex)`.
- Or pre-computed bone palettes per crowd archetype (every soldier shares
  one of K pose snapshots; per-instance picks the archetype index).

Specialized — only relevant for crowd-heavy games.

---

## References

| Source | Notes |
|---|---|
| Issue #173 | Original spec; this doc tracks deferred items |
| `feature/gpu-instancing-issue-173` | Implementation branch |
| [InstanceBlock_Vertex.glsl](../OloEditor/assets/shaders/include/InstanceBlock_Vertex.glsl) | Shader contract for instance SSBO |
| Bridson, R. — *Fast Poisson Disk Sampling in Arbitrary Dimensions* (2007) | For §1.3 |
| NVIDIA GPU Gems 3, Ch. 36 — *Indirect Multi-Draw* | For §3 |
