#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"

#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualSkinningPacking.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace OloEngine::VirtualMeshBuilder
{
    // An in-flight cluster during the DAG build. Indices are absolute references into
    // the compacted vertex array; LODBounds carries the conservative sphere + error
    // used for group merging (NOT the tight bounds � see clusterlod.h: using precise
    // bounds of simplified geometry would violate monotonicity).
    struct BuildCluster
    {
        TArray<u32> Indices;
        u32 UniqueVertexCount = 0;
        i32 RefinedGroup = -1;
        VirtualLODBounds LODBounds;
    };
} // namespace OloEngine::VirtualMeshBuilder

namespace OloEngine
{
    // Cluster ownership is entirely in its heap-backed index array; bounds/IDs are numerical.
    template<>
    struct TIsTriviallyRelocatable<VirtualMeshBuilder::BuildCluster>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(VirtualMeshBuilder::BuildCluster::Indices)>::Value &&
                                      TIsTriviallyRelocatable<decltype(VirtualMeshBuilder::BuildCluster::UniqueVertexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(VirtualMeshBuilder::BuildCluster::RefinedGroup)>::Value &&
                                      TIsTriviallyRelocatable<decltype(VirtualMeshBuilder::BuildCluster::LODBounds)>::Value;
    };
} // namespace OloEngine

namespace OloEngine::VirtualMeshBuilder
{
    namespace
    {
        // meshopt_computeSphereBounds / the positions pointer below rely on this exact layout.
        static_assert(offsetof(VirtualLODBounds, Center) == 0, "sphere center must be the first 12 bytes");
        static_assert(offsetof(VirtualLODBounds, Radius) == 12, "radius must directly follow the center");
        static_assert(offsetof(Vertex, Position) == 0, "positions must be the first 12 bytes of Vertex");

        // Attribute layout for attribute-aware simplification: nx ny nz u v u2 v2.
        // Weights match MeshOptimization::GenerateLODMeshWithAttributes (normals matter
        // less than UVs for visual quality).
        // Slots 5..6 are the baked lightmap UV2 (issue #867) and are FILLED WITH
        // ZERO on a mesh that has none — a constant attribute contributes nothing
        // to the quadric, so an unbaked cook simplifies exactly as it did before.
        constexpr sizet kAttributeCount = 7;
        constexpr std::array<f32, kAttributeCount> kAttributeWeights = { 0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f };

        // Which attributes count as a protected discontinuity under permissive simplification.
        // Mirrors the reference's clodMesh::attribute_protect_mask, which demo/nanite.cpp sets
        // to (1 << 3) | (1 << 4) — the UV pair — for exactly this attribute layout. NORMALS are
        // deliberately unprotected: a normal wedge is what flat shading produces everywhere, and
        // decimated geometry wants smoothed normals, not per-face-flat ones.
        // Extended to 4 for the lightmap UV2 pair (issue #867): a UV2 chart seam is
        // a discontinuity for exactly the reason a UV0 seam is — collapsing across
        // it welds two atlas charts, and the surviving wedge then addresses the
        // wrong texels. The bake's own unwrap SEAM-SPLITS vertices to create those
        // wedges, so leaving them unprotected would let the simplifier undo the
        // parameterization the atlas was rasterized against.
        constexpr sizet kFirstProtectedAttribute = 3;
        constexpr sizet kProtectedAttributeCount = 4;

        constexpr i32 kOwnerUnset = -2;
        constexpr i32 kOwnerShared = -1;

        [[nodiscard]] VirtualMeshBuildConfig Sanitize(const VirtualMeshBuildConfig& config)
        {
            VirtualMeshBuildConfig out = config;
            out.MaxClusterTriangles = std::clamp(out.MaxClusterTriangles, 1u, 512u);
            out.MaxClusterVertices = std::clamp(out.MaxClusterVertices, 3u, 256u);
            out.MinClusterTriangles = (out.MinClusterTriangles == 0)
                                          ? std::max(1u, out.MaxClusterTriangles / 3)
                                          : std::clamp(out.MinClusterTriangles, 1u, out.MaxClusterTriangles);
            out.TargetGroupSize = std::clamp(out.TargetGroupSize, 2u, 1024u);
            if (!std::isfinite(out.SimplifyRatio))
            {
                out.SimplifyRatio = 0.5f;
            }
            out.SimplifyRatio = std::clamp(out.SimplifyRatio, 0.1f, 0.9f);
            if (!std::isfinite(out.StuckThreshold))
            {
                out.StuckThreshold = 0.85f;
            }
            out.StuckThreshold = std::clamp(out.StuckThreshold, 0.5f, 0.99f);
            if (!std::isfinite(out.ClusterSplitFactor) || out.ClusterSplitFactor < 0.0f)
            {
                out.ClusterSplitFactor = 2.0f;
            }
            out.ClusterSplitFactor = std::min(out.ClusterSplitFactor, 16.0f);
            // A terminal group can sit at Depth == MaxLevels (the cap/root paths), so
            // LevelCount <= MaxLevels + 1; clamp against the shared blob-format cap so
            // every buildable mesh stays deserializable.
            out.MaxLevels = std::clamp(out.MaxLevels, 1u, kMaxVirtualMeshLevels - 1);
            return out;
        }

        // An in-flight cluster during the DAG build. Indices are absolute references into
        // the compacted vertex array; LODBounds carries the conservative sphere + error
        // used for group merging (NOT the tight bounds — see clusterlod.h: using precise
        // bounds of simplified geometry would violate monotonicity).

        struct BuildContext
        {
            VirtualMeshBuildConfig Config;
            const Vertex* Vertices = nullptr;
            sizet VertexCount = 0;
            TArray<f32> Attributes;    // kAttributeCount floats per vertex
            TArray<u32> PositionRemap; // canonical index per vertex (same position => same index)
            TArray<u8> ProtectBits;    // meshopt_SimplifyVertex_Protect per vertex, computed once
            // Canonical positions touched by an already-emitted TERMINAL group. Non-empty only
            // once some group has gone terminal; see FreezeTerminalGroupBoundary.
            TArray<u8> FrozenPositions;

            [[nodiscard]] const f32* Positions() const
            {
                return &Vertices[0].Position.x;
            }
        };

        // Marks the vertices that sit on a genuine ATTRIBUTE discontinuity, mirroring the
        // attribute_protect_mask pass at the top of clusterlod.h's clodBuild.
        //
        // meshopt_SimplifyPermissive lets the simplifier collapse across attribute wedges — that
        // is the whole point of it — but a UV seam is not a wedge we want welded shut: collapsing
        // it drags texture coordinates across the seam and smears the texture on every simplified
        // level. meshopt_SimplifyVertex_Protect exempts exactly those vertices, so the seam
        // survives while the incidental normal wedges collapse freely.
        //
        // The comparison is BIT-EXACT on purpose, and this is one of the rare places where that
        // is the correct float test rather than a violation of the float-comparison rule: the
        // question is "did the importer store the same UV at these two co-located vertices",
        // which is an identity question, not a proximity one. A tolerance would weld narrow-but-
        // real seams, and `!=` (as the reference uses) would mark every NaN UV as a seam because
        // NaN != NaN — a bit compare treats two identical NaNs as identical.
        void ComputeAttributeProtectBits(BuildContext& ctx)
        {
            ctx.ProtectBits = TArray<u8>(static_cast<i32>(ctx.VertexCount), 0);

            auto sameBits = [](f32 lhs, f32 rhs)
            {
                return std::bit_cast<u32>(lhs) == std::bit_cast<u32>(rhs);
            };

            for (sizet i = 0; i < ctx.VertexCount; ++i)
            {
                sizet const canonical = ctx.PositionRemap[i];
                if (canonical == i)
                {
                    continue; // the representative itself defines the canonical attributes
                }

                for (sizet a = kFirstProtectedAttribute; a < kFirstProtectedAttribute + kProtectedAttributeCount; ++a)
                {
                    if (!sameBits(ctx.Attributes[(i * kAttributeCount) + a],
                                  ctx.Attributes[(canonical * kAttributeCount) + a]))
                    {
                        ctx.ProtectBits[i] = meshopt_SimplifyVertex_Protect;
                        break;
                    }
                }
            }
        }

