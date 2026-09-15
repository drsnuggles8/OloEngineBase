#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Vertex.h"

#include <glm/vec2.hpp>
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

    // ── Skinned virtual geometry (issue #1150) ────────────────────────────────
    //
    // The number of DISTINCT bones one cluster's vertices may reference before
    // the cluster gives up its own deformed bound.
    //
    // A cluster is ~128 vertices of one small patch of surface, and a patch that
    // small is normally influenced by a handful of bones — so eight is generous
    // rather than tight. What matters is that the list is FIXED-WIDTH: it lets
    // the cook address a cluster's bone set as `clusterIndex * kMaxClusterBones`
    // with no base/count pair in VirtualCluster (which has one spare word, not
    // two) and no second level of indirection in the cull shader.
    //
    // A cluster that exceeds it is not an error and not a build failure: its
    // list is emitted as ALL-SENTINEL, which the cull reads as "I cannot bound
    // this cluster tightly" and falls back to the instance-wide conservative
    // bound. Loss of culling precision, never loss of geometry.
    inline constexpr u32 kMaxClusterBones = 8;

    // Empty slot in a cluster's bone list. Also the whole-list value for a
    // cluster whose bone set did not fit (see kMaxClusterBones).
    inline constexpr u32 kNoClusterBone = 0xFFFFFFFFu;

    // Per-vertex skin binding, parallel to VirtualMesh::Vertices. The same four
    // influences MeshSource::BoneInfluence carries, kept in the cook so the
    // virtual path never has to reach back into the source mesh.
    struct VirtualVertexSkinning
    {
        u32 BoneIDs[4] = { 0, 0, 0, 0 };
        f32 Weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    // Rest-pose (object-space) bounding sphere of every vertex one bone
    // influences — the static half of the conservative-bounds contract below.
    //
    // Radius < 0 marks a bone no vertex of this mesh binds to; such a bone
    // contributes nothing to any bound no matter how it moves.
    struct VirtualBoneBounds
    {
        glm::vec3 Center{ 0.0f };
        f32 Radius = -1.0f;

        [[nodiscard]] bool Influences() const
        {
            return Radius >= 0.0f;
        }
    };

    // Cook identity (issue #629). The blob's own version guards the WIRE FORMAT; this guards
    // the COOK — the geometry the builder produced.
    //
    // A cooked DAG rides inside the mesh's `.omesh` cache entry, whose validity is only
    // (source path hash, flipUV, source mtime). Nothing there notices that the BUILDER
    // changed. And because the DAG bakes its own copy of the vertex data, a stale cook is
    // fully self-consistent: it passes every structural validation in the deserializer and
    // then quietly draws different geometry from the classic path, forever, until someone
    // touches the source file or hand-deletes the cache. That cost real time during #629.
    //
    // So the blob records this version plus a fingerprint of the build config it was cooked
    // with, and the reader REJECTS a mismatch — which drops the registry onto its existing
    // runtime-build fallback (VirtualMeshRegistry::RegisterMeshSource).
    //
    // >>> BUMP THIS whenever VirtualMeshBuilder's clusterization/simplification/emission
    //     changes in a way that alters the produced geometry. <<<
    // (A change to VirtualMeshBuildConfig's DEFAULTS needs no bump — the fingerprint below
    //  already covers it.)
    // v2 (issue #685): the builder was aligned with meshoptimizer v1.2's stable clusterlod
    // reference — permissive simplification with UV-seam protect bits replaced the
    // unconditional position weld, and a watertightness-guarded sloppy pass was added as a
    // last-resort fallback. Every DAG's geometry changes, so v1 caches must be rejected.
    // v3 (issue #867): the attribute set handed to meshopt_simplifyWithAttributes grew from
    // 5 floats to 7 (the baked lightmap UV2 pair) and the protect window widened to cover
    // them, so a UV2 chart seam is now a wedge the simplifier may not collapse across.
    //
    // Bumped even though a mesh with NO UV2 fills those two slots with a constant and should
    // therefore simplify identically: the quadric accumulates over seven floats instead of
    // five, float addition is not associative, and a cached DAG that differs from what this
    // builder would now produce is exactly what the cook fingerprint exists to reject. A
    // "probably identical" cache is not a contract.
    // v4 (issue #1150): the builder no longer rejects skinned sources, so a mesh
    // that previously produced NO cook now produces one — and, more to the
    // point, the vertex compaction now carries a skin binding per vertex and the
    // emission computes per-cluster bone sets. A v3 blob for a skinned source
    // cannot exist, but a v3 blob for a RIGID source cooked by a builder that
    // still had the rejection is indistinguishable from one cooked by this
    // builder only if the rigid path is byte-identical — and it is not required
    // to be, so the version moves rather than being argued about.
    inline constexpr u32 kVirtualMeshBuilderVersion = 4;

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
        // Baked lightmap UV2, one per entry of Vertices, or EMPTY when the
        // source had none (issue #867).
        //
        // A PARALLEL array rather than a wider Vertex, for the same reason
        // MeshSource keeps m_LightmapUVs beside its vertices
        // (docs/agent-rules/baked-lightmap-pipeline.md §1): Vertex is 32 bytes
        // with three pinned offsets and ~38 shaders whose vertex-pull branch
        // hard-codes that stride. Only lightmapped meshes pay, and an unbaked
        // virtual mesh is byte-identical to what it cooked before.
        std::vector<glm::vec2> LightmapUVs;
        std::vector<VirtualCluster> Clusters;
        std::vector<VirtualClusterGroup> Groups;
        std::vector<u32> ClusterVertexRefs; // per-cluster references into Vertices
        std::vector<u8> ClusterTriangles;   // per-cluster local triangle indices (3 per triangle)

        // ── Skinning payload (issue #1150), all EMPTY for a rigid cook ────────
        //
        // The three arrays are all-or-nothing together: a cook either carries
        // the whole skinning payload or none of it, and IsSkinned() is the one
        // predicate every consumer asks. A partial payload would deform some
        // vertices and not others, which reads as a mesh tearing itself apart
        // rather than as a load failure — so the deserializer rejects it.

        // One per entry of Vertices. A PARALLEL array for exactly the reason
        // LightmapUVs is one: Vertex is 32 bytes with three pinned offsets and
        // the virtual path's packed GPU vertex mirrors it, so only skinned
        // meshes pay for the extra stream.
        std::vector<VirtualVertexSkinning> Skinning;

        // One per bone SLOT of the source skeleton (indexed by the same bone id
        // Skinning::BoneIDs carries), so a slot no vertex binds to is present
        // and marked non-influencing rather than shifting every later index.
        std::vector<VirtualBoneBounds> BoneBounds;

        // kMaxClusterBones entries per cluster, cluster-major: cluster k's set
        // is [k * kMaxClusterBones, (k + 1) * kMaxClusterBones). Unused slots —
        // and every slot of a cluster whose set overflowed — are kNoClusterBone.
        std::vector<u32> ClusterBoneRefs;

        [[nodiscard]] bool IsSkinned() const
        {
            return !Skinning.empty();
        }
        u32 LevelCount = 0; // number of DAG levels (max group Depth + 1)
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

        // The absolute object-space threshold that selects the COARSEST cut,
        // i.e. the DAG's root clusters. Every finite group error is at or under
        // it and only the terminal (FLT_MAX) groups are over it, which is
        // exactly rule 1 of the selection contract at its limit.
        //
        // Derived from the group errors rather than hard-coded to some large
        // constant: a mesh authored in centimetres has errors three orders of
        // magnitude larger than the same mesh in metres, so a fixed threshold
        // picks a different cut per unit system while this one does not.
        [[nodiscard]] f32 CoarsestCutThreshold() const;

        // The coarsest watertight cut, through the same tested rule as every
        // other cut (SelectClusters) rather than a second "walk the terminal
        // groups" implementation — the two would be free to disagree, and a
        // cut that disagrees with the rule is a cracked surface.
        [[nodiscard]] std::vector<u32> SelectCoarsestCut() const;
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
        // Fingerprint of the cook this build produces: kVirtualMeshBuilderVersion mixed with a
        // hash of the DEFAULT (sanitized) VirtualMeshBuildConfig — the config every cook path
        // uses. Written into every blob header and required to match on read, so changing the
        // builder or a config default invalidates every previously cached DAG instead of
        // leaving it to render forever. Exposed for the round-trip tests.
        [[nodiscard]] u32 CurrentCookFingerprint();

        [[nodiscard]] std::vector<u8> SerializeToBlob(const VirtualMesh& mesh);
        [[nodiscard]] bool DeserializeFromBlob(std::span<const u8> blob, VirtualMesh& out);

        [[nodiscard]] std::vector<u8> SerializeSetToBlob(const VirtualMeshSet& set);
        [[nodiscard]] bool DeserializeSetFromBlob(std::span<const u8> blob, VirtualMeshSet& out);
    } // namespace VirtualMeshSerializer
} // namespace OloEngine
