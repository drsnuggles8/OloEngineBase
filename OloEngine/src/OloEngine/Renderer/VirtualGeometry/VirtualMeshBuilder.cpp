#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"

#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace OloEngine::VirtualMeshBuilder
{
    namespace
    {
        // meshopt_computeSphereBounds / the positions pointer below rely on this exact layout.
        static_assert(offsetof(VirtualLODBounds, Center) == 0, "sphere center must be the first 12 bytes");
        static_assert(offsetof(VirtualLODBounds, Radius) == 12, "radius must directly follow the center");
        static_assert(offsetof(Vertex, Position) == 0, "positions must be the first 12 bytes of Vertex");

        // Attribute layout for attribute-aware simplification: nx ny nz u v.
        // Weights match MeshOptimization::GenerateLODMeshWithAttributes (normals matter
        // less than UVs for visual quality).
        constexpr sizet kAttributeCount = 5;
        constexpr std::array<f32, kAttributeCount> kAttributeWeights = { 0.5f, 0.5f, 0.5f, 1.0f, 1.0f };

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
        struct BuildCluster
        {
            std::vector<u32> Indices;
            u32 UniqueVertexCount = 0;
            i32 RefinedGroup = -1;
            VirtualLODBounds LODBounds;
        };

        struct BuildContext
        {
            VirtualMeshBuildConfig Config;
            const Vertex* Vertices = nullptr;
            sizet VertexCount = 0;
            std::vector<f32> Attributes;    // kAttributeCount floats per vertex
            std::vector<u32> PositionRemap; // canonical index per vertex (same position => same index)

            [[nodiscard]] const f32* Positions() const
            {
                return &Vertices[0].Position.x;
            }
        };

        [[nodiscard]] std::vector<BuildCluster> Clusterize(const BuildContext& ctx, const u32* indices, sizet indexCount)
        {
            sizet const maxMeshlets = meshopt_buildMeshletsBound(indexCount, ctx.Config.MaxClusterVertices,
                                                                 ctx.Config.MinClusterTriangles);
            std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
            std::vector<u32> meshletVertices(indexCount);
            std::vector<u8> meshletTriangles(indexCount);

            sizet const meshletCount = meshopt_buildMeshletsFlex(
                meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
                indices, indexCount,
                ctx.Positions(), ctx.VertexCount, sizeof(Vertex),
                ctx.Config.MaxClusterVertices, ctx.Config.MinClusterTriangles, ctx.Config.MaxClusterTriangles,
                0.0f, ctx.Config.ClusterSplitFactor);

            std::vector<BuildCluster> clusters(meshletCount);
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
                cluster.Indices.resize(clusterIndexCount);
                for (sizet j = 0; j < clusterIndexCount; ++j)
                {
                    cluster.Indices[j] = meshletVertices[meshlet.vertex_offset + meshletTriangles[meshlet.triangle_offset + j]];
                }
            }

            return clusters;
        }

        [[nodiscard]] VirtualLODBounds TightLODBounds(const BuildContext& ctx, const std::vector<u32>& indices, f32 error)
        {
            meshopt_Bounds const bounds = meshopt_computeClusterBounds(
                indices.data(), indices.size(), ctx.Positions(), ctx.VertexCount, sizeof(Vertex));

            VirtualLODBounds result;
            result.Center = { bounds.center[0], bounds.center[1], bounds.center[2] };
            result.Radius = bounds.radius;
            result.Error = error;
            return result;
        }

        // Conservative group bounds: a sphere-of-spheres over the member clusters' LOD
        // bounds plus the max member error. Deliberately NOT the precise bounds of the
        // merged/simplified geometry — that would break projected-error monotonicity.
        [[nodiscard]] VirtualLODBounds MergeLODBounds(const std::vector<BuildCluster>& clusters, const std::vector<u32>& members)
        {
            std::vector<VirtualLODBounds> memberBounds;
            memberBounds.reserve(members.size());
            for (u32 const member : members)
            {
                memberBounds.push_back(clusters[member].LODBounds);
            }

            meshopt_Bounds const merged = meshopt_computeSphereBounds(
                &memberBounds[0].Center.x, memberBounds.size(), sizeof(VirtualLODBounds),
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
        [[nodiscard]] std::vector<std::vector<u32>> Partition(const BuildContext& ctx,
                                                              const std::vector<BuildCluster>& clusters,
                                                              const std::vector<u32>& pending)
        {
            if (pending.size() <= ctx.Config.TargetGroupSize)
            {
                return { pending };
            }

            sizet const pendingCount = pending.size();
            std::vector<u32> flatIndices;
            std::vector<u32> flatCounts(pendingCount);
            {
                sizet totalIndexCount = 0;
                for (u32 const clusterIndex : pending)
                {
                    totalIndexCount += clusters[clusterIndex].Indices.size();
                }
                flatIndices.reserve(totalIndexCount);
            }
            for (sizet i = 0; i < pendingCount; ++i)
            {
                const BuildCluster& cluster = clusters[pending[i]];
                flatCounts[i] = static_cast<u32>(cluster.Indices.size());
                for (u32 const vertexIndex : cluster.Indices)
                {
                    flatIndices.push_back(ctx.PositionRemap[vertexIndex]);
                }
            }

            std::vector<u32> partitionIds(pendingCount);
            sizet const partitionCount = meshopt_partitionClusters(
                partitionIds.data(), flatIndices.data(), flatIndices.size(), flatCounts.data(), flatCounts.size(),
                ctx.Positions(), ctx.VertexCount, sizeof(Vertex), ctx.Config.TargetGroupSize);

            std::vector<std::vector<u32>> partitions(partitionCount);
            for (sizet i = 0; i < pendingCount; ++i)
            {
                partitions[partitionIds[i]].push_back(pending[i]);
            }

            std::erase_if(partitions, [](const std::vector<u32>& partition)
                          { return partition.empty(); });
            return partitions;
        }

        // Locks every vertex whose canonical position is used by more than one partition,
        // so group simplification cannot move the shared boundary — the crack-free
        // guarantee between neighbouring groups (and, transitively, across LOD levels).
        void LockGroupBoundaries(const BuildContext& ctx, const std::vector<BuildCluster>& clusters,
                                 const std::vector<std::vector<u32>>& partitions, std::vector<u8>& locks)
        {
            std::vector<i32> owner(ctx.VertexCount, kOwnerUnset);
            sizet const partitionCount = partitions.size();
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

            for (sizet v = 0; v < ctx.VertexCount; ++v)
            {
                locks[v] = (owner[ctx.PositionRemap[v]] == kOwnerShared) ? meshopt_SimplifyVertex_Lock : 0;
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

        [[nodiscard]] std::vector<u32> Simplify(const BuildContext& ctx, const std::vector<u32>& merged,
                                                const std::vector<u8>& locks, sizet targetIndexCount, f32& outAbsoluteError)
        {
            std::vector<u32> simplified(merged.size());
            f32 error = 0.0f;

            // Sparse: the group is a small subset of the full mesh. ErrorAbsolute: errors
            // must be comparable across groups and levels for the monotone DAG invariant.
            constexpr u32 kOptions = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;

            sizet const resultCount = meshopt_simplifyWithAttributes(
                simplified.data(), merged.data(), merged.size(),
                ctx.Positions(), ctx.VertexCount, sizeof(Vertex),
                ctx.Attributes.data(), kAttributeCount * sizeof(f32), kAttributeWeights.data(), kAttributeCount,
                locks.data(), targetIndexCount, std::numeric_limits<f32>::max(), kOptions, &error);

            simplified.resize(resultCount);
            outAbsoluteError = error;
            return simplified;
        }

        // Appends a group and its member clusters to the output mesh. Consumes (frees) the
        // members' geometry — every cluster is emitted exactly once.
        i32 EmitGroup(VirtualMesh& out, const BuildContext& ctx, std::vector<BuildCluster>& clusters,
                      const std::vector<u32>& members, u32 depth, const VirtualLODBounds& lodBounds)
        {
            auto groupIndex = static_cast<i32>(out.Groups.size());

            VirtualClusterGroup group;
            group.Depth = depth;
            group.FirstCluster = static_cast<u32>(out.Clusters.size());
            group.ClusterCount = static_cast<u32>(members.size());
            group.LODBounds = lodBounds;
            out.Groups.push_back(group);

            std::array<u32, 256> vertexRefs{};
            std::vector<u8> localTriangles;
            for (u32 const member : members)
            {
                BuildCluster& cluster = clusters[member];

                // Tight culling bounds (sphere + normal cone) for the emitted cluster.
                meshopt_Bounds const cullBounds = meshopt_computeClusterBounds(
                    cluster.Indices.data(), cluster.Indices.size(), ctx.Positions(), ctx.VertexCount, sizeof(Vertex));

                localTriangles.resize(cluster.Indices.size());
                sizet const uniqueVertexCount = meshopt_extractMeshletIndices(
                    vertexRefs.data(), localTriangles.data(), cluster.Indices.data(), cluster.Indices.size());

                VirtualCluster emitted;
                emitted.VertexOffset = static_cast<u32>(out.ClusterVertexRefs.size());
                emitted.TriangleOffset = static_cast<u32>(out.ClusterTriangles.size());
                emitted.VertexCount = static_cast<u32>(uniqueVertexCount);
                emitted.TriangleCount = static_cast<u32>(cluster.Indices.size() / 3);
                emitted.GroupIndex = groupIndex;
                emitted.RefinedGroup = cluster.RefinedGroup;
                emitted.BoundsCenter = { cullBounds.center[0], cullBounds.center[1], cullBounds.center[2] };
                emitted.BoundsRadius = cullBounds.radius;
                emitted.ConeApex = { cullBounds.cone_apex[0], cullBounds.cone_apex[1], cullBounds.cone_apex[2] };
                emitted.ConeAxis = { cullBounds.cone_axis[0], cullBounds.cone_axis[1], cullBounds.cone_axis[2] };
                emitted.ConeCutoff = cullBounds.cone_cutoff;
                out.Clusters.push_back(emitted);

                out.ClusterVertexRefs.insert(out.ClusterVertexRefs.end(), vertexRefs.begin(),
                                             vertexRefs.begin() + static_cast<std::ptrdiff_t>(uniqueVertexCount));
                out.ClusterTriangles.insert(out.ClusterTriangles.end(), localTriangles.begin(), localTriangles.end());

                cluster.Indices = std::vector<u32>(); // consumed — release the geometry
            }

            return groupIndex;
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
        if (meshSource.HasSkeleton() || meshSource.HasMorphTargets() || !meshSource.GetBoneInfo().IsEmpty())
        {
            OLO_CORE_WARN("VirtualMeshBuilder: Skinned / morph-target sources are not supported");
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
        std::vector<u32> indices(rangeIndexCount);
        {
            std::vector<u32> vertexRemap(static_cast<sizet>(srcVertices.Num()), UINT32_MAX);
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
                    vertexRemap[v] = static_cast<u32>(result.Vertices.size());
                    result.Vertices.push_back(srcVertices[static_cast<i32>(v)]);
                }
                indices[i] = vertexRemap[v];
            }
        }

        ctx.Vertices = result.Vertices.data();
        ctx.VertexCount = result.Vertices.size();
        result.SourceTriangleCount = static_cast<u32>(indices.size() / 3);

        ctx.Attributes.resize(ctx.VertexCount * kAttributeCount);
        for (sizet i = 0; i < ctx.VertexCount; ++i)
        {
            const Vertex& vertex = result.Vertices[i];
            ctx.Attributes[i * kAttributeCount + 0] = vertex.Normal.x;
            ctx.Attributes[i * kAttributeCount + 1] = vertex.Normal.y;
            ctx.Attributes[i * kAttributeCount + 2] = vertex.Normal.z;
            ctx.Attributes[i * kAttributeCount + 3] = vertex.TexCoord.x;
            ctx.Attributes[i * kAttributeCount + 4] = vertex.TexCoord.y;
        }

        // Canonical per-position indices: cluster connectivity and boundary locks must see
        // duplicated vertices (UV/normal seams) as the same point.
        ctx.PositionRemap.resize(ctx.VertexCount);
        meshopt_generatePositionRemap(ctx.PositionRemap.data(), ctx.Positions(), ctx.VertexCount, sizeof(Vertex));

        // LOD 0: split the source triangles into leaf clusters with tight bounds, error 0.
        std::vector<BuildCluster> clusters = Clusterize(ctx, indices.data(), indices.size());
        if (clusters.empty())
        {
            OLO_CORE_WARN("VirtualMeshBuilder::Build: Clusterization produced no clusters");
            result = VirtualMesh{};
            return result;
        }
        for (BuildCluster& cluster : clusters)
        {
            cluster.LODBounds = TightLODBounds(ctx, cluster.Indices, 0.0f);
        }

        std::vector<u32> pending(clusters.size());
        for (sizet i = 0; i < clusters.size(); ++i)
        {
            pending[i] = static_cast<u32>(i);
        }

        std::vector<u8> locks(ctx.VertexCount, 0);
        u32 depth = 0;

        // Merge + simplify until a single cluster (or nothing but stuck groups) remains.
        while (pending.size() > 1 && depth < ctx.Config.MaxLevels)
        {
            std::vector<std::vector<u32>> const partitions = Partition(ctx, clusters, pending);
            LockGroupBoundaries(ctx, clusters, partitions, locks);

            std::vector<u32> nextPending;
            for (const std::vector<u32>& members : partitions)
            {
                std::vector<u32> merged;
                {
                    sizet totalIndexCount = 0;
                    for (u32 const member : members)
                    {
                        totalIndexCount += clusters[member].Indices.size();
                    }
                    merged.reserve(totalIndexCount);
                }
                for (u32 const member : members)
                {
                    merged.insert(merged.end(), clusters[member].Indices.begin(), clusters[member].Indices.end());
                }

                VirtualLODBounds groupBounds = MergeLODBounds(clusters, members);

                auto targetIndexCount = static_cast<sizet>(static_cast<f64>(merged.size() / 3) * ctx.Config.SimplifyRatio) * 3;

                f32 absoluteError = 0.0f;
                std::vector<u32> const simplified = Simplify(ctx, merged, locks, targetIndexCount, absoluteError);

                if (auto stuckLimit = static_cast<sizet>(static_cast<f64>(merged.size()) * ctx.Config.StuckThreshold);
                    simplified.empty() || simplified.size() > stuckLimit)
                {
                    // Simplification is stuck (or annihilated the geometry, which must not
                    // leave a hole in coarse cuts) — emit as a terminal group that is never
                    // refined away (FLT_MAX error keeps it selected at any threshold).
                    groupBounds.Error = std::numeric_limits<f32>::max();
                    EmitGroup(result, ctx, clusters, members, depth, groupBounds);
                    continue;
                }

                // Monotone error: parent error can never be below any child's error.
                groupBounds.Error = std::max(groupBounds.Error, absoluteError);

                i32 const refinedGroup = EmitGroup(result, ctx, clusters, members, depth, groupBounds);

                std::vector<BuildCluster> split = Clusterize(ctx, simplified.data(), simplified.size());
                for (BuildCluster& cluster : split)
                {
                    cluster.RefinedGroup = refinedGroup;
                    // Conservative propagation: the new cluster inherits the group bounds,
                    // so the next level's merged sphere contains this group's sphere.
                    cluster.LODBounds = groupBounds;
                    clusters.push_back(std::move(cluster));
                    nextPending.push_back(static_cast<u32>(clusters.size()) - 1);
                }
            }

            pending = std::move(nextPending);
            ++depth;
        }

        if (pending.size() == 1)
        {
            // The DAG root: a single coarsest cluster, emitted as a terminal group.
            VirtualLODBounds rootBounds = clusters[pending[0]].LODBounds;
            rootBounds.Error = std::numeric_limits<f32>::max();
            EmitGroup(result, ctx, clusters, pending, depth, rootBounds);
        }
        else if (!pending.empty())
        {
            // MaxLevels safety cap hit — close the DAG with one terminal group.
            OLO_CORE_WARN("VirtualMeshBuilder::Build: Hit MaxLevels={} with {} clusters pending; emitting terminal group",
                          ctx.Config.MaxLevels, pending.size());
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

        OLO_CORE_TRACE("VirtualMeshBuilder: submesh {} -> {} clusters in {} groups across {} levels from {} triangles",
                       submeshIndex, result.Clusters.size(), result.Groups.size(), result.LevelCount,
                       result.SourceTriangleCount);

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

        if (meshSource.HasSkeleton() || meshSource.HasMorphTargets() || !meshSource.GetBoneInfo().IsEmpty())
        {
            OLO_CORE_WARN("VirtualMeshBuilder::BuildSet: Skinned / morph-target sources are not supported");
            return set;
        }

        const auto& submeshes = meshSource.GetSubmeshes();
        // No submesh records => one implicit submesh over the whole index buffer.
        u32 const submeshCount = submeshes.IsEmpty() ? 1u : static_cast<u32>(submeshes.Num());

        set.Parts.reserve(submeshCount);
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
            set.Parts.push_back(std::move(part));
        }

        if (!set.IsValid())
        {
            OLO_CORE_WARN("VirtualMeshBuilder::BuildSet: no submesh of the source produced a usable DAG");
            return set;
        }

        OLO_CORE_TRACE("VirtualMeshBuilder::BuildSet: {} parts (of {} submeshes), {} clusters, {} source triangles",
                       set.Parts.size(), submeshCount, set.TotalClusters(), set.TotalSourceTriangles());
        return set;
    }
} // namespace OloEngine::VirtualMeshBuilder