        [[nodiscard]] TArray<BuildCluster> Clusterize(const BuildContext& ctx, const u32* indices, sizet indexCount)
        {
            sizet const maxMeshlets = meshopt_buildMeshletsBound(indexCount, ctx.Config.MaxClusterVertices,
                                                                 ctx.Config.MinClusterTriangles);
            TArray<meshopt_Meshlet> meshlets(maxMeshlets);
            TArray<u32> meshletVertices(indexCount);
            TArray<u8> meshletTriangles(indexCount);

            sizet const meshletCount = meshopt_buildMeshletsFlex(
                meshlets.GetData(), meshletVertices.GetData(), meshletTriangles.GetData(),
                indices, indexCount,
                ctx.Positions(), ctx.VertexCount, sizeof(Vertex),
                ctx.Config.MaxClusterVertices, ctx.Config.MinClusterTriangles, ctx.Config.MaxClusterTriangles,
                0.0f, ctx.Config.ClusterSplitFactor);

            TArray<BuildCluster> clusters(meshletCount);
            for (sizet i = 0; i < meshletCount; ++i)
            {
                const meshopt_Meshlet& meshlet = meshlets[i];

                // Improves triangle/vertex locality inside the cluster (rasterizer- and
                // compression-friendly ordering for the later GPU consumption slices).
                meshopt_optimizeMeshletLevel(&meshletVertices[meshlet.vertex_offset], meshlet.vertex_count,
                                             &meshletTriangles[meshlet.triangle_offset], meshlet.triangle_count, 1);

                BuildCluster& cluster = clusters[i];
                cluster.UniqueVertexCount = meshlet.vertex_count;
                sizet const clusterIndexCount = static_cast<sizet>(meshlet.triangle_count) * 3;
                cluster.Indices.SetNum(clusterIndexCount, EAllowShrinking::No);
                for (sizet j = 0; j < clusterIndexCount; ++j)
                {
                    cluster.Indices[j] = meshletVertices[meshlet.vertex_offset + meshletTriangles[meshlet.triangle_offset + j]];
                }
            }

            return clusters;
        }

        [[nodiscard]] VirtualLODBounds TightLODBounds(const BuildContext& ctx, const TArray<u32>& indices, f32 error)
        {
            meshopt_Bounds const bounds = meshopt_computeClusterBounds(
                indices.GetData(), static_cast<sizet>(indices.Num()), ctx.Positions(), ctx.VertexCount, sizeof(Vertex));

            VirtualLODBounds result;
            result.Center = { bounds.center[0], bounds.center[1], bounds.center[2] };
            result.Radius = bounds.radius;
            result.Error = error;
            return result;
        }

        // Conservative group bounds: a sphere-of-spheres over the member clusters' LOD
        // bounds plus the max member error. Deliberately NOT the precise bounds of the
        // merged/simplified geometry — that would break projected-error monotonicity.
        [[nodiscard]] VirtualLODBounds MergeLODBounds(const TArray<BuildCluster>& clusters, const TArray<u32>& members)
        {
            TArray<VirtualLODBounds> memberBounds;
            memberBounds.Reserve(static_cast<sizet>(members.Num()));
            for (u32 const member : members)
            {
                memberBounds.Add(clusters[member].LODBounds);
            }

            meshopt_Bounds const merged = meshopt_computeSphereBounds(
                &memberBounds[0].Center.x, static_cast<sizet>(memberBounds.Num()), sizeof(VirtualLODBounds),
                &memberBounds[0].Radius, sizeof(VirtualLODBounds));

            VirtualLODBounds result;
            result.Center = { merged.center[0], merged.center[1], merged.center[2] };
            result.Radius = merged.radius;
            result.Error = 0.0f;
            for (const VirtualLODBounds& bounds : memberBounds)
            {
                result.Error = std::max(result.Error, bounds.Error);
            }
            return result;
        }

        // Groups pending clusters into partitions of ~TargetGroupSize clusters that share
        // canonical (position-remapped) vertices or are spatially close.
        [[nodiscard]] TArray<TArray<u32>> Partition(const BuildContext& ctx,
                                                    const TArray<BuildCluster>& clusters,
                                                    const TArray<u32>& pending)
        {
            if (static_cast<sizet>(pending.Num()) <= ctx.Config.TargetGroupSize)
            {
                return { pending };
            }

            sizet const pendingCount = static_cast<sizet>(pending.Num());
            TArray<u32> flatIndices;
            TArray<u32> flatCounts(pendingCount);
            {
                sizet totalIndexCount = 0;
                for (u32 const clusterIndex : pending)
                {
                    totalIndexCount += static_cast<sizet>(clusters[clusterIndex].Indices.Num());
                }
                flatIndices.Reserve(totalIndexCount);
            }
            for (sizet i = 0; i < pendingCount; ++i)
            {
                const BuildCluster& cluster = clusters[pending[i]];
                flatCounts[i] = static_cast<u32>(cluster.Indices.Num());
                for (u32 const vertexIndex : cluster.Indices)
                {
                    flatIndices.Add(ctx.PositionRemap[vertexIndex]);
                }
            }

            TArray<u32> partitionIds(pendingCount);
            sizet const partitionCount = meshopt_partitionClusters(
                partitionIds.GetData(), flatIndices.GetData(), static_cast<sizet>(flatIndices.Num()), flatCounts.GetData(), static_cast<sizet>(flatCounts.Num()),
                ctx.Positions(), ctx.VertexCount, sizeof(Vertex), ctx.Config.TargetGroupSize);

            TArray<TArray<u32>> partitions(partitionCount);
            for (sizet i = 0; i < pendingCount; ++i)
            {
                partitions[partitionIds[i]].Add(pending[i]);
            }

            partitions.RemoveAll([](const TArray<u32>& partition)
                                 { return partition.IsEmpty(); });
            return partitions;
        }

        // Locks every vertex whose canonical position is used by more than one partition,
        // so group simplification cannot move the shared boundary — the crack-free
        // guarantee between neighbouring groups (and, transitively, across LOD levels).
        void LockGroupBoundaries(const BuildContext& ctx, const TArray<BuildCluster>& clusters,
                                 const TArray<TArray<u32>>& partitions, TArray<u8>& locks)
        {
            TArray<i32> owner(ctx.VertexCount, kOwnerUnset);
            sizet const partitionCount = static_cast<sizet>(partitions.Num());
            for (sizet p = 0; p < partitionCount; ++p)
            {
                for (u32 const clusterIndex : partitions[p])
                {
                    for (u32 const vertexIndex : clusters[clusterIndex].Indices)
                    {
                        i32& canonicalOwner = owner[ctx.PositionRemap[vertexIndex]];
                        if (canonicalOwner == kOwnerUnset)
                        {
                            canonicalOwner = static_cast<i32>(p);
                        }
                        else if (canonicalOwner != static_cast<i32>(p))
                        {
                            canonicalOwner = kOwnerShared;
                        }
                    }
                }
            }

            // The lock bit is decided per canonical POSITION (so every wedge of a shared position
            // is locked consistently, which the simplifier requires), while the protect bit is
            // per VERTEX — it marks the individual wedge whose attributes differ. Same split as
            // the reference's lockBoundary(): `locks[i] = (locks[r] & 1) | (locks[i] & Protect)`.
            //
            // FrozenPositions is OR-ed in on top: a terminal group's boundary must stay locked
            // for the rest of the build, not just the level it was emitted at.
            const bool haveFrozen = !ctx.FrozenPositions.IsEmpty();
            for (sizet v = 0; v < ctx.VertexCount; ++v)
            {
                u32 const canonical = ctx.PositionRemap[v];
                bool const shared = owner[canonical] == kOwnerShared;
                bool const frozen = haveFrozen && ctx.FrozenPositions[canonical] != 0;
                u8 const lock = (shared || frozen) ? meshopt_SimplifyVertex_Lock : 0;
                locks[v] = static_cast<u8>(lock | ctx.ProtectBits[v]);
            }
        }

