// OLO_TEST_LAYER: cullinglod
//
// Contract tests for the Nanite-style cluster LOD DAG builder (issue #629, step 1).
// Everything here is pure CPU: procedural meshes in, DAG invariants out. The two
// load-bearing contracts from the issue's acceptance criteria:
//   - the DAG error is ALWAYS monotone along every refinement edge (and the LOD spheres
//     are nested, so projected screen-space error is monotone from any viewpoint), and
//   - the LOD cut selected for ANY error threshold is watertight on a closed mesh —
//     every edge of the selected triangle set is shared by exactly two triangles, i.e.
//     LOD transitions never open cracks.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Procedural mesh helpers
// =============================================================================

static Ref<MeshSource> MakeQuadMesh()
{
    TArray<Vertex> vertices;
    vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }));
    vertices.Add(Vertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }));

    TArray<u32> indices;
    for (u32 i : { 0u, 1u, 2u, 0u, 2u, 3u })
    {
        indices.Add(i);
    }

    return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
}

static Ref<MeshSource> MakeGridMesh(u32 gridSize)
{
    auto verticesPerSide = gridSize + 1;
    TArray<Vertex> vertices;
    vertices.Reserve(static_cast<i32>(verticesPerSide * verticesPerSide));

    for (u32 z = 0; z < verticesPerSide; ++z)
    {
        for (u32 x = 0; x < verticesPerSide; ++x)
        {
            auto fx = static_cast<f32>(x) / static_cast<f32>(gridSize);
            auto fz = static_cast<f32>(z) / static_cast<f32>(gridSize);
            // Gentle height variation so simplification produces non-zero error
            auto fy = 0.05f * std::sin(fx * 9.0f) * std::cos(fz * 7.0f);
            vertices.Add(Vertex({ fx, fy, fz }, { 0.0f, 1.0f, 0.0f }, { fx, fz }));
        }
    }

    TArray<u32> indices;
    indices.Reserve(static_cast<i32>(gridSize * gridSize * 6));
    for (u32 z = 0; z < gridSize; ++z)
    {
        for (u32 x = 0; x < gridSize; ++x)
        {
            auto i0 = z * verticesPerSide + x;
            auto i1 = i0 + 1;
            auto i2 = i0 + verticesPerSide;
            auto i3 = i2 + 1;
            for (u32 i : { i0, i1, i2, i1, i3, i2 })
            {
                indices.Add(i);
            }
        }
    }

    return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
}

// Subdivided icosahedron on the unit sphere: closed 2-manifold. 20 * 4^subdivisions triangles.
static void BuildIcosphereData(u32 subdivisions, std::vector<glm::vec3>& positions, std::vector<u32>& indices)
{
    const f32 t = (1.0f + std::sqrt(5.0f)) / 2.0f;

    positions = {
        { -1.0f, t, 0.0f },
        { 1.0f, t, 0.0f },
        { -1.0f, -t, 0.0f },
        { 1.0f, -t, 0.0f },
        { 0.0f, -1.0f, t },
        { 0.0f, 1.0f, t },
        { 0.0f, -1.0f, -t },
        { 0.0f, 1.0f, -t },
        { t, 0.0f, -1.0f },
        { t, 0.0f, 1.0f },
        { -t, 0.0f, -1.0f },
        { -t, 0.0f, 1.0f },
    };
    for (auto& p : positions)
    {
        p = glm::normalize(p);
    }

    indices = {
        0,
        11,
        5,
        0,
        5,
        1,
        0,
        1,
        7,
        0,
        7,
        10,
        0,
        10,
        11,
        1,
        5,
        9,
        5,
        11,
        4,
        11,
        10,
        2,
        10,
        7,
        6,
        7,
        1,
        8,
        3,
        9,
        4,
        3,
        4,
        2,
        3,
        2,
        6,
        3,
        6,
        8,
        3,
        8,
        9,
        4,
        9,
        5,
        2,
        4,
        11,
        6,
        2,
        10,
        8,
        6,
        7,
        9,
        8,
        1,
    };

    for (u32 s = 0; s < subdivisions; ++s)
    {
        std::map<std::pair<u32, u32>, u32> midpointCache;
        auto midpoint = [&](u32 a, u32 b) -> u32
        {
            std::pair<u32, u32> const key = std::minmax(a, b);
            if (auto it = midpointCache.find(key); it != midpointCache.end())
            {
                return it->second;
            }
            auto index = static_cast<u32>(positions.size());
            positions.push_back(glm::normalize((positions[a] + positions[b]) * 0.5f));
            midpointCache.emplace(key, index);
            return index;
        };

        std::vector<u32> next;
        next.reserve(indices.size() * 4);
        for (sizet i = 0; i + 2 < indices.size(); i += 3)
        {
            u32 const a = indices[i];
            u32 const b = indices[i + 1];
            u32 const c = indices[i + 2];
            u32 const ab = midpoint(a, b);
            u32 const bc = midpoint(b, c);
            u32 const ca = midpoint(c, a);
            for (u32 idx : { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca })
            {
                next.push_back(idx);
            }
        }
        indices = std::move(next);
    }
}

static Vertex IcosphereVertex(const glm::vec3& p, f32 uOffset = 0.0f)
{
    constexpr f32 kPi = 3.14159265358979323846f;
    glm::vec2 const uv{ std::atan2(p.z, p.x) / (2.0f * kPi) + 0.5f + uOffset,
                        std::asin(std::clamp(p.y, -1.0f, 1.0f)) / kPi + 0.5f };
    return Vertex(p, p, uv); // unit sphere: normal == position
}

