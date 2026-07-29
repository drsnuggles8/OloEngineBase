#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"

namespace OloEngine
{
    class MeshSource;

    // Configuration for the offline cluster LOD DAG cook. Defaults follow the reference
    // configuration in meshoptimizer demo/clusterlod.h (clodDefaultConfig(128)).
    struct VirtualMeshBuildConfig
    {
        u32 MaxClusterVertices = 128;  // clamped to [3, 256] (local triangle indices are u8)
        u32 MaxClusterTriangles = 128; // clamped to [1, 512] (meshoptimizer implementation limit)
        u32 MinClusterTriangles = 0;   // 0 derives MaxClusterTriangles / 3
        u32 TargetGroupSize = 16;      // clusters per simplification group; partitions may be up to ~1/3 larger
        f32 SimplifyRatio = 0.5f;      // per-level triangle reduction target
        f32 StuckThreshold = 0.85f;    // group is terminal if simplification keeps more than this fraction
        f32 ClusterSplitFactor = 2.0f; // meshopt_buildMeshletsFlex split factor (splits large-bounds clusters)
        u32 MaxLevels = 32;            // hard safety cap on DAG depth

        // Simplification entry point (clodConfig::simplify_permissive). Allows edge collapses
        // across ATTRIBUTE discontinuities — vertices that share a position but carry different
        // normals/UVs — except where the builder marks a genuine UV seam with
        // meshopt_SimplifyVertex_Protect. Without it, meshoptimizer classifies any position with
        // more than two attribute wedges as Kind_Locked and cannot collapse it at all, which is
        // what made flat-normal sources produce a flat, useless DAG (issue #651).
        bool SimplifyPermissive = true;

        // Fallback chain when a group will not reduce, in order. Both mirror
        // clodConfig::simplify_fallback_* ; a group that survives the whole chain becomes a
        // terminal (FLT_MAX-error) group as before.
        //
        // Welded: retry with the index buffer position-welded, which collapses ALL attribute
        // wedges rather than just the unprotected ones. Lossier than the permissive pass (the
        // canonical vertex's UV wins for the whole position) but topology-preserving.
        bool SimplifyFallbackWelded = true;
        // Sloppy: meshopt_simplifySloppy on a deindexed subset. This one does NOT preserve
        // topology, so the result is accepted only when it is still edge-manifold and has
        // exactly the border edges the group started with — see BuildCutSafety in the .cpp.
        bool SimplifyFallbackSloppy = true;
        // Error multiplier applied to a sloppy result (clodConfig::simplify_error_factor_sloppy),
        // accounting for appearance degradation the quadric error does not capture.
        f32 SimplifyErrorFactorSloppy = 2.0f;
    };

    // Offline builder for the Nanite-style cluster LOD DAG (issue #629, step 1).
    //
    // Algorithm (mirrors meshoptimizer demo/clusterlod.h; see also "Nanite: A Deep Dive",
    // Karis 2021): split the mesh into leaf clusters, then repeatedly partition live
    // clusters into groups of adjacent clusters, lock the vertices shared between groups,
    // merge + simplify each group to ~half its triangles, and re-split the simplified
    // geometry into parent clusters, until a single cluster (or a stuck group) remains.
    //
    // Invariants guaranteed on the produced VirtualMesh (pinned by VirtualMeshBuilderTest):
    //  - Monotone error: for every cluster, the error of the group it is a member of is
    //    >= the error of the group that produced it.
    //  - Nested LOD spheres: a member group's LOD sphere contains the producing group's
    //    LOD sphere, so projected screen-space error is monotone from any viewpoint.
    //  - Watertight cuts: for any error threshold, the selected clusters partition the
    //    surface exactly — group-boundary vertices are locked during simplification, so
    //    neighbouring clusters at different LOD levels share identical boundary edges.
    //    This lock has to OUTLIVE the level that created it wherever a group goes TERMINAL:
    //    a terminal group carries FLT_MAX error and is therefore selected at every threshold
    //    forever, but its clusters leave the pending set, so the ordinary per-level lock pass
    //    stops seeing the boundary it shares with its still-simplifying neighbours. Those
    //    neighbours then pull away from a boundary that is pinned for good and the coarse cuts
    //    crack along that seam. The builder keeps a build-lifetime frozen-position set for
    //    exactly this (FreezeTerminalGroupBoundary in the .cpp).
    //
    // Where this DIVERGES from clodDefaultConfig(128), and why:
    //  - clodConfig::optimize_bounds is irrelevant here: the emitted VirtualCluster always
    //    carries tight, separately-computed CULL bounds, while VirtualClusterGroup carries the
    //    conservative LOD bounds. The reference packs both roles into one clodBounds and has to
    //    choose; we do not.
    //  - clodConfig::partition_sort (spatial reordering of partitions) is off, as in the
    //    reference default — it only affects output ordering, which our blob does not depend on.
    //  - clodConfig::simplify_error_edge_limit is off, as in the reference default.
    //  - The error merge is max(previousError, currentError), which is exactly the reference
    //    formula at its defaults (simplify_error_merge_previous = 1, _additive = 0).
    //  - meshopt_simplifyWithUpdate is deliberately NOT used, even though it is the newest and
    //    highest-appearance-quality entry point. It updates vertex positions and attributes
    //    IN PLACE for an optimal fit. A VirtualMesh keeps ONE vertex array shared by every LOD
    //    level (simplification only ever removes indices, never adds vertices), so an in-place
    //    update would rewrite the LOD-0 geometry under the leaf clusters and move locked group
    //    boundaries — breaking both the source-fidelity guarantee and watertight cuts. Using it
    //    would require per-level vertex copies and a new blob format; clusterlod.h does not use
    //    it either.
    namespace VirtualMeshBuilder
    {
        // Builds the DAG for ONE submesh's triangle range. Returns an empty mesh
        // (IsValid() == false) for unsupported input: no geometry, a degenerate range, or a
        // skinned/morph-target source.
        [[nodiscard]] VirtualMesh BuildSubmesh(const MeshSource& meshSource, u32 submeshIndex,
                                               const VirtualMeshBuildConfig& config = {});

        // Builds one DAG per submesh. This is the entry point for real assets: a cluster
        // must not span a material boundary (a group is simplified as a unit, so a straddling
        // cluster could not be shaded by either material), so each submesh gets its own DAG
        // and is drawn as its own instance with its own material.
        //
        // Submeshes the builder cannot handle are skipped, not fatal — the set is valid as
        // long as at least one part built. Still rejects skinned / morph-target sources
        // outright: those deform at runtime, so a static cluster DAG would be wrong.
        [[nodiscard]] VirtualMeshSet BuildSet(const MeshSource& meshSource, const VirtualMeshBuildConfig& config = {});

        // Single-DAG convenience for a single-submesh source (and the CPU unit tests).
        // Equivalent to BuildSubmesh(meshSource, 0, config).
        [[nodiscard]] VirtualMesh Build(const MeshSource& meshSource, const VirtualMeshBuildConfig& config = {});
    } // namespace VirtualMeshBuilder
} // namespace OloEngine
