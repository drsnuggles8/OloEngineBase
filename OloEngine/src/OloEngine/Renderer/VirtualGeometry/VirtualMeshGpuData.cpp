#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"

#include <algorithm>
#include <limits>

namespace OloEngine
{
    VirtualMeshGpuData PackVirtualMeshForGpu(const VirtualMesh& mesh)
    {
        OLO_PROFILE_FUNCTION();

        VirtualMeshGpuData data;
        if (!mesh.IsValid())
        {
            return data;
        }

        data.Clusters.Reserve(static_cast<sizet>(mesh.Clusters.Num()));
        data.Groups.Reserve(static_cast<sizet>(mesh.Groups.Num()));
        {
            sizet totalVertices = 0;
            sizet totalIndices = 0;
            for (const VirtualCluster& cluster : mesh.Clusters)
            {
                totalVertices += cluster.VertexCount;
                totalIndices += static_cast<sizet>(cluster.TriangleCount) * 3;
            }
            data.Vertices.Reserve(totalVertices);
            if (!mesh.LightmapUVs.IsEmpty())
            {
                data.LightmapUVs.Reserve(totalVertices);
            }
            if (mesh.IsSkinned())
            {
                data.Skinning.Reserve(totalVertices);
            }
            data.Indices.Reserve(totalIndices);
        }

        for (const VirtualCluster& cluster : mesh.Clusters)
        {
            VirtualClusterGpuRecord record;
            record.CullSphere = { cluster.BoundsCenter, cluster.BoundsRadius };
            record.Cone = { cluster.ConeAxis, cluster.ConeCutoff };
            record.VertexBase = static_cast<u32>(data.Vertices.Num());
            record.IndexBase = static_cast<u32>(data.Indices.Num());
            record.IndexCount = cluster.TriangleCount * 3;
            record.VertexCount = cluster.VertexCount;
            record.GroupIndex = static_cast<u32>(cluster.GroupIndex);
            record.RefinedGroup = cluster.RefinedGroup >= 0 ? static_cast<u32>(cluster.RefinedGroup)
                                                            : VirtualClusterGpuRecord::kNoRefinedGroup;
            // DAG level of the member group, for the debug LOD visualization.
            record.Lod = mesh.Groups[static_cast<sizet>(cluster.GroupIndex)].Depth;
            data.Clusters.Add(record);

            // Expand the cluster's vertex window into cluster-owned packed vertices
            const bool hasLightmapUVs = mesh.LightmapUVs.Num() == mesh.Vertices.Num();
            const bool hasSkinning = mesh.Skinning.Num() == mesh.Vertices.Num();
            for (u32 v = 0; v < cluster.VertexCount; ++v)
            {
                const u32 sourceVertex = mesh.ClusterVertexRefs[cluster.VertexOffset + v];
                const Vertex& vertex = mesh.Vertices[sourceVertex];
                VirtualGpuVertex packed;
                packed.PositionU = { vertex.Position, vertex.TexCoord.x };
                packed.NormalV = { vertex.Normal, vertex.TexCoord.y };
                data.Vertices.Add(packed);
                // Same cluster-local slot, same expansion order (issue #867) —
                // the parallel stream is only addressable because the two are
                // pushed in lockstep here.
                if (hasLightmapUVs)
                {
                    data.LightmapUVs.Add(mesh.LightmapUVs[sourceVertex]);
                }
                // Ditto for the skin binding (issue #1150): cluster-owned, so a
                // vertex duplicated into two clusters carries its binding into
                // both and a page can be streamed without a shared indirection.
                if (hasSkinning)
                {
                    data.Skinning.Add(mesh.Skinning[sourceVertex]);
                }
            }

            // Local u8 triangle indices widen to u32; BaseVertex carries VertexBase
            u32 const indexCount = cluster.TriangleCount * 3;
            for (u32 i = 0; i < indexCount; ++i)
            {
                data.Indices.Add(mesh.ClusterTriangles[cluster.TriangleOffset + i]);
            }
        }

        // Skinning metadata passes through UNEXPANDED: both arrays are already
        // keyed the way the GPU addresses them — one entry per bone slot, and
        // kMaxClusterBones entries per cluster in the SAME cluster order this
        // loop emitted. Copying rather than rebuilding is what keeps the cook
        // the single owner of the bone sets.
        data.BoneBounds = mesh.BoneBounds;
        data.ClusterBoneRefs = mesh.ClusterBoneRefs;

        for (const VirtualClusterGroup& group : mesh.Groups)
        {
            VirtualGroupGpuRecord record;
            record.LODSphere = { group.LODBounds.Center, group.LODBounds.Radius };
            record.Error = group.LODBounds.Error;
            data.Groups.Add(record);
        }

        // Streamable pages: one per group, spanning its member clusters'
        // contiguous geometry ranges. Terminal groups (never refined away) are
        // pinned so a drawable fallback always exists under any budget.
        data.Pages.Reserve(static_cast<sizet>(mesh.Groups.Num()));
        for (sizet g = 0; g < static_cast<sizet>(mesh.Groups.Num()); ++g)
        {
            const VirtualClusterGroup& group = mesh.Groups[g];
            u32 const firstCluster = group.FirstCluster;
            u32 const lastCluster = group.FirstCluster + group.ClusterCount - 1;
            const VirtualClusterGpuRecord& firstPacked = data.Clusters[firstCluster];
            const VirtualClusterGpuRecord& lastPacked = data.Clusters[lastCluster];

            VirtualPageInfo page;
            page.GroupIndex = static_cast<u32>(g);
            page.FirstCluster = firstCluster;
            page.ClusterCount = group.ClusterCount;
            page.VertexOffset = firstPacked.VertexBase;
            page.VertexCount = (lastPacked.VertexBase - firstPacked.VertexBase) + mesh.Clusters[lastCluster].VertexCount;
            page.IndexOffset = firstPacked.IndexBase;
            page.IndexCount = (lastPacked.IndexBase - firstPacked.IndexBase) + lastPacked.IndexCount;
            page.Pinned = group.LODBounds.Error >= std::numeric_limits<f32>::max();
            data.Pages.Add(page);
        }

        return data;
    }

    bool IsMeshletCompatible(const VirtualMeshGpuData& data)
    {
        return data.IsValid() &&
               std::ranges::all_of(data.Clusters, [](const VirtualClusterGpuRecord& cluster)
                                   { return cluster.VertexCount <= kMeshletMaxVertices &&
                                            cluster.IndexCount <= kMeshletMaxTriangles * 3u; });
    }
} // namespace OloEngine
