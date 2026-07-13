#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/vec3.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    // @brief Nanite-style virtualized geometry: the offline-baked cluster LOD DAG (issue #629).
    //
    // A VirtualMesh partitions a source mesh into ~128-triangle clusters and recursively
    // merges groups of adjacent clusters, simplifies each group to ~half its triangles, and
    // re-splits the result into coarser parent clusters, forming a DAG that supports
    // crack-free view-dependent LOD selection (see VirtualMeshBuilder.h).
    //
    // Selection contract (mirrors the reference cut rule in meshoptimizer's clusterlod):
    // a cluster is part of the LOD cut for an error threshold T iff
    //   1. the error of the group it is a member of (GroupIndex) is OVER T, and
    //   2. it is original geometry (RefinedGroup == -1) OR the error of the group whose
    //      simplification produced it (RefinedGroup) is at or under T.
    // Group errors are monotone along every DAG edge and group LOD spheres are nested, so
    // the projected screen-space error is monotone too and the cut is always watertight.

    // Hard cap on DAG depth shared by the builder and the blob format: the builder clamps
    // its MaxLevels config to kMaxVirtualMeshLevels - 1 (a terminal group can sit at
    // Depth == MaxLevels, so LevelCount <= MaxLevels + 1), and the deserializer rejects
    // any blob with LevelCount above this — keeping every buildable mesh loadable.
    inline constexpr u32 kMaxVirtualMeshLevels = 64;

    // Sphere + object-space error used for view-dependent LOD selection.
    // For groups these are conservative: the sphere of a group contains the spheres of all
    // groups it refines, and the error never decreases from child group to parent group.
    struct VirtualLODBounds
    {
        glm::vec3 Center{ 0.0f };
        f32 Radius = 0.0f;
        f32 Error = 0.0f; // absolute object-space error; FLT_MAX marks a terminal (coarsest) group

        // Approximate perspective-projected screen-space error in [0..1] units of screen
        // height (multiply by viewport height for pixels). Reference formula from
        // meshoptimizer demo/clusterlod.h: error / max(dist - radius, zNear) * (proj * 0.5),
        // where projectionScale is projection[1][1] == cot(fovY / 2) and zNear is the
        // positive near-plane distance. Rotationally invariant (ignores perspective skew).
        [[nodiscard]] f32 ProjectError(const glm::vec3& cameraPosition, f32 zNear, f32 projectionScale) const;
    };

    // A cluster of at most 512 triangles / 256 vertices at some level of the LOD DAG.
    // Geometry is stored meshlet-style: local u8 triangle indices into a per-cluster window
    // of vertex references, which point into VirtualMesh::Vertices.
    struct VirtualCluster
    {
        u32 VertexOffset = 0;   // first entry in VirtualMesh::ClusterVertexRefs
        u32 TriangleOffset = 0; // first byte in VirtualMesh::ClusterTriangles (3 bytes per triangle)
        u32 VertexCount = 0;
        u32 TriangleCount = 0;

        i32 GroupIndex = -1;   // group this cluster is a member of (always valid in a built mesh)
        i32 RefinedGroup = -1; // group whose simplification produced this cluster; -1 for LOD-0 (leaf) clusters

        // Tight culling bounds (sphere + backface normal cone, meshoptimizer convention:
        // reject when dot(normalize(ConeApex - camera), ConeAxis) >= ConeCutoff).
        // NOT monotone across the DAG — use the group LODBounds for LOD selection.
        glm::vec3 BoundsCenter{ 0.0f };
        f32 BoundsRadius = 0.0f;
        glm::vec3 ConeApex{ 0.0f };
        glm::vec3 ConeAxis{ 0.0f };
        f32 ConeCutoff = 1.0f;
    };

    // A simplification group: the set of clusters that were merged and simplified together.
    // Every cluster is emitted as a member of exactly one group; a group's members are
    // contiguous in VirtualMesh::Clusters ([FirstCluster, FirstCluster + ClusterCount)).
    struct VirtualClusterGroup
    {
        u32 Depth = 0; // DAG level the group was formed at (0 = groups of leaf clusters)
        u32 FirstCluster = 0;
        u32 ClusterCount = 0;
        VirtualLODBounds LODBounds; // conservative merged sphere + post-simplification error (monotone)
    };

    struct VirtualMesh
    {
        std::vector<Vertex> Vertices; // compacted copy of the referenced source vertices
        std::vector<VirtualCluster> Clusters;
        std::vector<VirtualClusterGroup> Groups;
        std::vector<u32> ClusterVertexRefs; // per-cluster references into Vertices
        std::vector<u8> ClusterTriangles;   // per-cluster local triangle indices (3 per triangle)
        u32 LevelCount = 0;                 // number of DAG levels (max group Depth + 1)
        u32 SourceTriangleCount = 0;

        [[nodiscard]] bool IsValid() const
        {
            return !Clusters.empty() && !Groups.empty();
        }

        // Reference CPU implementation of the DAG cut rule (see the selection contract
        // above). errorThreshold is an absolute object-space error; pass a negative
        // threshold to select exactly the LOD-0 (leaf) clusters.
        [[nodiscard]] bool IsClusterSelected(u32 clusterIndex, f32 errorThreshold) const;
        [[nodiscard]] std::vector<u32> SelectClusters(f32 errorThreshold) const;

        // Same cut, but with the per-group errors projected to screen space first
        // (VirtualLODBounds::ProjectError); threshold is in [0..1] screen-height units.
        [[nodiscard]] bool IsClusterSelectedProjected(u32 clusterIndex, const glm::vec3& cameraPosition,
                                                      f32 zNear, f32 projectionScale, f32 threshold) const;
        [[nodiscard]] std::vector<u32> SelectClustersProjected(const glm::vec3& cameraPosition,
                                                               f32 zNear, f32 projectionScale, f32 threshold) const;
    };

    // One DAG plus the submesh/material it belongs to.
    //
    // Clusters must never span a material boundary: a group is simplified as a unit, so a
    // cluster straddling two materials could not be shaded by either. Real Nanite has the
    // same constraint. So a multi-submesh source is built as ONE DAG PER SUBMESH, and each
    // part is drawn as its own GPU instance with its own material — which is why supporting
    // multi-material meshes needed no shader change: the per-instance machinery (cluster
    // range + material slot) already existed.
    struct VirtualMeshPart
    {
        VirtualMesh Dag;
        u32 SubmeshIndex = 0;  // index into MeshSource::GetSubmeshes()
        u32 MaterialIndex = 0; // index into MeshSource::GetImportedMaterials()
    };

    // Every buildable submesh of one source mesh. Parts whose submesh the builder rejects
    // (degenerate, too few triangles) are simply absent, so Parts.size() can be < the
    // submesh count — the remaining parts still render.
    struct VirtualMeshSet
    {
        std::vector<VirtualMeshPart> Parts;

        [[nodiscard]] bool IsValid() const
        {
            return !Parts.empty();
        }
        [[nodiscard]] u32 TotalSourceTriangles() const;
        [[nodiscard]] sizet TotalClusters() const;
    };

    // Versioned binary sidecar blob. All fields are written little-endian native,
    // field-by-field (never raw structs — struct padding would leak uninitialized bytes
    // into the blob and break deterministic cooks). Deserialization treats the input as
    // hostile: exact-size check, count caps, finite-float validation, and full
    // cross-referencing of every offset.
    //
    // Two formats, distinguished by magic:
    //   "OVGM" — a single DAG. The original single-submesh format.
    //   "OVGS" — a SET: a count plus one length-prefixed "OVGM" blob per part. The set
    //            format simply wraps the single-mesh one, so the hardened OVGM reader
    //            validates every part and there is no second parser to keep in sync.
    // DeserializeSetFromBlob accepts BOTH, reading a bare "OVGM" blob as a one-part set,
    // so cooks written before multi-submesh support still load.
    namespace VirtualMeshSerializer
    {
        [[nodiscard]] std::vector<u8> SerializeToBlob(const VirtualMesh& mesh);
        [[nodiscard]] bool DeserializeFromBlob(std::span<const u8> blob, VirtualMesh& out);

        [[nodiscard]] std::vector<u8> SerializeSetToBlob(const VirtualMeshSet& set);
        [[nodiscard]] bool DeserializeSetFromBlob(std::span<const u8> blob, VirtualMeshSet& out);
    } // namespace VirtualMeshSerializer
} // namespace OloEngine