        // Pins the geometry of a group that is about to be emitted as TERMINAL.
        //
        // A terminal group carries FLT_MAX error, so it is selected at EVERY threshold and is
        // never refined away. Its neighbours, however, keep simplifying at later levels — and
        // LockGroupBoundaries only sees the clusters still in `pending`. Once a terminal group's
        // clusters are emitted they leave `pending` for good, so from the next level on, the
        // vertices along the boundary it shares with its neighbours look interior, get no lock,
        // and are free to move. The neighbour's surface then pulls away from a boundary that is
        // pinned forever, and the coarse cuts crack along exactly that seam.
        //
        // This is why the freeze is PERSISTENT (a build-lifetime set) rather than another
        // per-level computation: the constraint outlives the level that created it. Marking
        // every canonical position the group references is the conservative choice and costs
        // nothing extra in practice — a position interior to the terminal group is referenced by
        // no surviving cluster, so the only ones that ever affect a later simplification are the
        // shared boundary positions we actually need pinned.
        void FreezeTerminalGroupBoundary(BuildContext& ctx, const TArray<BuildCluster>& clusters,
                                         const TArray<u32>& members)
        {
            if (ctx.FrozenPositions.IsEmpty())
            {
                ctx.FrozenPositions = TArray<u8>(static_cast<i32>(ctx.VertexCount), 0);
            }

            for (u32 const member : members)
            {
                for (u32 const vertexIndex : clusters[member].Indices)
                {
                    ctx.FrozenPositions[ctx.PositionRemap[vertexIndex]] = 1;
                }
            }
        }

        // Census of UV-degenerate triangles in a finished DAG, split by where they came from
        // (issue #629). A UV-degenerate triangle (zero texture-space area, real 3D area) makes
        // the derivative tangent frame collapse; the shader guards it, but the count matters:
        // LOD-0 clusters inherit them from the source mesh verbatim, while SIMPLIFICATION
        // creates more — meshopt_simplifyWithAttributes penalises UV *deviation* through the
        // attribute quadric but has no notion of texture AREA, so nothing stops a collapse
        // that makes three corners UV-collinear. There is no meshopt flag/weight that forbids
        // it, so this reports rather than prevents; see the shader guard in PBRCommon.glsl.
        void ReportUvDegenerates(const VirtualMesh& mesh, u32 submeshIndex)
        {
            u32 leafDegenerate = 0; // in LOD-0 clusters (RefinedGroup == -1) — inherited from the source
            u32 leafTriangles = 0;
            u32 refinedDegenerate = 0; // in simplified clusters — created by the simplifier
            u32 refinedTriangles = 0;

            for (const VirtualCluster& cluster : mesh.Clusters)
            {
                const bool isLeaf = cluster.RefinedGroup < 0;
                (isLeaf ? leafTriangles : refinedTriangles) += cluster.TriangleCount;

                for (u32 t = 0; t < cluster.TriangleCount; ++t)
                {
                    const u8* tri = &mesh.ClusterTriangles[cluster.TriangleOffset + (static_cast<sizet>(t) * 3)];
                    const Vertex& v0 = mesh.Vertices[mesh.ClusterVertexRefs[cluster.VertexOffset + tri[0]]];
                    const Vertex& v1 = mesh.Vertices[mesh.ClusterVertexRefs[cluster.VertexOffset + tri[1]]];
                    const Vertex& v2 = mesh.Vertices[mesh.ClusterVertexRefs[cluster.VertexOffset + tri[2]]];

                    const f32 area3D = glm::length(glm::cross(v1.Position - v0.Position, v2.Position - v0.Position));
                    const glm::vec2 duv1 = v1.TexCoord - v0.TexCoord;
                    const glm::vec2 duv2 = v2.TexCoord - v0.TexCoord;
                    const f32 uvArea = std::abs((duv1.x * duv2.y) - (duv2.x * duv1.y));

                    if ((area3D > 0.0f) && !(uvArea > 0.0f))
                    {
                        ++(isLeaf ? leafDegenerate : refinedDegenerate);
                    }
                }
            }

            if (leafDegenerate + refinedDegenerate > 0)
            {
                OLO_CORE_WARN("VirtualMeshBuilder: submesh {} carries {} UV-degenerate triangles "
                              "({} of {} inherited from the source in LOD-0 clusters, {} of {} CREATED by "
                              "simplification). They shade with the geometric normal (PBRCommon.glsl guards "
                              "the collapsed tangent); no meshoptimizer option prevents the collapse.",
                              submeshIndex, leafDegenerate + refinedDegenerate,
                              leafDegenerate, leafTriangles, refinedDegenerate, refinedTriangles);
            }
        }

        // Sparse: the group is a small subset of the full mesh. ErrorAbsolute: errors must be
        // comparable across groups and levels for the monotone DAG invariant. Same pair the
        // reference's clod::simplify() always sets.
        constexpr u32 kSimplifyBaseOptions = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;

        [[nodiscard]] TArray<u32> SimplifyAttributed(const BuildContext& ctx, const u32* indices, sizet indexCount,
                                                     const TArray<u8>& locks, sizet targetIndexCount,
                                                     u32 extraOptions, f32& outAbsoluteError)
        {
            TArray<u32> simplified(indexCount);
            f32 error = 0.0f;

            sizet const resultCount = meshopt_simplifyWithAttributes(
                simplified.GetData(), indices, indexCount,
                ctx.Positions(), ctx.VertexCount, sizeof(Vertex),
                ctx.Attributes.GetData(), kAttributeCount * sizeof(f32), kAttributeWeights.data(), kAttributeCount,
                locks.GetData(), targetIndexCount, std::numeric_limits<f32>::max(),
                kSimplifyBaseOptions | extraOptions, &error);

            simplified.SetNum(resultCount, EAllowShrinking::No);
            outAbsoluteError = error;
            return simplified;
        }

        // Deindexed copy of a group for meshopt_simplifySloppy. Mirrors clusterlod.h's
        // SloppyVertex: sloppy has no sparse mode, so running it against the full vertex buffer
        // for one small group is prohibitively expensive.
        struct SloppyVertex
        {
            glm::vec3 Position;
            u32 Id;
        };

        [[nodiscard]] TArray<u32> SimplifySloppy(const BuildContext& ctx, const TArray<u32>& merged,
                                                 const TArray<u8>& locks, sizet targetIndexCount,
                                                 f32& outAbsoluteError)
        {
            TArray<SloppyVertex> subset(static_cast<sizet>(merged.Num()));
            TArray<u8> subsetLocks(static_cast<sizet>(merged.Num()));
            TArray<u32> lod(static_cast<sizet>(merged.Num()));

            for (sizet i = 0; i < static_cast<sizet>(merged.Num()); ++i)
            {
                u32 const v = merged[i];
                subset[i].Position = ctx.Vertices[v].Position;
                subset[i].Id = v;
                subsetLocks[i] = locks[v];
                lod[i] = static_cast<u32>(i);
            }

            f32 relativeError = 0.0f;
            sizet const resultCount = meshopt_simplifySloppy(
                lod.GetData(), lod.GetData(), static_cast<sizet>(lod.Num()),
                &subset[0].Position.x, static_cast<sizet>(subset.Num()), sizeof(SloppyVertex), subsetLocks.GetData(),
                targetIndexCount, std::numeric_limits<f32>::max(), &relativeError);
            lod.SetNum(resultCount, EAllowShrinking::No);

            // simplifySloppy has no absolute-error option, so scale its relative error by the
            // subset extent — the conversion clusterlod.h's simplifyFallback does.
            outAbsoluteError = relativeError * meshopt_simplifyScale(&subset[0].Position.x, static_cast<sizet>(subset.Num()),
                                                                     sizeof(SloppyVertex));

            for (u32& index : lod)
            {
                index = subset[index].Id;
            }
            return lod;
        }

