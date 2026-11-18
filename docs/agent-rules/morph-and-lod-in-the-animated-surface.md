# Morph deltas go on the rest surface, and a surface that moved gets no velocity

Six rules, five from issue #1227. They extend
[skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md), which owns the bone
half of the same surface — read that first.

1. **Morph deltas are applied to the REST surface, on the CPU, in
   `Scene::EvaluateMorphTargets`.** That pass writes the one vertex buffer every skinned consumer
   reads, and `SkeletalDeformation.glsl` then applies the skin matrix to whatever that buffer holds.
   The combination order is therefore morph-then-skin for colour, depth, all three shadow passes and
   velocity *by construction*. Do not add a second place that applies a delta, and never apply one
   after skinning. A held expression is not deformed again: when the weights equal
   `AppliedWeights` over the same cached base, the pass leaves the surface alone. A breed whose
   sculpt keys every clip holds at 1.0 paid 7.7 ms per fixed step re-deforming and re-sending it
   (`AHeldExpressionIsNotDeformedAgainEveryStep`).
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

6. **A mesh file with morph targets and no skeleton imports through `AnimatedModel`, and a script
   addresses its targets as `MorphTarget_<index>`.** The static importer drops morph targets the way
   it drops bones, so `Model` records either as "the source needs the animated importer"
   (`IsSourceRigged`, `OMeshFormat::FlagSourceRigged`) and `AssimpMeshImporter` re-routes.
   `AnimatedModel` bakes a boneless mesh's node transform into its vertices and morph deltas. The
   entity is a plain `MeshComponent` plus a `MorphTargetComponent`; give every authored level of
   detail the same targets. After changing what that flag means, bump `OMeshFormat::CurrentVersion`.
   [Details](#a-morph-only-file-and-the-cache-that-hid-it)

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

## A morph-only file and the cache that hid it

Drift's sail is one mesh with ten morph targets and no bones, blended by a script from the wind.
Three things had to be true before it moved.

- **The import.** `Model` runs `aiProcess_PreTransformVertices`, which rebuilds every mesh without
  its anim meshes, so the sail arrived as a mesh nothing could deform and its
  `MorphTargetComponent` counted every weight as an unknown target (164 808 of them in a few
  minutes, on `olo_skeletal_deformation_stats`). `Model` now treats `aiMesh::mNumAnimMeshes > 0`
  like `mNumBones > 0`, and `AnimatedModel` records a morph-carrying result as needing the animated
  route on the warm load too. `MeshAssetBoneInfluenceImport.AMorphOnlyFileKeepsItsMorphTargetsColdAndWarm`
  pins both loads; with the routing removed it fails on the cold one.
- **The textures.** A texture the file carries inside itself (`"*0"`, a data URI, a GLB chunk)
  is loaded by one function for both importers, `LoadEmbeddedModelTexture`. `AnimatedModel` used
  to read `"*0"` as a file name beside the model and find nothing, which no fixture showed while
  only rigged files took that route, and which became a regression for every morph-only file
  once those did. `MeshAssetBoneInfluenceImport.AMorphOnlyFileKeepsItsEmbeddedTextureColdAndWarm`
  imports a morph-only glTF with a data-URI image and fails on the cold load with the lookup
  removed.
- **The node.** `AnimatedModel` reads vertices in the mesh's own space and leaves a skinned mesh's
  node to its skeleton. A mesh with no bones has no skeleton, so once morph-only files were routed
  here, one on a moved or scaled node would have arrived at the origin in its local size, which
  the static importer (`aiProcess_PreTransformVertices`) never did to the same file. `ProcessMesh`
  now bakes the node's global transform into a boneless mesh: positions by the matrix, deltas by
  its linear part, normals by its inverse transpose, and two corners of every triangle swapped
  when the node mirrors, or the mesh would face away from its own normals.
  `MeshAssetBoneInfluenceProjectTest.AMorphOnlyMeshOnAMovedAndScaledNodeIsImportedWhereTheNodePutsIt`
  imports from a scaled node and from a mirrored one. A fixture that had scaled its entity to make
  up for the missing bake (the imported corpus's morph cube, on a node scaled by 100) loses that
  scale: look for one when a morph-only asset arrives the wrong size after this change.
  A test that wants the STATIC route must import a file with no morph targets as well as no
  bones: `StaticMeshCoincidentCornerImportTest` had used a morph sphere for it.
- **The names.** `AnimatedModel::ProcessMesh` names a target `MorphTarget_<i>` to match the clip
  channels it builds, whatever the file calls it. The file's own names (`extras.targetNames`) are
  for people. A script therefore lists the names in file order and derives the engine's name from
  the position; `DriftBoatDelivery` compares that list with the generator's.
- **The cache.** The first live run still showed a rigid sail. An editor built before the change
  was running while the files were regenerated; its file watcher re-imported them down the static
  path and wrote a cache entry stamped with the new file's time and no flag. The new build read
  that entry as current. Nothing a developer is likely to repeat, but a v16 static cache of any
  morph-only file on any machine has the same shape, so the format went to v17.

## Related

- [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) — the bone half, and
  the call-site scan this issue's contract test is modelled on.
- [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md) — the same archetype with a CPU/GPU mirror.
