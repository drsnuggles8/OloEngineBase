#include "OloEnginePCH.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshProxy.h"

#include <algorithm>
#include <array>
#include <limits>

namespace OloEngine
{
    namespace
    {
        constexpr u32 kUnmapped = std::numeric_limits<u32>::max();
    }

    VirtualProxyMesh BuildVirtualProxyMesh(const VirtualMesh& dag)
    {
        OLO_PROFILE_FUNCTION();

        VirtualProxyMesh proxy;
        if (!dag.IsValid() || dag.Vertices.empty())
        {
            return proxy;
        }
        proxy.SourceTriangleCount = dag.SourceTriangleCount;

        const std::vector<u32> cut = dag.SelectCoarsestCut();
        if (cut.empty())
        {
            return proxy;
        }

        // Dense source-vertex -> proxy-vertex map. One entry per DAG vertex is
        // a few hundred KB at worst and turns the remap into an array read; a
        // hash map here would be the hot loop's cost for a mesh with a million
        // vertices.
        std::vector<u32> remap(dag.Vertices.size(), kUnmapped);

        // Exact reserve rather than a per-cluster upper bound: a cut of a
        // Nanite-class mesh is thousands of clusters, and cluster capacity
        // (up to 512 triangles) over-reserves by an order of magnitude
        // against the ~128 they actually carry.
        sizet cutTriangles = 0;
        sizet cutVertices = 0;
        for (const u32 clusterIndex : cut)
        {
            if (clusterIndex < dag.Clusters.size())
            {
                cutTriangles += dag.Clusters[clusterIndex].TriangleCount;
                cutVertices += dag.Clusters[clusterIndex].VertexCount;
            }
        }
        //
        // BOTH reserves are clamped to what the DAG's own arrays can actually
        // hold. The counts summed above are the clusters' unvalidated
        // TriangleCount / VertexCount — the very fields the per-cluster window
        // check below exists to distrust — so reserving from them raw lets a
        // corrupt blob throw length_error out of a function whose contract is
        // to drop bad clusters and carry on.
        proxy.Indices.reserve(std::min(cutTriangles, dag.ClusterTriangles.size() / 3u) * 3u);
        // An over-estimate — clusters share boundary vertices, so the compacted
        // count is lower — but bounded by the DAG's own vertex array.
        proxy.Vertices.reserve(std::min(cutVertices, dag.Vertices.size()));

        u32 droppedClusters = 0;
        u32 droppedTriangles = 0;
        for (const u32 clusterIndex : cut)
        {
            if (clusterIndex >= dag.Clusters.size())
            {
                ++droppedClusters;
                continue;
            }
            const VirtualCluster& cluster = dag.Clusters[clusterIndex];

            // Both windows are checked WHOLE before a single element is read,
            // so an overflowing count cannot walk off the end mid-triangle.
            // sizet arithmetic on purpose: TriangleOffset + TriangleCount * 3
            // overflows u32 for a hostile blob, and the check would then pass.
            const sizet vertexEnd = static_cast<sizet>(cluster.VertexOffset) + static_cast<sizet>(cluster.VertexCount);
            const sizet triangleEnd =
                static_cast<sizet>(cluster.TriangleOffset) + static_cast<sizet>(cluster.TriangleCount) * 3u;
            if (vertexEnd > dag.ClusterVertexRefs.size() || triangleEnd > dag.ClusterTriangles.size())
            {
                ++droppedClusters;
                continue;
            }

            for (u32 triangle = 0; triangle < cluster.TriangleCount; ++triangle)
            {
                const sizet base = static_cast<sizet>(cluster.TriangleOffset) + static_cast<sizet>(triangle) * 3u;
                std::array<u32, 3> corner{};
                bool resolved = true;
                for (u32 k = 0; k < 3u; ++k)
                {
                    const u32 localIndex = dag.ClusterTriangles[base + k];
                    if (localIndex >= cluster.VertexCount)
                    {
                        resolved = false;
                        break;
                    }
                    const u32 sourceVertex =
                        dag.ClusterVertexRefs[static_cast<sizet>(cluster.VertexOffset) + localIndex];
                    if (sourceVertex >= dag.Vertices.size())
                    {
                        resolved = false;
                        break;
                    }
                    u32& slot = remap[sourceVertex];
                    if (slot == kUnmapped)
                    {
                        slot = static_cast<u32>(proxy.Vertices.size());
                        proxy.Vertices.push_back(dag.Vertices[sourceVertex]);
                    }
                    corner[k] = slot;
                }
                if (!resolved)
                {
                    ++droppedTriangles;
                    continue;
                }
                // A triangle with two identical corners has no area and no
                // normal. It is legal BLAS input and would simply never be
                // hit, so dropping it costs nothing and saves the build.
                if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2])
                {
                    ++droppedTriangles;
                    continue;
                }
                proxy.Indices.push_back(corner[0]);
                proxy.Indices.push_back(corner[1]);
                proxy.Indices.push_back(corner[2]);
            }
        }

        // A cut that lost anything is a cut that is no longer watertight, so it
        // is REJECTED, not merely remarked on. Warning and returning the
        // survivors would upload a proxy with a hole in it and let the registry
        // classify the part as supported — and the visible symptom is a shadow
        // ray leaking through a mesh that otherwise looks plausible, which is
        // the "loud exclusion replaced by a silent partial success" this whole
        // file exists to avoid. Returning nothing makes the part countable
        // instead: it lands in ProxylessParts and reports Virtualized.
        if (droppedClusters > 0 || droppedTriangles > 0)
        {
            OLO_CORE_WARN_TAG("VirtualGeometry",
                              "ray-tracing proxy dropped {} cluster(s) and {} triangle(s) with out-of-range "
                              "references — the cooked DAG is inconsistent, so the proxy is REJECTED rather than "
                              "uploaded with a hole in it. This mesh will not be ray-traced. Delete its .omesh "
                              "cache to force a rebuild.",
                              droppedClusters, droppedTriangles);
            return VirtualProxyMesh{};
        }

        proxy.Vertices.shrink_to_fit();
        proxy.Indices.shrink_to_fit();
        if (!proxy.IsValid())
        {
            // All-or-nothing: a caller tests IsValid() once and never has to
            // wonder whether a half-built proxy is safe to upload.
            proxy = VirtualProxyMesh{};
        }
        return proxy;
    }
} // namespace OloEngine
