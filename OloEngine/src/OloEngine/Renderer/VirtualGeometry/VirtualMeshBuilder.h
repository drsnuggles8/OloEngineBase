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