        // Canonical (position-keyed) edge topology of a triangle set: the border edges, plus
        // whether any edge is shared by more than two triangles.
        struct EdgeTopology
        {
            TArray<std::array<u32, 2>> BorderEdges; // sorted; edges used by exactly one triangle
            bool Manifold = true;                   // no edge used by three or more triangles
        };

        [[nodiscard]] EdgeTopology ComputeEdgeTopology(const BuildContext& ctx, const TArray<u32>& indices)
        {
            TArray<std::array<u32, 2>> edges;
            edges.Reserve(static_cast<sizet>(indices.Num()));
            for (sizet i = 0; i + 2 < static_cast<sizet>(indices.Num()); i += 3)
            {
                std::array<u32, 3> const corners = { ctx.PositionRemap[indices[i]],
                                                     ctx.PositionRemap[indices[i + 1]],
                                                     ctx.PositionRemap[indices[i + 2]] };
                for (u32 e = 0; e < 3; ++e)
                {
                    u32 const a = corners[e];
                    u32 const b = corners[(e + 1) % 3];
                    edges.Add({ std::min(a, b), std::max(a, b) });
                }
            }
            std::ranges::sort(edges);

            EdgeTopology result;
            for (sizet i = 0; i < static_cast<sizet>(edges.Num());)
            {
                sizet j = i;
                while (j < static_cast<sizet>(edges.Num()) && edges[j] == edges[i])
                {
                    ++j;
                }
                sizet const count = j - i;
                if (count == 1)
                {
                    result.BorderEdges.Add(edges[i]);
                }
                else if (count > 2)
                {
                    result.Manifold = false;
                }
                i = j;
            }
            return result;
        }

        // Would accepting `simplified` in place of `merged` keep every LOD cut watertight?
        //
        // Only the SLOPPY path needs asking. Regular (edge-collapse) simplification preserves
        // topology by construction and cannot move a locked vertex, so the group's outer boundary
        // survives edge-for-edge. meshopt_simplifySloppy explicitly does NOT preserve topology —
        // it merges vertices into grid cells — so it can fold two surface sheets onto one edge or
        // punch an interior hole. Either one silently breaks the builder's watertight-cut
        // invariant, which clodBuild does not promise and VirtualMeshBuilderTest does.
        //
        // The exact condition is: still edge-manifold, and exactly the border edges it started
        // with. A NEW border edge is a hole; a MISSING one means the boundary shared with the
        // neighbouring group moved, which would crack against a neighbour at a different LOD.
        [[nodiscard]] bool SloppyResultKeepsCutsWatertight(const BuildContext& ctx, const TArray<u32>& merged,
                                                           const TArray<u32>& simplified)
        {
            EdgeTopology const after = ComputeEdgeTopology(ctx, simplified);
            if (!after.Manifold)
            {
                return false;
            }
            return after.BorderEdges == ComputeEdgeTopology(ctx, merged).BorderEdges;
        }

        // How a group's simplification was obtained; reported in the build summary so a cook that
        // quietly leaned on the lossy fallbacks is visible rather than inferred from screenshots.
        enum class SimplifyPath : u8
        {
            Permissive, // attribute-aware, collapses across unprotected attribute wedges
            Welded,     // attribute-aware over a position-welded index buffer
            Sloppy,     // grid-collapse fallback, error amplified
            Stuck,      // nothing reduced it — the group becomes terminal
        };

        struct SimplifyOutcome
        {
            TArray<u32> Indices;
            f32 AbsoluteError = 0.0f;
            SimplifyPath Path = SimplifyPath::Stuck;
        };

        // Merge-and-simplify for one group, as a cascade of progressively lossier entry points.
        // Mirrors clod::simplify()'s simplify_fallback_permissive / simplify_fallback_sloppy
        // chain, with one extra rung (Welded) and one extra guard (the watertightness check).
        [[nodiscard]] SimplifyOutcome Simplify(const BuildContext& ctx, const TArray<u32>& merged,
                                               const TArray<u8>& locks, sizet targetIndexCount)
        {
            SimplifyOutcome outcome;

            // The reference's `if (target_count > indices.size()) return indices;` guard: asking
            // for more triangles than we have is not a simplification, and the result would be
            // scored "stuck" anyway.
            if (targetIndexCount >= static_cast<sizet>(merged.Num()))
            {
                return outcome;
            }

            sizet const stuckLimit = static_cast<sizet>(static_cast<f64>(static_cast<sizet>(merged.Num())) * ctx.Config.StuckThreshold);
            auto reduced = [&](const TArray<u32>& candidate)
            {
                return !candidate.IsEmpty() && static_cast<sizet>(candidate.Num()) <= stuckLimit;
            };

            // Rung 1 — attribute-aware simplification, permissive by default.
            //
            // Permissive is what lets a mesh with MANY attribute wedges per position simplify at
            // all. meshoptimizer classifies a position carrying more than two wedges as
            // Kind_Locked (simplifier.cpp classifyVertices: "more than one vertex maps to this
            // one; we don't have classification available") and never collapses it. That is
            // exactly the shape Assimp produces for any source without normals — aiProcess_
            // GenNormals splits every shared vertex to give each face its own flat normal, and
            // JoinIdenticalVertices cannot re-merge them. Issue #651 measured one 799-triangle
            // group with 2,395 indices over 450 distinct positions and ZERO collapses; the DAG
            // flattened to a single level and virtual geometry drew full source density forever,
            // in the main view and every shadow cascade.
            //
            // meshopt_SimplifyPermissive promotes those Kind_Locked positions to Kind_Complex
            // unless a wedge carries meshopt_SimplifyVertex_Protect (our UV seams — see
            // ComputeAttributeProtectBits) or the vertex is a genuine topological border. Group
            // boundary locks are applied AFTER the permissive pass inside classifyVertices, so
            // permissive can never unlock a group boundary and cannot break watertight cuts.
            u32 const permissiveOption = ctx.Config.SimplifyPermissive ? meshopt_SimplifyPermissive : 0u;
            outcome.Indices = SimplifyAttributed(ctx, merged.GetData(), static_cast<sizet>(merged.Num()), locks, targetIndexCount,
                                                 permissiveOption, outcome.AbsoluteError);
            if (reduced(outcome.Indices))
            {
                outcome.Path = SimplifyPath::Permissive;
                return outcome;
            }

            // Rung 2 — retry over a position-welded index buffer.
            //
            // Welding collapses ALL attribute wedges instead of just the unprotected ones, so the
            // simplifier sees the fully connected surface no matter how the attributes are split.
            // Lossier than permissive (the canonical vertex's UV wins for the whole position, so
            // a protected seam is welded shut too), which is why it is the fallback and not the
            // default — before this alignment it was the ONLY path. Still topology-preserving:
            // the output indices name real vertices, and LOD 0 keeps the unwelded originals.
            if (ctx.Config.SimplifyFallbackWelded && static_cast<sizet>(ctx.PositionRemap.Num()) == ctx.VertexCount)
            {
                TArray<u32> welded(static_cast<sizet>(merged.Num()));
                for (sizet k = 0; k < static_cast<sizet>(merged.Num()); ++k)
                {
                    welded[k] = ctx.PositionRemap[merged[k]];
                }

                f32 weldedError = 0.0f;
                TArray<u32> candidate = SimplifyAttributed(ctx, welded.GetData(), static_cast<sizet>(welded.Num()), locks,
                                                           targetIndexCount, permissiveOption, weldedError);
                if (reduced(candidate))
                {
                    outcome.Indices = std::move(candidate);
                    outcome.AbsoluteError = weldedError;
                    outcome.Path = SimplifyPath::Welded;
                    return outcome;
                }
            }

            // Rung 3 — sloppy grid collapse, accepted only if the cut stays watertight.
            if (ctx.Config.SimplifyFallbackSloppy)
            {
                f32 sloppyError = 0.0f;
                TArray<u32> candidate = SimplifySloppy(ctx, merged, locks, targetIndexCount, sloppyError);
                if (reduced(candidate) && SloppyResultKeepsCutsWatertight(ctx, merged, candidate))
                {
                    outcome.Indices = std::move(candidate);
                    // Amplify to account for appearance degradation the quadric never saw
                    // (clodConfig::simplify_error_factor_sloppy).
                    outcome.AbsoluteError = sloppyError * ctx.Config.SimplifyErrorFactorSloppy;
                    outcome.Path = SimplifyPath::Sloppy;
                    return outcome;
                }
            }

            // Nothing reduced the group — the caller emits it as a terminal group.
            outcome.Indices.Reset();
            outcome.AbsoluteError = 0.0f;
            outcome.Path = SimplifyPath::Stuck;
            return outcome;
        }

