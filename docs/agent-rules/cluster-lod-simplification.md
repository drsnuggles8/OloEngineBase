# Cluster-LOD DAG simplification (VirtualMeshBuilder)

Specifics of `Renderer/VirtualGeometry/VirtualMeshBuilder.cpp` and the meshoptimizer simplifier underneath it (issues #629, #651, #685). Read before changing simplification, group locking, or the cook.

## Rules

1. **Keep a terminal group's boundary locked for the whole build.** The per-level lock is recomputed from the pending set, so once a group goes terminal (`FLT_MAX` error) its seam with still-simplifying neighbours is invisible and the coarse cuts crack. Use the build-lifetime frozen-position set marked before `EmitGroup` consumes member geometry, OR-ed into per-level locks (`FreezeTerminalGroupBoundary`). A constraint created by an absorbing state must outlive the iteration that created it. [Details](cluster-lod-simplification-details.md#1-a-terminal-groups-boundary-must-stay-locked-for-the-whole-build)
2. **Assert the fixture contains both terminal and simplified groups before asserting watertightness.** Otherwise the test is vacuous; also sample many thresholds, since only coarse cuts break. [Details](cluster-lod-simplification-details.md#1-a-terminal-groups-boundary-must-stay-locked-for-the-whole-build)
3. **Use `meshopt_SimplifyPermissive`, not position welding, for wedge-heavy sources.** A position with more than two attribute wedges is `Kind_Locked` in meshoptimizer (`classifyVertices`); Assimp `aiProcess_GenNormals` splits every vertex, so nothing collapses and the DAG flattens silently (#651). Permissive preserves UV seams via `meshopt_SimplifyVertex_Protect`; welding closes them. Cook cost is roughly neutral (Sponza +4.3%, dragon parity). [Details](cluster-lod-simplification-details.md#2-meshopt_simplifypermissive-vs-position-welding-same-cure-different-cost-and-fidelity)
4. **Protect UVs only, compare attributes bit-exactly.** Mirror `clodMesh::attribute_protect_mask`: mark a vertex whose protected attributes differ from its canonical same-position representative. Normal wedges are what flat shading produces everywhere; `!=` would flag every NaN UV. [Details](cluster-lod-simplification-details.md#2-meshopt_simplifypermissive-vs-position-welding-same-cure-different-cost-and-fidelity)
5. **Do not invent mechanisms from the synthetic benchmark.** The flat-normal grid shows +61.5% but the real 7.2M-triangle dragon is parity; if a stand-in disagrees with the asset by 60 points the stand-in is wrong. Match n, compare distributions per side, and discard the first run (cold cache). [Details](cluster-lod-simplification-details.md#2-meshopt_simplifypermissive-vs-position-welding-same-cure-different-cost-and-fidelity)
6. **Combine permissive with explicit locks freely.** In `classifyVertices` the permissive promotion runs before the `vertex_lock & meshopt_SimplifyVertex_Lock` pass, so explicit locks win. [Details](cluster-lod-simplification-details.md#2-meshopt_simplifypermissive-vs-position-welding-same-cure-different-cost-and-fidelity)
7. **Do not use `meshopt_simplifyWithUpdate`.** It rewrites positions and attributes in place, and a `VirtualMesh` shares one vertex array across all LOD levels, so it would rewrite LOD-0 under leaf clusters and move locked boundaries. [Details](cluster-lod-simplification-details.md#3-meshopt_simplifywithupdate-is-unusable-while-one-vertex-array-serves-every-lod-level)
8. **Validate `meshopt_simplifySloppy` results before accepting them.** It does not preserve topology: require edge-manifold and exactly the group's original border edges. It has no absolute-error mode: scale its relative error by `meshopt_simplifyScale` over the deindexed subset and amplify (reference default x2). Locked vertices survive it. [Details](cluster-lod-simplification-details.md#4-meshopt_simplifysloppy-does-not-preserve-topology--guard-it-or-skip-it)
9. **Test for a terminal group with `>= std::numeric_limits<f32>::max()`, never `!std::isfinite`.** `FLT_MAX` is finite; an `isfinite` filter makes the coarsest-cut threshold `FLT_MAX`, `Error > threshold` selects nothing, and consumers get a valid empty answer (#1144: all 25 Sponza parts reported "no proxy"). `ProjectError` already does this right. [Details](cluster-lod-simplification-details.md#5b-terminal-groups-are-marked-flt_max-and-flt_max-is-finite)
10. **Switch to Deferred before reading virtual-geometry counters.** Virtual geometry only submits on the Deferred path and the editor starts on Forward, so every counter reads zero: `olo_renderer_settings_set { setting: 'renderpath', value: 'deferred' }`. `VirtualGeometrySponza.olo` works on a clean checkout (1 submitted, 25 instances, 5018 clusters); for the builder in isolation use `VirtualMeshBuilder::BuildSet` via `VirtualMeshRealAssetCookTest`. [Details](cluster-lod-simplification-details.md#5-measuring-a-builder-change)
11. **Instrument both sides of an A/B.** Stash the change selectively, leaving instrumentation applied to both. [Details](cluster-lod-simplification-details.md#5-measuring-a-builder-change)
12. **Do not delete or repoint the `xyzrgb_dragon.ply` registry entry.** It is fetch-on-demand (137 MB, 7.2M triangles) per `scripts/assets/asset-manifest.json`, fetched with `scripts/Fetch-Assets.ps1 -Name xyzrgb-dragon`; `AssetContentValidityTest` allowlists it on purpose and `VirtualGeometryStress.olo` places 24 of them. Check the manifest before calling a registry entry dead. [Details](cluster-lod-simplification-details.md#5-measuring-a-builder-change)
13. **Revert `AssetRegistry.oar` and any patched `.oloproj` after an editor run.** The editor rewrites the registry as a side effect. [Details](cluster-lod-simplification-details.md#5-measuring-a-builder-change)
14. **Bump `kVirtualMeshBuilderVersion` when geometry changes, and mix every new `VirtualMeshBuildConfig` field into `CurrentCookFingerprint()`.** Together they reject a stale `.omesh`. [Details](cluster-lod-simplification-details.md#5-measuring-a-builder-change)

## Commands and checks

```
scripts/Fetch-Assets.ps1 -Name xyzrgb-dragon
olo_renderer_settings_set { setting: 'renderpath', value: 'deferred' }
```

Fetch the dragon stress asset on demand; switch the render path before reading virtual-geometry counters.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[cluster-lod-simplification-details.md](cluster-lod-simplification-details.md).
