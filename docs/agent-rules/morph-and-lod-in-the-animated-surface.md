# Morph deltas go on the rest surface, and a surface that moved gets no velocity

Five rules, from issue #1227. They extend
[skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md), which owns the bone
half of the same surface — read that first.

1. **Morph deltas are applied to the REST surface, on the CPU, in
   `Scene::EvaluateMorphTargets`.** That pass writes the one vertex buffer every skinned consumer
   reads, and `SkeletalDeformation.glsl` then applies the skin matrix to whatever that buffer holds.
   The combination order is therefore morph-then-skin for colour, depth, all three shadow passes and
   velocity *by construction*. Do not add a second place that applies a delta, and never apply one
   after skinning.
2. **Every frame entry point that advances bone history advances morph history at the same call
   site.** `SkeletalDeformationSystem::AdvanceHistory` and `MorphDeformationSystem::AdvanceHistory`
   are paired in all four of Scene's entry points, and
   `SkeletalDeformationContract.EveryBoneHistoryAdvanceIsPairedWithAMorphAdvance` scans the call
   sites for it.
3. **A morphed surface that moved this frame gets its deformation history rejected — both halves,
   with a cause.** Call `Animation::RejectDeformationHistory(skeleton, morph, cause)`. Never hand a
   previous pose to a surface that is not the surface it was measured on.
4. **The conventional LOD level of an animated entity is resolved once, at the frame boundary, by
   `Scene::SelectAnimatedSurfaceLOD`.** Every consumer asks `AnimatedSurfaceSource()` for the mesh.
   Selecting again inside submission lets the morph pass deform LOD 0 while the renderer draws LOD 2.
   The selection is one frame behind the entity's authored transform, because
   `Scene::GetWorldTransform` reads a cache that `PropagateWorldTransforms` refreshes inside the
   tick — so a level is chosen from where the entity was when the last frame drew it. Expect a
   teleport to take one frame to change level, and do not "fix" it by selecting during submission.
5. **Any code that derives a `MeshSource` from another must call
   `MeshOptimization::CopyDeformationStreams`.** Bone influences and morph deltas are per-vertex
   arrays parallel to `m_Vertices`; a derived mesh that drops them or carries them without the remap
   is silently wrong.

## Why morph motion cannot become a velocity here

`OloDeformSkinnedVertex` builds its previous-pose position as `prevSkinMatrix * restPosition`, and
`restPosition` is whatever the vertex buffer holds **this** frame — that is, the surface as morphed
this frame. When the weights move, that previous position is a hybrid: last frame's pose on this
frame's surface. Every vertex the morph displaced reports a velocity measured between two different
surfaces, and TAA and motion blur smear it faithfully.

A real morph velocity needs the *previous morphed rest position* in the vertex stage, which is a
second per-draw vertex stream. There is nowhere to put one: both engine-wide vertex-pull bindings
are taken (`SSBO_VERTEX_PULL` 57, `SSBO_BONE_PULL` 63) and `UBO_*` 65 is the last free uniform
binding in the engine. So the contract is the issue's other branch — an explicit, counted rejection.
The counters are on `olo_skeletal_deformation_stats` under `morph` and in the editor's Statistics
panel; `surfacesRejected` staying at zero while a face is visibly expressing means the rejection is
not reaching the entity that is deforming.

The rejection is a statement about **one frame** and is re-decided every frame. It is cleared in the
frame-boundary advance and set again by the morph pass, so a face that reaches an expression and
holds it keeps its history. Latching it instead would cost every expressing character its temporal
history permanently, and the symptom — a face that is noisy under TAA — looks like a filter problem
several subsystems from the cause.

## Why conventional LOD had never reached a skinned mesh

Four separate refusals, each reasonable on its own:

- `MeshOptimization::GenerateLODMesh`, `GenerateLODMeshWithAttributes` and `BuildAutoLODChain` each
  returned early for any source with a skeleton, morph targets or a bone table — "not supported until
  auxiliary streams (weights/morphs) are preserved".
- `ModelImporter::EnsureAutoLODGroup` refused the same sources, so no imported character ever got a
  group.
- `Renderer3D::DrawAnimatedMesh` and `DrawAnimatedMeshParallel` never called `SelectLODMesh` at all.

The streams turned out to be cheap to preserve. The two index-only generators keep the source vertex
array verbatim and only shrink the index buffer, so the deltas and influences transfer with no remap.
`BuildLODMeshSource` compacts, and needed only `meshopt_optimizeVertexFetchRemap` in place of the
one-shot `meshopt_optimizeVertexFetch` — the remap **table** is the whole difference, and
`OptimizeMeshSource` had been applying it to these exact arrays all along.

Two ways a carried stream still goes wrong, and neither raises anything:

- **A level that lost its bone stream** has all-zero weights, which the shared producer reads as an
  unskinned vertex (`OLO_MIN_TOTAL_BONE_WEIGHT`). The character draws at its rest pose at that level
  and snaps back when it returns to LOD 0. `Scene::SelectAnimatedSurfaceLOD` refuses such a level
  rather than drawing it, because a hand-authored group can point at any mesh at all.
- **Deltas carried without the remap** deform whichever vertices happen to sit at those indices after
  compaction — a face that pulls apart rather than smiles.

## Two input traps

**`std::clamp` cannot reject a NaN.** Every comparison against NaN is false, so
`std::clamp(nan, 0.0f, 1.0f)` returns the NaN unchanged. `MorphTargetComponent::SetWeight` tests
`std::isfinite` first and counts the refusal; `MorphTargetEvaluator::EvaluateCPU` tests it before the
`w < 1e-4f` threshold for the same reason — a NaN passes that threshold test too. Weights arrive from
a scene file, a save file, a C# script, a Lua script and an MCP write: five untrusted routes.

**`MorphTargetSet::GetVertexCount()` says nothing about a sparse set.** It reads
`Targets[0].Vertices.size()`, and a sparse target's dense array is empty, so it reports zero. Use
`CheckCompatibility(meshVertexCount)`, which walks dense lengths and sparse indices separately.

## Why the base-surface cache is keyed on the morph set

`MorphTargetComponent::BasePositions` is the undeformed surface, cached to avoid re-reading it every
frame. Keying it on the mesh-source pointer is not enough: `GenerateLODMesh` keeps the full vertex
array, so two levels of one chain have the *same vertex count*, and a freed `MeshSource` can be
replaced by a new one at the same address. The key is the `MorphTargetSet` the component holds a
strong `Ref` to, plus the vertex count — the `Ref` is what makes the address non-recyclable while the
cache lives. A stale cache is in range for the new mesh, so reusing it writes the old mesh's rest
positions into the new mesh's vertices: silent, and wrong.

## Related

- [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) — the bone half, and
  the call-site scan this issue's contract test is modelled on.
- [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md) — the same archetype with a CPU/GPU mirror.