        // Appends a group and its member clusters to the output mesh. Consumes (frees) the
        // members' geometry — every cluster is emitted exactly once.
        i32 EmitGroup(VirtualMesh& out, const BuildContext& ctx, TArray<BuildCluster>& clusters,
                      const TArray<u32>& members, u32 depth, const VirtualLODBounds& lodBounds)
        {
            auto groupIndex = static_cast<i32>(out.Groups.Num());

            VirtualClusterGroup group;
            group.Depth = depth;
            group.FirstCluster = static_cast<u32>(out.Clusters.Num());
            group.ClusterCount = static_cast<u32>(members.Num());
            group.LODBounds = lodBounds;
            out.Groups.Add(group);

            std::array<u32, 256> vertexRefs{};
            TArray<u8> localTriangles;
            for (u32 const member : members)
            {
                BuildCluster& cluster = clusters[member];

                // Tight culling bounds (sphere + normal cone) for the emitted cluster.
                meshopt_Bounds const cullBounds = meshopt_computeClusterBounds(
                    cluster.Indices.GetData(), static_cast<sizet>(cluster.Indices.Num()), ctx.Positions(), ctx.VertexCount, sizeof(Vertex));

                localTriangles.SetNum(static_cast<sizet>(cluster.Indices.Num()), EAllowShrinking::No);
                sizet const uniqueVertexCount = meshopt_extractMeshletIndices(
                    vertexRefs.data(), localTriangles.GetData(), cluster.Indices.GetData(), static_cast<sizet>(cluster.Indices.Num()));

                VirtualCluster emitted;
                emitted.VertexOffset = static_cast<u32>(out.ClusterVertexRefs.Num());
                emitted.TriangleOffset = static_cast<u32>(out.ClusterTriangles.Num());
                emitted.VertexCount = static_cast<u32>(uniqueVertexCount);
                emitted.TriangleCount = static_cast<u32>(static_cast<sizet>(cluster.Indices.Num()) / 3);
                emitted.GroupIndex = groupIndex;
                emitted.RefinedGroup = cluster.RefinedGroup;
                emitted.BoundsCenter = { cullBounds.center[0], cullBounds.center[1], cullBounds.center[2] };
                emitted.BoundsRadius = cullBounds.radius;
                emitted.ConeApex = { cullBounds.cone_apex[0], cullBounds.cone_apex[1], cullBounds.cone_apex[2] };
                emitted.ConeAxis = { cullBounds.cone_axis[0], cullBounds.cone_axis[1], cullBounds.cone_axis[2] };
                emitted.ConeCutoff = cullBounds.cone_cutoff;
                out.Clusters.Add(emitted);

                out.ClusterVertexRefs.Append(vertexRefs.data(), static_cast<i32>(uniqueVertexCount));
                out.ClusterTriangles.Append(localTriangles);

                cluster.Indices = TArray<u32>(); // consumed — release the geometry
            }

            return groupIndex;
        }

