// OLO_TEST_LAYER: cullinglod
//
// Contract tests for the virtual-geometry RAY-TRACING PROXY (issue #1144).
//
// The bug this closes is silent by construction: virtual geometry never
// reached GPU Scene, so it never reached the TLAS, so a Nanite mesh cast no
// ray-traced shadow and appeared in no ray-traced reflection — and the symptom
// is MISSING geometry, which looks like nothing at all. The fix builds a proxy
// mesh from the DAG's coarsest cut and registers that as ordinary rigid
// geometry.
//
// Two properties carry the whole idea, and both are pinned here on the CPU so
// CI can hold them:
//
//   1. WATERTIGHT. A proxy with a hole in it is a proxy shadow rays leak
//      through, and the leak is a soft grey patch nobody would attribute to
//      the acceleration structure. Every undirected edge of the proxy must be
//      shared by exactly two triangles on a closed source mesh — the same
//      property VirtualMeshBuilderTest pins for every OTHER cut, checked here
//      on the flattened output rather than on the cluster selection, so a bug
//      in the flattening itself (a bad local->global index resolve, a dropped
//      triangle) cannot hide behind a watertight selection.
//
//   2. SMALL. The reason not to trace the full-resolution mesh is cost, so a
//      proxy that is not dramatically smaller than its source has bought
//      nothing. The DAG halves per level, so the reduction is large.
//
// Plus the things that would corrupt a BLAS rather than merely look wrong:
// every index in range, an index count that is a multiple of three, and
// vertices that are byte-copies of the DAG's rather than re-derived.
//
// NOT covered here, and it cannot be: whether the proxy actually reaches the
// TLAS on a device. That is RayTracingSceneTest (the classification and
// build policy, device-free), VirtualGeometryVisualEvidenceTest (the GPU Scene
// records, on a real GL context) and the live Vulkan editor pass.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshProxy.h"
#include "VirtualMeshFixtures.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        using PositionKey = std::array<u32, 3>;

        // Exact bits, not a tolerance: the proxy is supposed to COPY the DAG's
        // vertices, so two corners that should be the same vertex are the same
        // bytes. A tolerance here would accept a proxy that silently re-derived
        // positions and hide exactly the class of bug that opens a crack.
        PositionKey MakePositionKey(const Vertex& vertex)
        {
            return { std::bit_cast<u32>(vertex.Position.x), std::bit_cast<u32>(vertex.Position.y),
                     std::bit_cast<u32>(vertex.Position.z) };
        }

        void ExpectProxyIsWatertight(const VirtualProxyMesh& proxy, const char* label)
        {
            ASSERT_TRUE(proxy.IsValid()) << label << ": proxy is invalid";

            std::map<std::pair<PositionKey, PositionKey>, int> edgeCounts;
            for (sizet i = 0; i + 2 < proxy.Indices.size(); i += 3)
            {
                const std::array<PositionKey, 3> keys = {
                    MakePositionKey(proxy.Vertices[proxy.Indices[i]]),
                    MakePositionKey(proxy.Vertices[proxy.Indices[i + 1]]),
                    MakePositionKey(proxy.Vertices[proxy.Indices[i + 2]]),
                };
                for (u32 e = 0; e < 3; ++e)
                {
                    ++edgeCounts[std::minmax(keys[e], keys[(e + 1) % 3])];
                }
            }

            sizet badEdges = 0;
            for (const auto& [edge, count] : edgeCounts)
            {
                if (count != 2)
                {
                    ++badEdges;
                }
            }
            EXPECT_EQ(badEdges, 0u) << label << ": " << badEdges << " of " << edgeCounts.size()
                                    << " proxy edges are not shared by exactly 2 triangles — a ray-traced shadow "
                                       "would leak through this proxy";
        }

        void ExpectProxyIndicesAreInRange(const VirtualProxyMesh& proxy, const char* label)
        {
            ASSERT_EQ(proxy.Indices.size() % 3u, 0u) << label << ": index count is not a whole number of triangles";
            const auto vertexCount = static_cast<u32>(proxy.Vertices.size());
            for (const u32 index : proxy.Indices)
            {
                ASSERT_LT(index, vertexCount) << label << ": index " << index << " is out of range of "
                                              << vertexCount << " proxy vertices";
            }
        }
    } // namespace

    TEST(VirtualMeshProxy, CoarsestCutSelectsOnlyTerminalGroups)
    {
        auto mesh = VirtualMeshFixtures::MakeIcosphereMesh(4); // 5120 triangles
        VirtualMesh vm = VirtualMeshBuilder::Build(*mesh);
        ASSERT_TRUE(vm.IsValid());
        ASSERT_GT(vm.LevelCount, 1u) << "fixture did not build a multi-level DAG — the cut test would be vacuous";

        const f32 threshold = vm.CoarsestCutThreshold();
        const std::vector<u32> cut = vm.SelectCoarsestCut();
        ASSERT_FALSE(cut.empty());

        // Every selected cluster belongs to a group the threshold cannot
        // satisfy, i.e. a terminal (FLT_MAX-error) one. That IS the definition
        // of the root cut, and it is what makes the proxy view-independent.
        for (const u32 clusterIndex : cut)
        {
            const VirtualCluster& cluster = vm.Clusters[clusterIndex];
            ASSERT_GE(cluster.GroupIndex, 0);
            const f32 groupError = vm.Groups[static_cast<sizet>(cluster.GroupIndex)].LODBounds.Error;
            // Terminal is the FLT_MAX marker. Deliberately NOT !isfinite():
            // FLT_MAX is a finite float, and reading it as infinite is exactly
            // the bug that made the first version of CoarsestCutThreshold
            // return an empty cut.
            EXPECT_GE(groupError, std::numeric_limits<f32>::max())
                << "cluster " << clusterIndex << " sits in a non-terminal group (error " << groupError
                << ") yet was selected at the coarsest threshold " << threshold;
        }

        // ...and going one notch finer must select MORE, or the "coarsest"
        // claim is untested: a threshold that selects everything at every
        // value would pass the loop above vacuously.
        const std::vector<u32> finer = vm.SelectClusters(threshold * 0.5f);
        EXPECT_GT(finer.size(), cut.size()) << "a finer threshold selected no more clusters than the coarsest one";
    }

    TEST(VirtualMeshProxy, ProxyIsWatertightAndSubstantiallySmallerThanTheSource)
    {
        auto mesh = VirtualMeshFixtures::MakeIcosphereMesh(5); // 20480 triangles, closed manifold
        VirtualMesh vm = VirtualMeshBuilder::Build(*mesh);
        ASSERT_TRUE(vm.IsValid());

        const VirtualProxyMesh proxy = BuildVirtualProxyMesh(vm);
        ASSERT_TRUE(proxy.IsValid());
        ExpectProxyIndicesAreInRange(proxy, "icosphere(5)");
        ExpectProxyIsWatertight(proxy, "icosphere(5)");

        EXPECT_EQ(proxy.SourceTriangleCount, vm.SourceTriangleCount);
        // The reduction is the point. A DAG halves per level, so a 20k-triangle
        // source reduces by orders of magnitude; 10% is a deliberately loose
        // bound that still fails loudly if the coarsest cut ever degenerates
        // into "the whole mesh".
        EXPECT_LT(proxy.TriangleCount(), proxy.SourceTriangleCount / 10u)
            << "proxy is " << proxy.TriangleCount() << " of " << proxy.SourceTriangleCount
            << " source triangles — the coarse cut bought nothing";
        EXPECT_GT(proxy.TriangleCount(), 0u);
        // Compaction: the proxy must not carry the DAG's whole vertex array,
        // which holds every LOD level's vertices.
        EXPECT_LT(proxy.Vertices.size(), vm.Vertices.size());
    }

    TEST(VirtualMeshProxy, ProxyVerticesAreByteCopiesOfTheDagVertices)
    {
        auto mesh = VirtualMeshFixtures::MakeIcosphereMesh(4);
        VirtualMesh vm = VirtualMeshBuilder::Build(*mesh);
        ASSERT_TRUE(vm.IsValid());

        const VirtualProxyMesh proxy = BuildVirtualProxyMesh(vm);
        ASSERT_TRUE(proxy.IsValid());

        // A proxy vertex that is not bit-identical to a DAG vertex means the
        // flattening rewrote geometry, which would move the traced surface off
        // the rasterized one by an amount no test threshold would catch.
        std::set<PositionKey> dagPositions;
        for (const Vertex& vertex : vm.Vertices)
        {
            dagPositions.insert(MakePositionKey(vertex));
        }
        for (const Vertex& vertex : proxy.Vertices)
        {
            EXPECT_TRUE(dagPositions.contains(MakePositionKey(vertex)))
                << "proxy carries a vertex position the DAG does not have";
        }
    }

    TEST(VirtualMeshProxy, ProxySurfaceStaysInsideTheSourceBounds)
    {
        auto mesh = VirtualMeshFixtures::MakeIcosphereMesh(4);
        VirtualMesh vm = VirtualMeshBuilder::Build(*mesh);
        ASSERT_TRUE(vm.IsValid());

        const VirtualProxyMesh proxy = BuildVirtualProxyMesh(vm);
        ASSERT_TRUE(proxy.IsValid());

        // The proxy stands in for the mesh spatially, so it must occupy the
        // same place. A proxy that drifted would cast its shadow next to the
        // object rather than under it — the failure the CPU can catch that a
        // pixel comparison would blame on the light.
        glm::vec3 dagMin(std::numeric_limits<f32>::max());
        glm::vec3 dagMax(std::numeric_limits<f32>::lowest());
        for (const Vertex& vertex : vm.Vertices)
        {
            dagMin = glm::min(dagMin, vertex.Position);
            dagMax = glm::max(dagMax, vertex.Position);
        }
        for (const Vertex& vertex : proxy.Vertices)
        {
            EXPECT_TRUE(glm::all(glm::greaterThanEqual(vertex.Position, dagMin)));
            EXPECT_TRUE(glm::all(glm::lessThanEqual(vertex.Position, dagMax)));
        }
    }

    TEST(VirtualMeshProxy, AnInvalidDagProducesAnInvalidProxyRatherThanAPartialOne)
    {
        // The all-or-nothing contract: a caller tests IsValid() once. A proxy
        // with vertices and no indices would upload a live GPU allocation that
        // nothing can trace, which is the "loud exclusion replaced by a silent
        // partial success" this issue explicitly warns against.
        const VirtualProxyMesh empty = BuildVirtualProxyMesh(VirtualMesh{});
        EXPECT_FALSE(empty.IsValid());
        EXPECT_TRUE(empty.Vertices.empty());
        EXPECT_TRUE(empty.Indices.empty());
    }

    TEST(VirtualMeshProxy, EveryPartOfAMultiSubmeshSourceGetsItsOwnProxy)
    {
        // A cluster may not span a material boundary, so a multi-submesh source
        // is one DAG per submesh and therefore one proxy — and one TLAS
        // instance — per submesh. Losing a part here loses a material's worth
        // of geometry from every ray-traced effect while the raster path still
        // draws it.
        auto mesh = VirtualMeshFixtures::MakeIcosphereMesh(4);
        const auto totalIndices = static_cast<u32>(mesh->GetIndices().Num());
        const u32 halfIndices = (totalIndices / 6u) * 3u; // triangle-aligned split

        Submesh first;
        first.m_BaseIndex = 0;
        first.m_IndexCount = halfIndices;
        first.m_MaterialIndex = 0;
        mesh->AddSubmesh(first);

        Submesh second;
        second.m_BaseIndex = halfIndices;
        second.m_IndexCount = totalIndices - halfIndices;
        second.m_MaterialIndex = 1;
        mesh->AddSubmesh(second);

        const VirtualMeshSet set = VirtualMeshBuilder::BuildSet(*mesh);
        ASSERT_TRUE(set.IsValid());
        ASSERT_GE(set.Parts.size(), 2u) << "fixture did not produce a multi-part DAG";

        for (const VirtualMeshPart& part : set.Parts)
        {
            const VirtualProxyMesh proxy = BuildVirtualProxyMesh(part.Dag);
            const std::string label = "submesh " + std::to_string(part.SubmeshIndex);
            ASSERT_TRUE(proxy.IsValid()) << label << ": no proxy";
            ExpectProxyIndicesAreInRange(proxy, label.c_str());
            EXPECT_LE(proxy.TriangleCount(), proxy.SourceTriangleCount);
        }
    }
} // namespace OloEngine::Tests