static Ref<MeshSource> MakeIcosphereMesh(u32 subdivisions)
{
    std::vector<glm::vec3> positions;
    std::vector<u32> indices;
    BuildIcosphereData(subdivisions, positions, indices);

    TArray<Vertex> vertices;
    vertices.Reserve(static_cast<i32>(positions.size()));
    for (const glm::vec3& p : positions)
    {
        vertices.Add(IcosphereVertex(p));
    }

    TArray<u32> meshIndices;
    meshIndices.Reserve(static_cast<i32>(indices.size()));
    for (u32 const index : indices)
    {
        meshIndices.Add(index);
    }

    return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(meshIndices));
}

// Icosphere with a genuine attribute seam: every vertex on the boundary between the
// z >= 0 and z < 0 triangle regions is duplicated with a shifted UV, and the z >= 0
// region is rewritten to use the duplicates. Geometrically closed (positions match
// across the seam) but open in index space — exactly the shape a UV-seamed import has.
// This exercises the builder's position-remap path: without canonical position
// handling, the seam copies would simplify independently and crack.
static Ref<MeshSource> MakeSeamedIcosphereMesh(u32 subdivisions)
{
    std::vector<glm::vec3> positions;
    std::vector<u32> indices;
    BuildIcosphereData(subdivisions, positions, indices);

    sizet const triangleCount = indices.size() / 3;
    std::vector<bool> inRegion(triangleCount, false);
    std::vector<bool> usedInside(positions.size(), false);
    std::vector<bool> usedOutside(positions.size(), false);
    for (sizet tri = 0; tri < triangleCount; ++tri)
    {
        glm::vec3 const centroid = (positions[indices[tri * 3]] + positions[indices[tri * 3 + 1]] + positions[indices[tri * 3 + 2]]) / 3.0f;
        inRegion[tri] = centroid.z >= 0.0f;
        for (sizet corner = 0; corner < 3; ++corner)
        {
            (inRegion[tri] ? usedInside : usedOutside)[indices[tri * 3 + corner]] = true;
        }
    }

    TArray<Vertex> vertices;
    vertices.Reserve(static_cast<i32>(positions.size() + 64));
    for (const glm::vec3& p : positions)
    {
        vertices.Add(IcosphereVertex(p));
    }

    std::map<u32, u32> seamDuplicate;
    sizet const positionCount = positions.size();
    for (sizet v = 0; v < positionCount; ++v)
    {
        if (usedInside[v] && usedOutside[v])
        {
            seamDuplicate.emplace(static_cast<u32>(v), static_cast<u32>(vertices.Num()));
            vertices.Add(IcosphereVertex(positions[v], 1.0f)); // same position, shifted UV
        }
    }

    TArray<u32> meshIndices;
    meshIndices.Reserve(static_cast<i32>(indices.size()));
    for (sizet tri = 0; tri < triangleCount; ++tri)
    {
        for (sizet corner = 0; corner < 3; ++corner)
        {
            u32 index = indices[tri * 3 + corner];
            if (inRegion[tri])
            {
                if (auto it = seamDuplicate.find(index); it != seamDuplicate.end())
                {
                    index = it->second;
                }
            }
            meshIndices.Add(index);
        }
    }

    return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(meshIndices));
}

// =============================================================================
// DAG inspection helpers
// =============================================================================

// Bit-exact position key: the builder never invents new vertex positions (simplification
// only drops vertices), so triangles across all LOD levels reference bitwise-identical
// positions and exact keys are the correct comparison.
using PositionKey = std::array<u32, 3>;

static PositionKey MakePositionKey(const Vertex& vertex)
{
    return { std::bit_cast<u32>(vertex.Position.x), std::bit_cast<u32>(vertex.Position.y),
             std::bit_cast<u32>(vertex.Position.z) };
}

static std::array<PositionKey, 3> ClusterTriangleKeys(const VirtualMesh& vm, const VirtualCluster& cluster, u32 triangle)
{
    std::array<PositionKey, 3> keys;
    for (u32 corner = 0; corner < 3; ++corner)
    {
        u8 const local = vm.ClusterTriangles[cluster.TriangleOffset + (static_cast<sizet>(triangle) * 3) + corner];
        u32 const ref = vm.ClusterVertexRefs[cluster.VertexOffset + local];
        keys[corner] = MakePositionKey(vm.Vertices[ref]);
    }
    return keys;
}

// Canonical (order-independent) triangle multiset of a set of clusters.
static std::vector<std::array<PositionKey, 3>> CanonicalTriangles(const VirtualMesh& vm, const std::vector<u32>& clusterIndices)
{
    std::vector<std::array<PositionKey, 3>> triangles;
    for (u32 const clusterIndex : clusterIndices)
    {
        const VirtualCluster& cluster = vm.Clusters[clusterIndex];
        for (u32 t = 0; t < cluster.TriangleCount; ++t)
        {
            auto keys = ClusterTriangleKeys(vm, cluster, t);
            std::ranges::sort(keys);
            triangles.push_back(keys);
        }
    }
    std::ranges::sort(triangles);
    return triangles;
}

static std::vector<std::array<PositionKey, 3>> CanonicalSourceTriangles(const MeshSource& mesh)
{
    const auto& verts = mesh.GetVertices();
    const auto& idxs = mesh.GetIndices();
    std::vector<std::array<PositionKey, 3>> triangles;
    triangles.reserve(static_cast<sizet>(idxs.Num()) / 3);
    for (i32 i = 0; i + 2 < idxs.Num(); i += 3)
    {
        std::array<PositionKey, 3> tri = { MakePositionKey(verts[static_cast<i32>(idxs[i])]),
                                           MakePositionKey(verts[static_cast<i32>(idxs[i + 1])]),
                                           MakePositionKey(verts[static_cast<i32>(idxs[i + 2])]) };
        std::ranges::sort(tri);
        triangles.push_back(tri);
    }
    std::ranges::sort(triangles);
    return triangles;
}