        // ── Skinning metadata (issue #1150) ──────────────────────────────────
        //
        // Derived from the FINISHED DAG rather than from the source mesh, and
        // that is not an implementation detail: both products below are keyed to
        // the emitted clusters and to the compacted vertex array, neither of
        // which exists until emission is done. Simplification only ever REMOVES
        // vertices, so every vertex any cluster still references carries the
        // binding it was compacted with and no binding has to be re-derived for
        // a coarse level.
        //
        // Both are no-ops on a rigid cook (mesh.Skinning empty), so the rigid
        // path reaches the same VirtualMesh it reached before.
        void ComputeSkinningMetadata(VirtualMesh& mesh)
        {
            if (!mesh.IsSkinned())
            {
                return;
            }

            // A SUBMESH of a rigged mesh can carry no bone weights at all — a
            // static prop parented into a character's FBX is the ordinary case.
            // The compaction above fills its Skinning stream anyway, because
            // "is this source rigged" is a property of the MeshSource and not of
            // the submesh, so without this the part would claim to be skinned
            // and then produce an EMPTY BoneBounds. The blob's all-or-nothing
            // check rejects exactly that shape, which would make the cook
            // permanently unloadable: every load re-cooks the whole DAG and
            // re-fails. The part does not deform, so the honest answer is that
            // it is rigid.
            const bool anyInfluence = std::ranges::any_of(
                mesh.Skinning, [](const VirtualVertexSkinning& binding)
                { return binding.Weights[0] > 0.0f || binding.Weights[1] > 0.0f || binding.Weights[2] > 0.0f ||
                         binding.Weights[3] > 0.0f; });
            if (!anyInfluence)
            {
                mesh.Skinning.Reset();
                mesh.Skinning.Shrink();
                return;
            }

            // (1) Per-bone rest-pose spheres. Two passes because a centroid has
            // to exist before anything can be measured against it; the sphere is
            // centroid + farthest member rather than a minimal enclosing sphere,
            // which is a slightly larger bound computed in a fraction of the
            // time — and every use of it is one-sided (larger is safe).
            u32 boneSlotCount = 0;
            for (const VirtualVertexSkinning& binding : mesh.Skinning)
            {
                for (u32 i = 0; i < 4; ++i)
                {
                    if (binding.Weights[i] > 0.0f)
                    {
                        boneSlotCount = std::max(boneSlotCount, binding.BoneIDs[i] + 1u);
                    }
                }
            }
            // Sized to the highest bone any vertex actually binds, not to the
            // skeleton: a palette longer than this simply has no influence on
            // this mesh, and SkinDisplacementBound walks the shorter of the two.
            mesh.BoneBounds = TArray<VirtualBoneBounds>(static_cast<i32>(boneSlotCount));

            TArray<glm::dvec3> centroidSum(boneSlotCount, glm::dvec3(0.0));
            TArray<u32> centroidCount(boneSlotCount, 0u);
            for (sizet v = 0; v < static_cast<sizet>(mesh.Skinning.Num()); ++v)
            {
                const VirtualVertexSkinning& binding = mesh.Skinning[v];
                for (u32 i = 0; i < 4; ++i)
                {
                    if (binding.Weights[i] <= 0.0f)
                    {
                        continue;
                    }
                    u32 const boneId = binding.BoneIDs[i];
                    centroidSum[boneId] += glm::dvec3(mesh.Vertices[v].Position);
                    ++centroidCount[boneId];
                }
            }
            for (u32 b = 0; b < boneSlotCount; ++b)
            {
                if (centroidCount[b] > 0)
                {
                    mesh.BoneBounds[b].Center = glm::vec3(centroidSum[b] / static_cast<f64>(centroidCount[b]));
                    mesh.BoneBounds[b].Radius = 0.0f; // influences something; radius grown below
                }
            }
            for (sizet v = 0; v < static_cast<sizet>(mesh.Skinning.Num()); ++v)
            {
                const VirtualVertexSkinning& binding = mesh.Skinning[v];
                for (u32 i = 0; i < 4; ++i)
                {
                    if (binding.Weights[i] <= 0.0f)
                    {
                        continue;
                    }
                    VirtualBoneBounds& bounds = mesh.BoneBounds[binding.BoneIDs[i]];
                    bounds.Radius = std::max(bounds.Radius, glm::length(mesh.Vertices[v].Position - bounds.Center));
                }
            }

            // (2) Per-cluster bone sets, fixed width. A cluster whose set does
            // not fit keeps an ALL-SENTINEL list, which the cull reads as "no
            // tight bound available" and answers with the instance-wide one —
            // the reason this is a capacity decision rather than a build
            // failure (see kMaxClusterBones).
            mesh.ClusterBoneRefs = TArray<u32>(mesh.Clusters.Num() * kMaxClusterBones, kNoClusterBone);
            u32 overflowedClusters = 0;
            for (sizet c = 0; c < static_cast<sizet>(mesh.Clusters.Num()); ++c)
            {
                const VirtualCluster& cluster = mesh.Clusters[c];
                sizet const base = c * kMaxClusterBones;
                u32 used = 0;
                bool overflowed = false;
                for (u32 local = 0; local < cluster.VertexCount && !overflowed; ++local)
                {
                    u32 const vertexIndex = mesh.ClusterVertexRefs[cluster.VertexOffset + local];
                    const VirtualVertexSkinning& binding = mesh.Skinning[vertexIndex];

                    // A RIGID vertex inside a skinned mesh forfeits the whole
                    // cluster's tight bound, and this is a correctness rule
                    // rather than a tuning one.
                    //
                    // The deformed sphere is the hull of { M_b * c : b in the
                    // set }. A vertex with no influence does not move, so it
                    // stays at its REST position — which that hull need not
                    // contain once the bones have carried the rest of the
                    // cluster away. There is no bone id that stands for "the
                    // identity", so the honest answer is the instance-wide
                    // bound, which contains a rigid vertex trivially (its
                    // displacement is zero, and zero is under any bound).
                    if (!(binding.Weights[0] > 0.0f || binding.Weights[1] > 0.0f || binding.Weights[2] > 0.0f ||
                          binding.Weights[3] > 0.0f))
                    {
                        overflowed = true;
                        break;
                    }

                    for (u32 i = 0; i < 4; ++i)
                    {
                        if (binding.Weights[i] <= 0.0f)
                        {
                            continue;
                        }
                        u32 const boneId = binding.BoneIDs[i];
                        bool present = false;
                        for (u32 slot = 0; slot < used; ++slot)
                        {
                            present = present || mesh.ClusterBoneRefs[base + slot] == boneId;
                        }
                        if (present)
                        {
                            continue;
                        }
                        if (used == kMaxClusterBones)
                        {
                            overflowed = true;
                            break;
                        }
                        mesh.ClusterBoneRefs[base + used] = boneId;
                        ++used;
                    }
                }
                if (overflowed)
                {
                    std::fill_n(mesh.ClusterBoneRefs.begin() + static_cast<std::ptrdiff_t>(base), kMaxClusterBones,
                                kNoClusterBone);
                    ++overflowedClusters;
                }
            }

            if (overflowedClusters > 0)
            {
                // Not a warning: this is a cost report, not a fault. Those
                // clusters still draw, still cast shadows and still pick the
                // same LOD — they just cull against the whole instance's bound.
                OLO_CORE_TRACE("VirtualMeshBuilder: {} of {} clusters reference more than {} bones (or hold a vertex "
                               "with no influence) and fall back to the instance-wide deformed bound",
                               overflowedClusters, static_cast<sizet>(mesh.Clusters.Num()), kMaxClusterBones);
            }
        }
    } // namespace

