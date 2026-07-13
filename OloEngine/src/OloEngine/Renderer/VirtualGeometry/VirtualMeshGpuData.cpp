#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"

#include <limits>
#include <vector>

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

        data.Clusters.reserve(mesh.Clusters.size());
        data.Groups.reserve(mesh.Groups.size());
        {
            sizet totalVertices = 0;
            sizet totalIndices = 0;
            for (const VirtualCluster& cluster : mesh.Clusters)
            {
                totalVertices += cluster.VertexCount;
                totalIndices += static_cast<sizet>(cluster.TriangleCount) * 3;
            }
            data.Vertices.reserve(totalVertices);
            data.Indices.reserve(totalIndices);
        }

        for (const VirtualCluster& cluster : mesh.Clusters)
        {
            VirtualClusterGpuRecord record;
            record.CullSphere = { cluster.BoundsCenter, cluster.BoundsRadius };
            record.Cone = { cluster.ConeAxis, cluster.ConeCutoff };
            record.VertexBase = static_cast<u32>(data.Vertices.size());
            record.IndexBase = static_cast<u32>(data.Indices.size());
            record.IndexCount = cluster.TriangleCount * 3;
            record.GroupIndex = static_cast<u32>(cluster.GroupIndex);
            record.RefinedGroup = cluster.RefinedGroup >= 0 ? static_cast<u32>(cluster.RefinedGroup)
                                                            : VirtualClusterGpuRecord::kNoRefinedGroup;
            // DAG level of the member group, for the debug LOD visualization.
            record.Lod = mesh.Groups[static_cast<sizet>(cluster.GroupIndex)].Depth;
            data.Clusters.push_back(record);

            // Expand the cluster's vertex window into cluster-owned packed vertices
            for (u32 v = 0; v < cluster.VertexCount; ++v)
            {
                const Vertex& vertex = mesh.Vertices[mesh.ClusterVertexRefs[cluster.VertexOffset + v]];
                VirtualGpuVertex packed;
                packed.PositionU = { vertex.Position, vertex.TexCoord.x };
                packed.NormalV = { vertex.Normal, vertex.TexCoord.y };
                data.Vertices.push_back(packed);
            }

            // Local u8 triangle indices widen to u32; BaseVertex carries VertexBase
            u32 const indexCount = cluster.TriangleCount * 3;
            for (u32 i = 0; i < indexCount; ++i)
            {
                data.Indices.push_back(mesh.ClusterTriangles[cluster.TriangleOffset + i]);
            }
        }

        for (const VirtualClusterGroup& group : mesh.Groups)
        {
            VirtualGroupGpuRecord record;
            record.LODSphere = { group.LODBounds.Center, group.LODBounds.Radius };
            record.Error = group.LODBounds.Error;
            data.Groups.push_back(record);
        }

        // Streamable pages: one per group, spanning its member clusters'
        // contiguous geometry ranges. Terminal groups (never refined away) are
        // pinned so a drawable fallback always exists under any budget.
        data.Pages.reserve(mesh.Groups.size());
        for (sizet g = 0; g < mesh.Groups.size(); ++g)
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
            data.Pages.push_back(page);
        }

        return data;
    }
} // namespace OloEngine