// Watertightness of a cluster selection on a CLOSED manifold: every undirected edge of
// the selected triangle set must be shared by exactly two triangles. A crack (mismatched
// LOD boundary), a hole, or double-coverage all break the exactly-two property.
static void ExpectWatertightSelection(const VirtualMesh& vm, const std::vector<u32>& selected, const char* label)
{
    ASSERT_FALSE(selected.empty()) << label << ": selection is empty";

    std::map<std::pair<PositionKey, PositionKey>, int> edgeCounts;
    for (u32 const clusterIndex : selected)
    {
        const VirtualCluster& cluster = vm.Clusters[clusterIndex];
        for (u32 t = 0; t < cluster.TriangleCount; ++t)
        {
            auto keys = ClusterTriangleKeys(vm, cluster, t);
            for (u32 e = 0; e < 3; ++e)
            {
                std::pair<PositionKey, PositionKey> const edge = std::minmax(keys[e], keys[(e + 1) % 3]);
                ++edgeCounts[edge];
            }
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
                            << " edges are not shared by exactly 2 triangles (crack/hole/overlap in the LOD cut)";
}

// Every threshold that can flip a selection decision: just below/above each distinct
// group error, the exact errors themselves, plus the leaf (-1) and coarse extremes.
static std::vector<f32> InterestingThresholds(const VirtualMesh& vm)
{
    std::vector<f32> errors;
    for (const VirtualClusterGroup& group : vm.Groups)
    {
        if (group.LODBounds.Error < std::numeric_limits<f32>::max())
        {
            errors.push_back(group.LODBounds.Error);
        }
    }
    std::ranges::sort(errors);
    errors.erase(std::unique(errors.begin(), errors.end()), errors.end());

    std::vector<f32> thresholds{ -1.0f, 0.0f };
    f32 previous = 0.0f;
    for (f32 const error : errors)
    {
        thresholds.push_back((previous + error) * 0.5f); // between consecutive distinct errors
        thresholds.push_back(error);                     // exactly at a transition
        previous = error;
    }
    thresholds.push_back(previous * 2.0f + 1.0f); // beyond every finite error
    return thresholds;
}

static u32 SelectedTriangleCount(const VirtualMesh& vm, const std::vector<u32>& selected)
{
    u32 total = 0;
    for (u32 const clusterIndex : selected)
    {
        total += vm.Clusters[clusterIndex].TriangleCount;
    }
    return total;
}

// =============================================================================
// Structural validity
// =============================================================================

TEST(VirtualMeshBuilder, BuildProducesStructurallyValidDAG)
{
    auto mesh = MakeIcosphereMesh(4); // 5120 triangles
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    VirtualMeshBuildConfig const defaults;

    // Group ranges tile Clusters contiguously and agree with each cluster's GroupIndex
    u64 runningFirst = 0;
    for (sizet g = 0; g < vm.Groups.size(); ++g)
    {
        const VirtualClusterGroup& group = vm.Groups[g];
        EXPECT_EQ(group.FirstCluster, runningFirst);
        EXPECT_GT(group.ClusterCount, 0u);
        EXPECT_LT(group.Depth, vm.LevelCount);
        runningFirst += group.ClusterCount;

        for (u32 c = group.FirstCluster; c < group.FirstCluster + group.ClusterCount; ++c)
        {
            EXPECT_EQ(vm.Clusters[c].GroupIndex, static_cast<i32>(g));
        }
    }
    EXPECT_EQ(runningFirst, vm.Clusters.size());

    for (const VirtualCluster& cluster : vm.Clusters)
    {
        EXPECT_GT(cluster.TriangleCount, 0u);
        EXPECT_LE(cluster.TriangleCount, defaults.MaxClusterTriangles);
        EXPECT_GT(cluster.VertexCount, 0u);
        EXPECT_LE(cluster.VertexCount, defaults.MaxClusterVertices);

        ASSERT_LE(static_cast<u64>(cluster.VertexOffset) + cluster.VertexCount, vm.ClusterVertexRefs.size());
        ASSERT_LE(static_cast<u64>(cluster.TriangleOffset) + (static_cast<u64>(cluster.TriangleCount) * 3),
                  vm.ClusterTriangles.size());

        for (u32 t = 0; t < cluster.TriangleCount * 3; ++t)
        {
            EXPECT_LT(vm.ClusterTriangles[cluster.TriangleOffset + t], cluster.VertexCount);
        }
        for (u32 v = 0; v < cluster.VertexCount; ++v)
        {
            EXPECT_LT(vm.ClusterVertexRefs[cluster.VertexOffset + v], vm.Vertices.size());
        }

        if (cluster.RefinedGroup >= 0)
        {
            EXPECT_LT(static_cast<sizet>(cluster.RefinedGroup), vm.Groups.size());
        }
    }

    // A 5120-triangle sphere must produce a genuinely hierarchical DAG
    EXPECT_GE(vm.LevelCount, 2u);
    EXPECT_EQ(vm.SourceTriangleCount, 5120u);
}

TEST(VirtualMeshBuilder, LeafClustersExactlyPartitionSourceTriangles)
{
    for (auto& mesh : { MakeIcosphereMesh(3), MakeGridMesh(24) })
    {
        auto vm = VirtualMeshBuilder::Build(*mesh);
        ASSERT_TRUE(vm.IsValid());

        std::vector<u32> leaves;
        for (sizet i = 0; i < vm.Clusters.size(); ++i)
        {
            if (vm.Clusters[i].RefinedGroup < 0)
            {
                leaves.push_back(static_cast<u32>(i));
            }
        }

        EXPECT_EQ(CanonicalTriangles(vm, leaves), CanonicalSourceTriangles(*mesh))
            << "LOD-0 clusters must contain exactly the source triangle multiset";
    }
}

TEST(VirtualMeshBuilder, ClusterCullBoundsContainReferencedVertices)
{
    auto mesh = MakeIcosphereMesh(4);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    for (const VirtualCluster& cluster : vm.Clusters)
    {
        EXPECT_GE(cluster.BoundsRadius, 0.0f);
        f32 const limit = cluster.BoundsRadius * 1.0001f + 1e-5f;
        for (u32 v = 0; v < cluster.VertexCount; ++v)
        {
            u32 const ref = vm.ClusterVertexRefs[cluster.VertexOffset + v];
            f32 const distance = glm::length(vm.Vertices[ref].Position - cluster.BoundsCenter);
            EXPECT_LE(distance, limit) << "vertex outside its cluster's culling sphere";
        }
    }
}

TEST(VirtualMeshBuilder, GroupLODSphereContainsMemberGeometry)
{
    auto mesh = MakeIcosphereMesh(4);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    for (const VirtualClusterGroup& group : vm.Groups)
    {
        f32 const limit = group.LODBounds.Radius * 1.0001f + 1e-5f;
        for (u32 c = group.FirstCluster; c < group.FirstCluster + group.ClusterCount; ++c)
        {
            const VirtualCluster& cluster = vm.Clusters[c];
            for (u32 v = 0; v < cluster.VertexCount; ++v)
            {
                u32 const ref = vm.ClusterVertexRefs[cluster.VertexOffset + v];
                f32 const distance = glm::length(vm.Vertices[ref].Position - group.LODBounds.Center);
                EXPECT_LE(distance, limit) << "member vertex outside its group's LOD sphere";
            }
        }
    }
}

// =============================================================================
// The monotone-DAG contract (acceptance criterion: "monotone parent error")
// =============================================================================

TEST(VirtualMeshBuilder, GroupErrorNeverDecreasesAlongRefinementEdges)
{
    auto mesh = MakeIcosphereMesh(4);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    sizet refinementEdges = 0;
    for (const VirtualCluster& cluster : vm.Clusters)
    {
        ASSERT_GE(cluster.GroupIndex, 0);
        f32 const parentError = vm.Groups[static_cast<sizet>(cluster.GroupIndex)].LODBounds.Error;
        EXPECT_GE(parentError, 0.0f);

        if (cluster.RefinedGroup >= 0)
        {
            ++refinementEdges;
            f32 const selfError = vm.Groups[static_cast<sizet>(cluster.RefinedGroup)].LODBounds.Error;
            EXPECT_GE(parentError, selfError)
                << "DAG error must be monotone: member group error >= producing group error";
            EXPECT_LT(selfError, std::numeric_limits<f32>::max())
                << "a terminal group must never produce refined clusters";
        }
    }
    EXPECT_GT(refinementEdges, 0u) << "a 5120-triangle mesh must produce refined (non-leaf) clusters";

    // Terminal groups close the DAG; every non-terminal group must carry a strictly
    // positive error — simplifying curved geometry always deforms the surface, and a
    // builder that forgot to fold the simplification error into the group error would
    // leave every finite error at the leaves' seeded 0.
    sizet terminalGroups = 0;
    std::vector<f32> finiteErrors;
    for (const VirtualClusterGroup& group : vm.Groups)
    {
        if (group.LODBounds.Error >= std::numeric_limits<f32>::max())
        {
            ++terminalGroups;
        }
        else
        {
            EXPECT_GT(group.LODBounds.Error, 0.0f)
                << "simplifying curved geometry must accrue nonzero absolute error";
            finiteErrors.push_back(group.LODBounds.Error);
        }
    }
    EXPECT_GE(terminalGroups, 1u);

    // Multiple genuinely distinct LOD transition points must exist across the levels
    std::ranges::sort(finiteErrors);
    finiteErrors.erase(std::unique(finiteErrors.begin(), finiteErrors.end()), finiteErrors.end());
    EXPECT_GE(finiteErrors.size(), 2u);
}

TEST(VirtualMeshBuilder, GroupLODSphereContainsProducingGroupSphere)
{
    auto mesh = MakeIcosphereMesh(4);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    for (const VirtualCluster& cluster : vm.Clusters)
    {
        if (cluster.RefinedGroup < 0)
        {
            continue;
        }
        const VirtualLODBounds& parent = vm.Groups[static_cast<sizet>(cluster.GroupIndex)].LODBounds;
        const VirtualLODBounds& child = vm.Groups[static_cast<sizet>(cluster.RefinedGroup)].LODBounds;

        f32 const centerDistance = glm::length(parent.Center - child.Center);
        EXPECT_LE(centerDistance + child.Radius, parent.Radius * 1.0001f + 1e-5f)
            << "member group's LOD sphere must contain the producing group's LOD sphere "
            << "(otherwise projected error is not monotone from every viewpoint)";
    }
}

TEST(VirtualMeshBuilder, ProjectedErrorIsMonotoneFromArbitraryViewpoints)
{
    auto mesh = MakeIcosphereMesh(4);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    constexpr f32 kZNear = 0.05f;
    constexpr f32 kProjectionScale = 1.7320508f; // cot(fovY/2) for fovY = 60 degrees

    std::mt19937 rng(12345);
    std::uniform_real_distribution<f32> coord(-3.0f, 3.0f);

    for (int view = 0; view < 16; ++view)
    {
        glm::vec3 const cameraPosition{ coord(rng), coord(rng), coord(rng) };
        for (const VirtualCluster& cluster : vm.Clusters)
        {
            if (cluster.RefinedGroup < 0)
            {
                continue;
            }
            f32 const parentProjected = vm.Groups[static_cast<sizet>(cluster.GroupIndex)]
                                            .LODBounds.ProjectError(cameraPosition, kZNear, kProjectionScale);
            f32 const childProjected = vm.Groups[static_cast<sizet>(cluster.RefinedGroup)]
                                           .LODBounds.ProjectError(cameraPosition, kZNear, kProjectionScale);
            EXPECT_GE(parentProjected, childProjected * (1.0f - 1e-4f))
                << "projected error regressed for camera " << view;
        }
    }
}

// =============================================================================
// The crack-free cut contract (acceptance criterion: "crack-free transitions")
// =============================================================================

TEST(VirtualMeshBuilder, CutAtNegativeThresholdSelectsExactlyTheLeafClusters)
{
    auto mesh = MakeIcosphereMesh(3);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    auto selected = vm.SelectClusters(-1.0f);
    std::vector<u32> leaves;
    for (sizet i = 0; i < vm.Clusters.size(); ++i)
    {
        if (vm.Clusters[i].RefinedGroup < 0)
        {
            leaves.push_back(static_cast<u32>(i));
        }
    }
    EXPECT_EQ(selected, leaves) << "a threshold below every error must select the full-detail cut";
}

TEST(VirtualMeshBuilder, CutIsWatertightAtEveryThreshold)
{
    auto mesh = MakeIcosphereMesh(4); // closed manifold — every edge borders exactly 2 triangles
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    auto thresholds = InterestingThresholds(vm);
    EXPECT_GE(thresholds.size(), 5u) << "expected several distinct LOD transitions on a 5120-triangle sphere";

    for (f32 const threshold : thresholds)
    {
        auto selected = vm.SelectClusters(threshold);
        ExpectWatertightSelection(vm, selected, ("object-space threshold " + std::to_string(threshold)).c_str());
    }
}

TEST(VirtualMeshBuilder, ProjectedCutIsWatertightFromMultipleCameras)
{
    auto mesh = MakeIcosphereMesh(4);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    constexpr f32 kZNear = 0.05f;
    constexpr f32 kProjectionScale = 1.7320508f;

    const glm::vec3 cameras[] = { { 3.0f, 0.5f, 0.0f }, { 0.0f, -1.6f, 1.4f }, { -8.0f, 6.0f, -5.0f } };
    const f32 thresholds[] = { 0.0005f, 0.005f, 0.05f };

    for (const glm::vec3& camera : cameras)
    {
        for (f32 const threshold : thresholds)
        {
            auto selected = vm.SelectClustersProjected(camera, kZNear, kProjectionScale, threshold);
            ExpectWatertightSelection(vm, selected, "projected cut");
        }
    }
}

TEST(VirtualMeshBuilder, SeamedMeshCutsStayWatertightAcrossDuplicatedVertices)
{
    auto seamed = MakeSeamedIcosphereMesh(4);

    // Fixture sanity: the seam really duplicated positions (same position, shifted UV)
    auto plainVertexCount = MakeIcosphereMesh(4)->GetVertices().Num();
    ASSERT_GT(seamed->GetVertices().Num(), plainVertexCount);

    auto vm = VirtualMeshBuilder::Build(*seamed);
    ASSERT_TRUE(vm.IsValid());

    // The full-detail cut still carries exactly the source surface
    std::vector<u32> leaves;
    for (sizet i = 0; i < vm.Clusters.size(); ++i)
    {
        if (vm.Clusters[i].RefinedGroup < 0)
        {
            leaves.push_back(static_cast<u32>(i));
        }
    }
    EXPECT_EQ(CanonicalTriangles(vm, leaves), CanonicalSourceTriangles(*seamed));

    // Position-keyed watertightness across every LOD transition: if the builder did not
    // treat the seam duplicates as one canonical point (position remap + boundary
    // locks), the two sides of the seam would simplify apart and open cracks.
    for (f32 const threshold : InterestingThresholds(vm))
    {
        auto selected = vm.SelectClusters(threshold);
        ExpectWatertightSelection(vm, selected, ("seamed mesh, threshold " + std::to_string(threshold)).c_str());
    }
}

TEST(VirtualMeshBuilder, MaxLevelsCapProducesLoadableWatertightDAG)
{
    // Force the MaxLevels safety-cap path: the 5120-triangle sphere needs more than two
    // levels to reach a single root, so the build must close the DAG with a terminal
    // cap group at Depth == MaxLevels — and the resulting blob must still deserialize
    // (LevelCount == MaxLevels + 1 must stay within the shared blob-format cap).
    VirtualMeshBuildConfig config;
    config.MaxLevels = 2;

    auto mesh = MakeIcosphereMesh(4);
    auto vm = VirtualMeshBuilder::Build(*mesh, config);
    ASSERT_TRUE(vm.IsValid());
    EXPECT_LE(vm.LevelCount, 3u);

    for (f32 const threshold : InterestingThresholds(vm))
    {
        ExpectWatertightSelection(vm, vm.SelectClusters(threshold), "MaxLevels-capped cut");
    }

    auto blob = VirtualMeshSerializer::SerializeToBlob(vm);
    VirtualMesh loaded;
    EXPECT_TRUE(VirtualMeshSerializer::DeserializeFromBlob(blob, loaded))
        << "every mesh the builder can produce must round-trip through the blob format";
}

TEST(VirtualMeshBuilder, CutTriangleCountIsMonotoneInThreshold)
{
    auto mesh = MakeIcosphereMesh(4);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    auto thresholds = InterestingThresholds(vm); // sorted ascending by construction
    u32 previousCount = std::numeric_limits<u32>::max();
    for (f32 const threshold : thresholds)
    {
        u32 const count = SelectedTriangleCount(vm, vm.SelectClusters(threshold));
        EXPECT_LE(count, previousCount) << "raising the error threshold must never add triangles";
        previousCount = count;
    }

    // The full-detail cut is the source mesh; the coarsest cut must be a real reduction
    u32 const leafCount = SelectedTriangleCount(vm, vm.SelectClusters(-1.0f));
    EXPECT_EQ(leafCount, vm.SourceTriangleCount);
    u32 const coarseCount = SelectedTriangleCount(vm, vm.SelectClusters(thresholds.back()));
    EXPECT_LE(coarseCount, vm.SourceTriangleCount / 4)
        << "the coarsest LOD cut of a 5120-triangle sphere should be a small fraction of the source";
}

// =============================================================================
// Degenerate and unsupported inputs
// =============================================================================

TEST(VirtualMeshBuilder, RejectsEmptySources)
{
    auto empty = Ref<MeshSource>::Create();
    EXPECT_FALSE(VirtualMeshBuilder::Build(*empty).IsValid());
    EXPECT_FALSE(VirtualMeshBuilder::BuildSet(*empty).IsValid());
}

// Multi-submesh sources used to be rejected outright, which made every multi-material
// asset (Sponza, the backpack — anything with more than one usemtl) unusable as virtual
// geometry. BuildSet now emits ONE DAG PER SUBMESH: a cluster must never span a material
// boundary, since a group is simplified as a unit and a straddling cluster could not be
// shaded by either material.
TEST(VirtualMeshBuilder, BuildSetEmitsOneDagPerSubmesh)
{
    auto multi = MakeGridMesh(8);
    auto const totalIndices = static_cast<u32>(multi->GetIndices().Num());
    auto const halfIndices = (totalIndices / 6u) * 3u; // triangle-aligned split

    Submesh first;
    first.m_BaseIndex = 0;
    first.m_IndexCount = halfIndices;
    first.m_MaterialIndex = 0;
    multi->AddSubmesh(first);

    Submesh second;
    second.m_BaseIndex = halfIndices;
    second.m_IndexCount = totalIndices - halfIndices;
    second.m_MaterialIndex = 1;
    multi->AddSubmesh(second);

    VirtualMeshSet const set = VirtualMeshBuilder::BuildSet(*multi);
    ASSERT_TRUE(set.IsValid());
    ASSERT_EQ(set.Parts.size(), 2u);

    // Each part carries the submesh it came from and that submesh's material.
    EXPECT_EQ(set.Parts[0].SubmeshIndex, 0u);
    EXPECT_EQ(set.Parts[0].MaterialIndex, 0u);
    EXPECT_EQ(set.Parts[1].SubmeshIndex, 1u);
    EXPECT_EQ(set.Parts[1].MaterialIndex, 1u);

    // The parts partition the source triangles exactly — no triangle is dropped, and none
    // is built into two DAGs.
    EXPECT_EQ(set.Parts[0].Dag.SourceTriangleCount, halfIndices / 3u);
    EXPECT_EQ(set.Parts[1].Dag.SourceTriangleCount, (totalIndices - halfIndices) / 3u);
    EXPECT_EQ(set.TotalSourceTriangles(), totalIndices / 3u);

    for (const auto& part : set.Parts)
    {
        EXPECT_TRUE(part.Dag.IsValid());
    }
}

// A source with no submesh records is one implicit submesh covering the whole buffer.
TEST(VirtualMeshBuilder, BuildSetTreatsASubmeshlessSourceAsOnePart)
{
    auto mesh = MakeGridMesh(8);
    VirtualMeshSet const set = VirtualMeshBuilder::BuildSet(*mesh);

    ASSERT_TRUE(set.IsValid());
    ASSERT_EQ(set.Parts.size(), 1u);
    EXPECT_EQ(set.Parts[0].SubmeshIndex, 0u);
    EXPECT_EQ(set.TotalSourceTriangles(), static_cast<u32>(mesh->GetIndices().Num()) / 3u);
}

// The set blob wraps the hardened single-mesh one; it must round-trip every part with its
// submesh/material tags, and must still accept a legacy single-DAG "OVGM" cook.
TEST(VirtualMeshSerializer, SetBlobRoundTripsEveryPart)
{
    auto multi = MakeGridMesh(8);
    auto const totalIndices = static_cast<u32>(multi->GetIndices().Num());
    auto const halfIndices = (totalIndices / 6u) * 3u;

    Submesh first;
    first.m_BaseIndex = 0;
    first.m_IndexCount = halfIndices;
    first.m_MaterialIndex = 3;
    multi->AddSubmesh(first);

    Submesh second;
    second.m_BaseIndex = halfIndices;
    second.m_IndexCount = totalIndices - halfIndices;
    second.m_MaterialIndex = 7;
    multi->AddSubmesh(second);

    VirtualMeshSet const built = VirtualMeshBuilder::BuildSet(*multi);
    ASSERT_EQ(built.Parts.size(), 2u);

    std::vector<u8> const blob = VirtualMeshSerializer::SerializeSetToBlob(built);
    ASSERT_FALSE(blob.empty());

    VirtualMeshSet restored;
    ASSERT_TRUE(VirtualMeshSerializer::DeserializeSetFromBlob(blob, restored));
    ASSERT_EQ(restored.Parts.size(), built.Parts.size());

    for (sizet i = 0; i < built.Parts.size(); ++i)
    {
        EXPECT_EQ(restored.Parts[i].SubmeshIndex, built.Parts[i].SubmeshIndex);
        EXPECT_EQ(restored.Parts[i].MaterialIndex, built.Parts[i].MaterialIndex);
        EXPECT_EQ(restored.Parts[i].Dag.Clusters.size(), built.Parts[i].Dag.Clusters.size());
        EXPECT_EQ(restored.Parts[i].Dag.SourceTriangleCount, built.Parts[i].Dag.SourceTriangleCount);
    }
    EXPECT_EQ(restored.Parts[1].MaterialIndex, 7u);
}

TEST(VirtualMeshSerializer, SetReaderAcceptsALegacySingleDagBlob)
{
    // Cooks written before multi-submesh support are bare "OVGM" blobs. They must load as a
    // one-part set rather than forcing a re-cook of every cached asset.
    auto mesh = MakeGridMesh(8);
    VirtualMesh const single = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(single.IsValid());

    std::vector<u8> const legacyBlob = VirtualMeshSerializer::SerializeToBlob(single);

    VirtualMeshSet restored;
    ASSERT_TRUE(VirtualMeshSerializer::DeserializeSetFromBlob(legacyBlob, restored));
    ASSERT_EQ(restored.Parts.size(), 1u);
    EXPECT_EQ(restored.Parts[0].SubmeshIndex, 0u);
    EXPECT_EQ(restored.Parts[0].Dag.Clusters.size(), single.Clusters.size());
    EXPECT_EQ(restored.Parts[0].Dag.SourceTriangleCount, single.SourceTriangleCount);
}

TEST(VirtualMeshSerializer, SetReaderRejectsCorruptBlobs)
{
    auto mesh = MakeGridMesh(8);
    VirtualMeshSet const built = VirtualMeshBuilder::BuildSet(*mesh);
    std::vector<u8> const blob = VirtualMeshSerializer::SerializeSetToBlob(built);

    VirtualMeshSet out;
    EXPECT_FALSE(VirtualMeshSerializer::DeserializeSetFromBlob({}, out));

    // Truncated
    EXPECT_FALSE(VirtualMeshSerializer::DeserializeSetFromBlob(
        std::span<const u8>(blob.data(), blob.size() / 2), out));

    // Trailing garbage — the exact-size check must reject it.
    std::vector<u8> padded = blob;
    padded.push_back(0xAB);
    EXPECT_FALSE(VirtualMeshSerializer::DeserializeSetFromBlob(padded, out));

    // Bad magic
    std::vector<u8> badMagic = blob;
    badMagic[0] ^= 0xFF;
    EXPECT_FALSE(VirtualMeshSerializer::DeserializeSetFromBlob(badMagic, out));
}

TEST(VirtualMeshBuilder, TinyMeshBecomesSingleTerminalCluster)
{
    auto mesh = MakeQuadMesh();
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    ASSERT_EQ(vm.Clusters.size(), 1u);
    ASSERT_EQ(vm.Groups.size(), 1u);
    EXPECT_EQ(vm.Clusters[0].RefinedGroup, -1);
    EXPECT_EQ(vm.Clusters[0].TriangleCount, 2u);
    EXPECT_GE(vm.Groups[0].LODBounds.Error, std::numeric_limits<f32>::max());
    EXPECT_EQ(vm.LevelCount, 1u);

    // The lone cluster is selected at any threshold
    EXPECT_EQ(vm.SelectClusters(-1.0f), std::vector<u32>{ 0u });
    EXPECT_EQ(vm.SelectClusters(1e6f), std::vector<u32>{ 0u });
}

TEST(VirtualMeshBuilder, BuildIsDeterministic)
{
    auto meshA = MakeIcosphereMesh(3);
    auto meshB = MakeIcosphereMesh(3);

    auto blobA = VirtualMeshSerializer::SerializeToBlob(VirtualMeshBuilder::Build(*meshA));
    auto blobB = VirtualMeshSerializer::SerializeToBlob(VirtualMeshBuilder::Build(*meshB));

    ASSERT_FALSE(blobA.empty());
    EXPECT_EQ(blobA, blobB) << "the cook must be bit-reproducible for identical input";
}

// =============================================================================
// Serialization
// =============================================================================

TEST(VirtualMeshSerializer, RoundTripIsExact)
{
    auto mesh = MakeIcosphereMesh(3);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());

    auto blob = VirtualMeshSerializer::SerializeToBlob(vm);
    ASSERT_FALSE(blob.empty());

    VirtualMesh loaded;
    ASSERT_TRUE(VirtualMeshSerializer::DeserializeFromBlob(blob, loaded));

    EXPECT_EQ(loaded.Clusters.size(), vm.Clusters.size());
    EXPECT_EQ(loaded.Groups.size(), vm.Groups.size());
    EXPECT_EQ(loaded.Vertices.size(), vm.Vertices.size());
    EXPECT_EQ(loaded.ClusterVertexRefs, vm.ClusterVertexRefs);
    EXPECT_EQ(loaded.ClusterTriangles, vm.ClusterTriangles);
    EXPECT_EQ(loaded.LevelCount, vm.LevelCount);
    EXPECT_EQ(loaded.SourceTriangleCount, vm.SourceTriangleCount);

    // Byte-exact round trip: re-serializing the loaded mesh reproduces the blob
    EXPECT_EQ(VirtualMeshSerializer::SerializeToBlob(loaded), blob);
}

TEST(VirtualMeshSerializer, RejectsCorruptedBlobs)
{
    auto mesh = MakeIcosphereMesh(2);
    auto vm = VirtualMeshBuilder::Build(*mesh);
    ASSERT_TRUE(vm.IsValid());
    auto blob = VirtualMeshSerializer::SerializeToBlob(vm);

    VirtualMesh out;

    // OVGM header: magic, wire version, builder version, build-config fingerprint, then the
    // 7 counts (issue #629 added the two cook-identity words). Every byte poke below is
    // relative to it, so it MUST track the writer — it used to be a bare `36` and, when the
    // header grew, each poke silently landed in the wrong section and corrupted nothing.
    constexpr sizet kHeaderBytes = 11 * sizeof(u32);

    // Empty / truncated input
    EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob({}, out));
    EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(std::span<const u8>(blob.data(), 8), out));
    EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(std::span<const u8>(blob.data(), blob.size() / 2), out));
    EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(std::span<const u8>(blob.data(), blob.size() - 1), out));

    // Trailing garbage
    {
        auto oversized = blob;
        oversized.push_back(0);
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(oversized, out));
    }

    // Bad magic
    {
        auto corrupted = blob;
        corrupted[0] ^= 0xFF;
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // Unknown (future) wire version. Must be a version this build does NOT write —
    // the blob format is at v2 since the cook-identity header (issue #629), so hard-coding
    // "2" here silently stopped testing anything. Derive it from a byte that cannot collide:
    // read the version the writer actually emitted and add one.
    {
        auto corrupted = blob;
        u32 currentVersion = 0;
        std::memcpy(&currentVersion, corrupted.data() + 4, sizeof(u32));
        u32 const futureVersion = currentVersion + 1;
        std::memcpy(corrupted.data() + 4, &futureVersion, sizeof(u32));
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // Allocation-bomb vertex count
    {
        auto corrupted = blob;
        u32 const hugeCount = 0x7FFFFFFF;
        std::memcpy(corrupted.data() + 8, &hugeCount, sizeof(u32));
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // Out-of-range group index on the first cluster
    {
        auto corrupted = blob;
        sizet const clusterTableOffset = kHeaderBytes + vm.Vertices.size() * 32; // header + vertices
        sizet const groupIndexOffset = clusterTableOffset + 4 * sizeof(u32);
        i32 const bogusGroup = static_cast<i32>(vm.Groups.size()) + 7;
        std::memcpy(corrupted.data() + groupIndexOffset, &bogusGroup, sizeof(i32));
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // Non-finite float in the vertex payload
    {
        auto corrupted = blob;
        f32 const nan = std::numeric_limits<f32>::quiet_NaN();
        std::memcpy(corrupted.data() + kHeaderBytes, &nan, sizeof(f32));
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // Wire-format section offsets (vertex 32, cluster 68, group 32 bytes)
    sizet const clusterTable = kHeaderBytes + vm.Vertices.size() * 32;
    sizet const groupTable = clusterTable + vm.Clusters.size() * 68;
    sizet const refsTable = groupTable + vm.Groups.size() * 32;
    sizet const trianglesTable = refsTable + vm.ClusterVertexRefs.size() * 4;

    // Out-of-range cluster vertex reference (first ref -> one past the last vertex)
    {
        auto corrupted = blob;
        auto const badRef = static_cast<u32>(vm.Vertices.size());
        std::memcpy(corrupted.data() + refsTable, &badRef, sizeof(u32));
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // Out-of-range local triangle index in the first cluster's window
    {
        ASSERT_LT(vm.Clusters[0].VertexCount, 256u); // u8 local indices must have a pokeable out-of-range value
        auto corrupted = blob;
        corrupted[trianglesTable + vm.Clusters[0].TriangleOffset] = static_cast<u8>(vm.Clusters[0].VertexCount);
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // Self-referential refinement edge (RefinedGroup == GroupIndex) breaks the DAG
    {
        sizet refinedClusterIndex = vm.Clusters.size();
        for (sizet i = 0; i < vm.Clusters.size(); ++i)
        {
            if (vm.Clusters[i].RefinedGroup >= 0)
            {
                refinedClusterIndex = i;
                break;
            }
        }
        ASSERT_LT(refinedClusterIndex, vm.Clusters.size()) << "fixture must contain refined clusters";

        auto corrupted = blob;
        i32 const selfGroup = vm.Clusters[refinedClusterIndex].GroupIndex;
        std::memcpy(corrupted.data() + clusterTable + refinedClusterIndex * 68 + 20, &selfGroup, sizeof(i32));
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // All-zero group errors: refinement edges stay "monotone" but the terminal-closure
    // invariant breaks (a never-refined group with finite error leaves holes in coarse cuts)
    {
        auto corrupted = blob;
        f32 const zero = 0.0f;
        for (sizet g = 0; g < vm.Groups.size(); ++g)
        {
            std::memcpy(corrupted.data() + groupTable + g * 32 + 28, &zero, sizeof(f32));
        }
        EXPECT_FALSE(VirtualMeshSerializer::DeserializeFromBlob(corrupted, out));
    }

    // The pristine blob still loads after all of the above
    EXPECT_TRUE(VirtualMeshSerializer::DeserializeFromBlob(blob, out));
}