    VirtualMesh BuildSubmesh(const MeshSource& meshSource, u32 submeshIndex, const VirtualMeshBuildConfig& config)
    {
        OLO_PROFILE_FUNCTION();

        VirtualMesh result;

        const auto& srcVertices = meshSource.GetVertices();
        const auto& srcIndices = meshSource.GetIndices();
        const auto& submeshes = meshSource.GetSubmeshes();

        if (srcVertices.IsEmpty() || srcIndices.Num() < 3)
        {
            OLO_CORE_WARN("VirtualMeshBuilder: Source mesh has no geometry");
            return result;
        }
        // MORPH TARGETS remain rejected (issue #1150 lifted the SKINNING half only).
        //
        // The two look alike from here and are not. Linear-blend skinning is a
        // per-vertex convex combination of a small, STATIC set of bone
        // transforms, which is what lets VirtualSkinningBounds derive a bound
        // that holds for every pose from data the cook can compute once. A morph
        // target is an arbitrary per-vertex displacement field with a runtime
        // weight: there is no bone set to bound it with, and a conservative
        // bound would have to be "the union of every target's displacement",
        // which is neither small nor cheap to keep watertight across a DAG. It
        // stays a loud rejection with its own message rather than being folded
        // into a generic one — "unsupported" that does not say WHICH feature was
        // the problem is the failure this warning exists to avoid.
        if (meshSource.HasMorphTargets())
        {
            OLO_CORE_WARN("VirtualMeshBuilder: Morph-target sources are not supported (skinning is, issue #1150)");
            return result;
        }

        // Resolve this submesh's slice of the shared index buffer. A source with no submesh
        // records is treated as one implicit submesh covering everything (the shape the CPU
        // unit tests build). Combined-mesh indices are ABSOLUTE — Model offsets them by the
        // base vertex when concatenating — so the range is used directly, not rebased.
        sizet indexBegin = 0;
        sizet indexEnd = static_cast<sizet>(srcIndices.Num());
        if (!submeshes.IsEmpty())
        {
            if (submeshIndex >= static_cast<u32>(submeshes.Num()))
            {
                OLO_CORE_WARN("VirtualMeshBuilder: Submesh index {} out of range ({} submeshes)",
                              submeshIndex, submeshes.Num());
                return result;
            }
            const auto& submesh = submeshes[static_cast<i32>(submeshIndex)];
            indexBegin = static_cast<sizet>(submesh.m_BaseIndex);
            indexEnd = indexBegin + static_cast<sizet>(submesh.m_IndexCount);
            if (indexEnd > static_cast<sizet>(srcIndices.Num()) || indexBegin > indexEnd)
            {
                OLO_CORE_WARN("VirtualMeshBuilder: Submesh {} index range [{}, {}) exceeds the index buffer ({})",
                              submeshIndex, indexBegin, indexEnd, srcIndices.Num());
                return result;
            }
        }
        else if (submeshIndex != 0)
        {
            return result;
        }

        sizet const rangeIndexCount = indexEnd - indexBegin;
        if (rangeIndexCount < 3 || rangeIndexCount % 3 != 0)
        {
            OLO_CORE_WARN("VirtualMeshBuilder: Submesh {} index count {} is empty or not a multiple of 3",
                          submeshIndex, rangeIndexCount);
            return result;
        }

        BuildContext ctx;
        ctx.Config = Sanitize(config);

        // Compact to the vertex set THIS submesh uses, so each part is self-contained and
        // its DAG never references another material's geometry. Simplification creates no
        // new vertices, so this single array serves every LOD level of the part.
        TArray<u32> indices(rangeIndexCount);
        {
            // The lightmap UV2 stream rides the SAME compaction as the vertices
            // (issue #867). MeshSource guarantees the parallel array is either
            // empty or exactly as long as the vertex array (HasLightmapUVs), and
            // a stream that silently fell out of step here would not error — it
            // would address another chart's texels.
            const bool sourceHasLightmapUVs = meshSource.HasLightmapUVs();
            const auto& srcLightmapUVs = meshSource.GetLightmapUVs();

            // The skin bindings ride the SAME compaction, for the same reason
            // (issue #1150). MeshSource pre-allocates m_BoneInfluences to the
            // vertex count for every source, rigged or not, so the length check
            // is what separates "this mesh is skinned" from "this mesh has an
            // empty influence array sized like its vertices" — HasBoneInfluences
            // answers the first, and a stream that fell out of step here would
            // deform vertices by another vertex's bones, which reads as the mesh
            // tearing itself apart rather than as a load failure.
            const bool sourceHasSkinning = meshSource.HasBoneInfluences() &&
                                           meshSource.GetBoneInfluences().Num() == srcVertices.Num();
            const auto& srcInfluences = meshSource.GetBoneInfluences();

            TArray<u32> vertexRemap(static_cast<sizet>(srcVertices.Num()), UINT32_MAX);
            for (sizet i = 0; i < rangeIndexCount; ++i)
            {
                u32 const v = srcIndices[static_cast<i32>(indexBegin + i)];
                if (v >= static_cast<u32>(srcVertices.Num()))
                {
                    OLO_CORE_WARN("VirtualMeshBuilder: Index {} out of range at position {}", v, indexBegin + i);
                    return result;
                }
                if (vertexRemap[v] == UINT32_MAX)
                {
                    vertexRemap[v] = static_cast<u32>(result.Vertices.Num());
                    result.Vertices.Add(srcVertices[static_cast<i32>(v)]);
                    if (sourceHasLightmapUVs)
                    {
                        result.LightmapUVs.Add(srcLightmapUVs[static_cast<i32>(v)]);
                    }
                    if (sourceHasSkinning)
                    {
                        const BoneInfluence& influence = srcInfluences[static_cast<i32>(v)];
                        VirtualVertexSkinning binding;
                        f32 total = 0.0f;
                        for (u32 slot = 0; slot < 4; ++slot)
                        {
                            u32 const boneId = influence.m_BoneIDs[slot];
                            f32 const weight = influence.m_Weights[slot];
                            // Two independent reasons to drop an influence, and
                            // BOTH are about hostile input rather than tidiness.
                            //
                            // A non-finite or negative WEIGHT reaches the GPU as
                            // a multiplier on a bone matrix, where a NaN spreads
                            // to the whole vertex and a negative weight breaks
                            // the convexity every bound in VirtualSkinningBounds
                            // rests on.
                            //
                            // An out-of-range bone ID is worse: these arrive raw
                            // from the asset pack, and the emission below sizes
                            // its per-bone arrays from max(id) + 1 and then
                            // INDEXES them by the same ids. 0xFFFFFFFF wraps that
                            // increment to zero and writes out of bounds; a
                            // merely large id asks for a multi-gigabyte
                            // allocation. Nothing upstream bounds them — this
                            // path was unreachable until the skinned rejection
                            // was lifted. The cap is the one the GPU packing can
                            // address at all (VirtualSkinningPacking.h), so an id
                            // above it could never have been read by a shader.
                            bool const usable = std::isfinite(weight) && weight > 0.0f &&
                                                boneId < kVirtualSkinningMaxBoneId;
                            binding.BoneIDs[slot] = usable ? boneId : 0u;
                            binding.Weights[slot] = usable ? weight : 0.0f;
                            total += binding.Weights[slot];
                        }
                        // Normalize here, once, at cook time. The shaders and
                        // the CPU bound both assume SUM(w) == 1 (that is what
                        // makes a skinned position a CONVEX combination); an
                        // importer that left the weights merely close to 1 would
                        // otherwise scale every vertex slightly toward or away
                        // from the origin, which looks like a modelling error.
                        if (total > 0.0f)
                        {
                            for (f32& weight : binding.Weights)
                            {
                                weight /= total;
                            }
                        }
                        result.Skinning.Add(binding);
                    }
                }
                indices[i] = vertexRemap[v];
            }
        }

        ctx.Vertices = result.Vertices.GetData();
        ctx.VertexCount = static_cast<sizet>(result.Vertices.Num());
        result.SourceTriangleCount = static_cast<u32>(static_cast<sizet>(indices.Num()) / 3);

        ctx.Attributes.SetNum(ctx.VertexCount * kAttributeCount, EAllowShrinking::No);
        const bool hasLightmapUVs = !result.LightmapUVs.IsEmpty();
        for (sizet i = 0; i < ctx.VertexCount; ++i)
        {
            const Vertex& vertex = result.Vertices[i];
            ctx.Attributes[i * kAttributeCount + 0] = vertex.Normal.x;
            ctx.Attributes[i * kAttributeCount + 1] = vertex.Normal.y;
            ctx.Attributes[i * kAttributeCount + 2] = vertex.Normal.z;
            ctx.Attributes[i * kAttributeCount + 3] = vertex.TexCoord.x;
            ctx.Attributes[i * kAttributeCount + 4] = vertex.TexCoord.y;
            // Zero, not garbage, when the mesh has no UV2. A CONSTANT attribute
            // adds nothing to the quadric and marks no vertex as a wedge, so an
            // unbaked cook simplifies to the same geometry it did before #867 —
            // "the same", not "bit-identical": the quadric now accumulates over
            // seven floats instead of five, and float addition is not
            // associative. That is exactly why kVirtualMeshBuilderVersion moved.
            ctx.Attributes[i * kAttributeCount + 5] = hasLightmapUVs ? result.LightmapUVs[i].x : 0.0f;
            ctx.Attributes[i * kAttributeCount + 6] = hasLightmapUVs ? result.LightmapUVs[i].y : 0.0f;
        }

        // Canonical per-position indices: cluster connectivity and boundary locks must see
        // duplicated vertices (UV/normal seams) as the same point.
        ctx.PositionRemap.SetNum(ctx.VertexCount, EAllowShrinking::No);
        meshopt_generatePositionRemap(ctx.PositionRemap.GetData(), ctx.Positions(), ctx.VertexCount, sizeof(Vertex));

        // Positions and attributes never change during the build, so the protect mask is
        // computed once and OR-ed into the per-level locks.
        ComputeAttributeProtectBits(ctx);

        // LOD 0: split the source triangles into leaf clusters with tight bounds, error 0.
        TArray<BuildCluster> clusters = Clusterize(ctx, indices.GetData(), static_cast<sizet>(indices.Num()));
        if (clusters.IsEmpty())
        {
            OLO_CORE_WARN("VirtualMeshBuilder::Build: Clusterization produced no clusters");
            result = VirtualMesh{};
            return result;
        }
        for (BuildCluster& cluster : clusters)
        {
            cluster.LODBounds = TightLODBounds(ctx, cluster.Indices, 0.0f);
        }

        TArray<u32> pending(static_cast<sizet>(clusters.Num()));
        for (sizet i = 0; i < static_cast<sizet>(clusters.Num()); ++i)
        {
            pending[i] = static_cast<u32>(i);
        }

        TArray<u8> locks(ctx.VertexCount, 0);
        u32 depth = 0;
        std::array<u32, 4> pathCounts{}; // indexed by SimplifyPath

        // Merge + simplify until a single cluster (or nothing but stuck groups) remains.
        while (static_cast<sizet>(pending.Num()) > 1 && depth < ctx.Config.MaxLevels)
        {
            TArray<TArray<u32>> const partitions = Partition(ctx, clusters, pending);
            LockGroupBoundaries(ctx, clusters, partitions, locks);

            TArray<u32> nextPending;
            for (const TArray<u32>& members : partitions)
            {
                TArray<u32> merged;
                {
                    sizet totalIndexCount = 0;
                    for (u32 const member : members)
                    {
                        totalIndexCount += static_cast<sizet>(clusters[member].Indices.Num());
                    }
                    merged.Reserve(totalIndexCount);
                }
                for (u32 const member : members)
                {
                    merged.Append(clusters[member].Indices);
                }

                VirtualLODBounds groupBounds = MergeLODBounds(clusters, members);

                auto targetIndexCount = static_cast<sizet>(static_cast<f64>(static_cast<sizet>(merged.Num()) / 3) * ctx.Config.SimplifyRatio) * 3;

                SimplifyOutcome const outcome = Simplify(ctx, merged, locks, targetIndexCount);
                ++pathCounts[static_cast<sizet>(outcome.Path)];

                if (outcome.Path == SimplifyPath::Stuck)
                {
                    // Simplification is stuck (or annihilated the geometry, which must not
                    // leave a hole in coarse cuts) — emit as a terminal group that is never
                    // refined away (FLT_MAX error keeps it selected at any threshold).
                    //
                    // Freeze BEFORE emitting: EmitGroup consumes (clears) the members' indices,
                    // and the boundary this group shares with its still-live neighbours has to
                    // stay locked for every remaining level or the coarse cuts crack along it.
                    groupBounds.Error = std::numeric_limits<f32>::max();
                    FreezeTerminalGroupBoundary(ctx, clusters, members);
                    EmitGroup(result, ctx, clusters, members, depth, groupBounds);
                    continue;
                }

                const TArray<u32>& simplified = outcome.Indices;

                // Monotone error: parent error can never be below any child's error. This is the
                // reference's error merge at its default parameters —
                // max(previous * simplify_error_merge_previous, current) + current *
                // simplify_error_merge_additive with merge_previous = 1 and additive = 0.
                groupBounds.Error = std::max(groupBounds.Error, outcome.AbsoluteError);

                i32 const refinedGroup = EmitGroup(result, ctx, clusters, members, depth, groupBounds);

                TArray<BuildCluster> split = Clusterize(ctx, simplified.GetData(), static_cast<sizet>(simplified.Num()));
                for (BuildCluster& cluster : split)
                {
                    cluster.RefinedGroup = refinedGroup;
                    // Conservative propagation: the new cluster inherits the group bounds,
                    // so the next level's merged sphere contains this group's sphere.
                    cluster.LODBounds = groupBounds;
                    clusters.Add(std::move(cluster));
                    nextPending.Add(static_cast<u32>(clusters.Num()) - 1);
                }
            }

            pending = std::move(nextPending);
            ++depth;
        }

        if (static_cast<sizet>(pending.Num()) == 1)
        {
            // The DAG root: a single coarsest cluster, emitted as a terminal group.
            VirtualLODBounds rootBounds = clusters[pending[0]].LODBounds;
            rootBounds.Error = std::numeric_limits<f32>::max();
            EmitGroup(result, ctx, clusters, pending, depth, rootBounds);
        }
        else if (!pending.IsEmpty())
        {
            // MaxLevels safety cap hit — close the DAG with one terminal group.
            OLO_CORE_WARN("VirtualMeshBuilder::Build: Hit MaxLevels={} with {} clusters pending; emitting terminal group",
                          ctx.Config.MaxLevels, static_cast<sizet>(pending.Num()));
            VirtualLODBounds capBounds = MergeLODBounds(clusters, pending);
            capBounds.Error = std::numeric_limits<f32>::max();
            EmitGroup(result, ctx, clusters, pending, depth, capBounds);
        }

        u32 maxDepth = 0;
        for (const VirtualClusterGroup& group : result.Groups)
        {
            maxDepth = std::max(maxDepth, group.Depth);
        }
        result.LevelCount = maxDepth + 1;

        OLO_CORE_TRACE("VirtualMeshBuilder: submesh {} -> {} clusters in {} groups across {} levels from {} triangles "
                       "(simplify path: {} permissive, {} welded, {} sloppy, {} stuck)",
                       submeshIndex, static_cast<sizet>(result.Clusters.Num()), static_cast<sizet>(result.Groups.Num()), result.LevelCount,
                       result.SourceTriangleCount,
                       pathCounts[static_cast<sizet>(SimplifyPath::Permissive)],
                       pathCounts[static_cast<sizet>(SimplifyPath::Welded)],
                       pathCounts[static_cast<sizet>(SimplifyPath::Sloppy)],
                       pathCounts[static_cast<sizet>(SimplifyPath::Stuck)]);

        // A NON-TRIVIAL mesh that produced only ONE level built no LOD hierarchy at all — the
        // cut can never coarsen, so virtual geometry draws it at full source density forever,
        // in the main view and in every shadow cascade. This is total, and it used to be SILENT:
        // the DAG looked valid, and the only way it surfaced was a 173M-triangle scene running at
        // 10 fps for no visible reason (issue #651). Make it loud.
        //
        // The threshold is deliberately generous — a genuinely tiny mesh legitimately has one
        // level, so only warn once the source is big enough that a flat DAG is certainly a bug
        // (a couple of cluster's worth of triangles).
        if (result.LevelCount <= 1 && result.SourceTriangleCount > 4u * ctx.Config.MaxClusterTriangles)
        {
            OLO_CORE_WARN("VirtualMeshBuilder: submesh {} built a SINGLE-LEVEL DAG from {} triangles — the "
                          "cluster simplifier could not reduce ANY group, so this mesh has NO usable LOD and "
                          "will always render at full source density (issue #651). The usual cause is an "
                          "index-unwelded mesh (co-located vertices with distinct indices, e.g. a source with "
                          "no normals that Assimp gave flat per-face normals): meshopt classifies a position "
                          "with many attribute wedges as locked and cannot collapse it. The builder defends "
                          "against exactly this with THREE rungs — permissive simplification, a position-welded "
                          "retry, and a watertightness-guarded sloppy pass — so if you still see this warning, "
                          "all three failed and the mesh is genuinely irreducible (or the config disabled them).",
                          submeshIndex, result.SourceTriangleCount);
        }

        ComputeSkinningMetadata(result);

        ReportUvDegenerates(result, submeshIndex);

        return result;
    }

    VirtualMesh Build(const MeshSource& meshSource, const VirtualMeshBuildConfig& config)
    {
        return BuildSubmesh(meshSource, 0, config);
    }

    VirtualMeshSet BuildSet(const MeshSource& meshSource, const VirtualMeshBuildConfig& config)
    {
        OLO_PROFILE_FUNCTION();

        VirtualMeshSet set;

        // Skinned sources build (issue #1150); morph targets still do not — see
        // BuildSubmesh for why the two part company here.
        if (meshSource.HasMorphTargets())
        {
            OLO_CORE_WARN("VirtualMeshBuilder::BuildSet: Morph-target sources are not supported");
            return set;
        }

        const auto& submeshes = meshSource.GetSubmeshes();
        // No submesh records => one implicit submesh over the whole index buffer.
        u32 const submeshCount = submeshes.IsEmpty() ? 1u : static_cast<u32>(submeshes.Num());

        set.Parts.Reserve(submeshCount);
        for (u32 i = 0; i < submeshCount; ++i)
        {
            VirtualMesh dag = BuildSubmesh(meshSource, i, config);
            if (!dag.IsValid())
            {
                // A submesh the builder can't handle (degenerate, sub-triangle) is skipped
                // rather than failing the whole mesh — the other parts still render.
                continue;
            }

            VirtualMeshPart part;
            part.SubmeshIndex = i;
            part.MaterialIndex = submeshes.IsEmpty() ? 0u : submeshes[static_cast<i32>(i)].m_MaterialIndex;
            part.Dag = std::move(dag);
            set.Parts.Add(std::move(part));
        }

        if (!set.IsValid())
        {
            OLO_CORE_WARN("VirtualMeshBuilder::BuildSet: no submesh of the source produced a usable DAG");
            return set;
        }

        OLO_CORE_TRACE("VirtualMeshBuilder::BuildSet: {} parts (of {} submeshes), {} clusters, {} source triangles",
                       static_cast<sizet>(set.Parts.Num()), submeshCount, set.TotalClusters(), set.TotalSourceTriangles());
        return set;
    }
} // namespace OloEngine::VirtualMeshBuilder
