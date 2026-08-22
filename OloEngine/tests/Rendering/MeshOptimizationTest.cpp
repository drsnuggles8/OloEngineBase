#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/Vertex.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Helper: create a simple quad mesh (2 triangles, 4 vertices)
// =============================================================================

static Ref<MeshSource> MakeQuadMesh()
{
    TArray<Vertex> vertices;
    vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }));
    vertices.Add(Vertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }));

    TArray<u32> indices;
    indices.Add(0);
    indices.Add(1);
    indices.Add(2);
    indices.Add(0);
    indices.Add(2);
    indices.Add(3);

    return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
}

// =============================================================================
// Helper: create a grid mesh with many triangles for meaningful simplification
// =============================================================================

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
            vertices.Add(Vertex(
                { fx, 0.0f, fz },
                { 0.0f, 1.0f, 0.0f },
                { fx, fz }));
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

            indices.Add(i0);
            indices.Add(i1);
            indices.Add(i2);
            indices.Add(i1);
            indices.Add(i3);
            indices.Add(i2);
        }
    }

    return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
}

// =============================================================================
// Helper: create a wavy grid with varying normals and UVs.
// Sharp UV seams at the center make attribute-aware simplification preferable.
// =============================================================================

static Ref<MeshSource> MakeWavyGridMesh(u32 gridSize)
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
            // Sine-wave displacement creates varying normals
            auto fy = 0.3f * std::sin(fx * 6.0f) * std::cos(fz * 6.0f);

            // Analytical normal from the height-field partial derivatives
            auto dydx = 0.3f * 6.0f * std::cos(fx * 6.0f) * std::cos(fz * 6.0f);
            auto dydz = -0.3f * 6.0f * std::sin(fx * 6.0f) * std::sin(fz * 6.0f);
            glm::vec3 n = glm::normalize(glm::vec3(-dydx, 1.0f, -dydz));

            // UV with a sharp seam at x == 0.5
            auto u = (fx < 0.5f) ? (fx * 2.0f) : ((fx - 0.5f) * 2.0f);
            vertices.Add(Vertex({ fx, fy, fz }, n, { u, fz }));
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

            indices.Add(i0);
            indices.Add(i1);
            indices.Add(i2);
            indices.Add(i1);
            indices.Add(i3);
            indices.Add(i2);
        }
    }

    return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
}

// =============================================================================
// Helper: a closed, curved, UNWELDED icosphere — the true analogue of the Stanford
// dragon (a closed organic surface imported without normals). Assimp's aiProcess_GenNormals
// synthesises FLAT per-face normals, splitting every shared vertex, and
// aiProcess_JoinIdenticalVertices cannot re-merge them (normals differ per face). meshopt
// derives adjacency from shared INDICES, so this is a disconnected triangle soup to the
// simplifier — every internal edge is a spurious one-sided border. On a CLOSED surface there
// is no real boundary to relieve those spurious borders, so nothing collapses. (Issue #653 /
// found via #651.)
// =============================================================================
static Ref<MeshSource> MakeUnweldedIcosphere(u32 subdivisions)
{
    const f32 t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::vector<glm::vec3> pos = { { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t }, { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    for (auto& p : pos)
    {
        p = glm::normalize(p);
    }
    std::vector<u32> idx = { 0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11,
                             4, 11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3,
                             6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1 };
    for (u32 s = 0; s < subdivisions; ++s)
    {
        std::vector<u32> next;
        for (sizet i = 0; i + 2 < idx.size(); i += 3)
        {
            const u32 a = idx[i], b = idx[i + 1], c = idx[i + 2];
            const auto mid = [&](u32 x, u32 y) -> u32
            {
                pos.push_back(glm::normalize((pos[x] + pos[y]) * 0.5f));
                return static_cast<u32>(pos.size() - 1);
            };
            const u32 ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
            for (u32 v : { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca })
            {
                next.push_back(v);
            }
        }
        idx = std::move(next);
    }

    // Emit UNWELDED: one fresh vertex trio per triangle, with the flat face normal.
    TArray<Vertex> vertices;
    TArray<u32> indices;
    for (sizet i = 0; i + 2 < idx.size(); i += 3)
    {
        const glm::vec3 a = pos[idx[i]], b = pos[idx[i + 1]], c = pos[idx[i + 2]];
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        const auto base = static_cast<u32>(vertices.Num());
        vertices.Add(Vertex(a, n, { 0.0f, 0.0f }));
        vertices.Add(Vertex(b, n, { 0.0f, 0.0f }));
        vertices.Add(Vertex(c, n, { 0.0f, 0.0f }));
        indices.Add(base);
        indices.Add(base + 1);
        indices.Add(base + 2);
    }
    return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
}

// =============================================================================
// Helper: extract canonical triangle multiset from a mesh
// Each triangle is a sorted triple of positions for order-independent comparison
// =============================================================================

using Vec3Tuple = std::tuple<float, float, float>;
using CanonicalTriangle = std::array<Vec3Tuple, 3>;

static std::vector<CanonicalTriangle> ExtractCanonicalTriangles(const MeshSource& mesh)
{
    const auto& verts = mesh.GetVertices();
    const auto& idxs = mesh.GetIndices();
    std::vector<CanonicalTriangle> triangles;
    triangles.reserve(static_cast<size_t>(idxs.Num()) / 3);

    for (i32 i = 0; i + 2 < idxs.Num(); i += 3)
    {
        CanonicalTriangle tri = { { { verts[idxs[i]].Position.x, verts[idxs[i]].Position.y, verts[idxs[i]].Position.z },
                                    { verts[idxs[i + 1]].Position.x, verts[idxs[i + 1]].Position.y, verts[idxs[i + 1]].Position.z },
                                    { verts[idxs[i + 2]].Position.x, verts[idxs[i + 2]].Position.y, verts[idxs[i + 2]].Position.z } } };
        std::ranges::sort(tri);
        triangles.push_back(tri);
    }

    std::ranges::sort(triangles);
    return triangles;
}

// =============================================================================
// OptimizeMesh Tests
// =============================================================================

TEST(MeshOptimization, OptimizeMeshPreservesVertexCount)
{
    auto mesh = MakeQuadMesh();
    auto originalVertCount = mesh->GetVertices().Num();
    auto originalIdxCount = mesh->GetIndices().Num();

    MeshOptimization::OptimizeMesh(*mesh);

    EXPECT_EQ(mesh->GetVertices().Num(), originalVertCount);
    EXPECT_EQ(mesh->GetIndices().Num(), originalIdxCount);
}

TEST(MeshOptimization, OptimizeMeshPreservesTriangleContent)
{
    auto mesh = MakeGridMesh(4);
    auto originalIdxCount = mesh->GetIndices().Num();

    auto trianglesBefore = ExtractCanonicalTriangles(*mesh);

    MeshOptimization::OptimizeMesh(*mesh);

    // Index count must remain the same (same triangles, different order)
    EXPECT_EQ(mesh->GetIndices().Num(), originalIdxCount);

    // All indices must still be valid
    auto vertCount = static_cast<u32>(mesh->GetVertices().Num());
    for (i32 i = 0; i < mesh->GetIndices().Num(); ++i)
    {
        EXPECT_LT(mesh->GetIndices()[i], vertCount) << "Invalid index at position " << i;
    }

    // The exact same set of triangles (by position) must be present
    auto trianglesAfter = ExtractCanonicalTriangles(*mesh);
    EXPECT_EQ(trianglesBefore, trianglesAfter) << "Optimization altered the triangle set";
}

TEST(MeshOptimization, OptimizeMeshHandlesEmptyMesh)
{
    auto mesh = Ref<MeshSource>::Create();
    // Should not crash
    MeshOptimization::OptimizeMesh(*mesh);
    EXPECT_TRUE(mesh->GetVertices().IsEmpty());
    EXPECT_TRUE(mesh->GetIndices().IsEmpty());
}

TEST(MeshOptimization, OptimizeMeshRemapsBoneInfluences)
{
    auto mesh = MakeQuadMesh();

    // Add bone influences — resize first so assignments are in-bounds
    auto& bones = mesh->GetBoneInfluences();
    bones.SetNum(mesh->GetVertices().Num());
    for (i32 i = 0; i < mesh->GetVertices().Num(); ++i)
    {
        BoneInfluence bi;
        bi.m_BoneIDs[0] = static_cast<u32>(i); // Unique per vertex
        bi.m_Weights[0] = 1.0f;
        bones[i] = bi;
    }

    MeshOptimization::OptimizeMesh(*mesh);

    // Bone influences must still have the same count
    EXPECT_EQ(bones.Num(), mesh->GetVertices().Num());
}

// =============================================================================
// GenerateLODMesh Tests
// =============================================================================

TEST(MeshOptimization, GenerateLODReducesTriangles)
{
    auto mesh = MakeGridMesh(16); // 16x16 grid = 512 triangles
    auto originalTriCount = mesh->GetIndices().Num() / 3;

    auto lod = MeshOptimization::GenerateLODMesh(*mesh, 0.25f);

    ASSERT_NE(lod, nullptr);
    auto lodTriCount = lod->GetIndices().Num() / 3;

    // LOD should have fewer triangles than original
    EXPECT_LT(lodTriCount, originalTriCount);
    EXPECT_GT(lodTriCount, 0);
}

TEST(MeshOptimization, GenerateLODReturnsNullForEmptyMesh)
{
    auto mesh = Ref<MeshSource>::Create();
    auto lod = MeshOptimization::GenerateLODMesh(*mesh, 0.5f);
    EXPECT_EQ(lod, nullptr);
}

TEST(MeshOptimization, GenerateLODPreservesVertexValidity)
{
    auto mesh = MakeGridMesh(8);

    auto lod = MeshOptimization::GenerateLODMesh(*mesh, 0.5f);
    ASSERT_NE(lod, nullptr);

    auto vertCount = static_cast<u32>(lod->GetVertices().Num());
    for (i32 i = 0; i < lod->GetIndices().Num(); ++i)
    {
        EXPECT_LT(lod->GetIndices()[i], vertCount) << "Invalid LOD index at position " << i;
    }
}

TEST(MeshOptimization, GenerateLODWithVeryLowRatioProducesMinimalMesh)
{
    auto mesh = MakeGridMesh(8);

    auto lod = MeshOptimization::GenerateLODMesh(*mesh, 0.01f, 1.0f);
    ASSERT_NE(lod, nullptr);

    // Should produce at least 1 triangle
    EXPECT_GE(lod->GetIndices().Num(), 3);
}

// Issue #653: LOD generation must simplify an UNWELDED mesh, not just a welded one.
//
// meshoptimizer sees an index-unwelded mesh (co-located vertices with distinct
// indices — what Assimp produces for any normal-less source, see MakeUnweldedGridMesh)
// as a disconnected triangle soup and collapses nothing. Every existing LOD test uses a
// welded grid, so this whole class of real-world assets (Stanford scans, raw PLY/OBJ
// without normals) silently produced NO usable LOD — the classic sibling of the #651
// virtual-geometry DAG failure. The fix position-welds the index buffer before
// simplifying (a no-op on an already-welded mesh, so the welded tests are unaffected).
// The reproduction had to be a CLOSED curved surface (an unwelded icosphere — the dragon's
// shape), and finding that out was the whole point of writing the test first. An unwelded
// *open* grid simplifies fine, because collapses are cheap at its real boundary; it is only a
// closed surface — where every "border" is spurious and there is no real boundary to relieve
// it — that collapses to zero. The bug is real (measured: 5120 -> 5120, zero reduction).
TEST(MeshOptimization, GenerateLODSimplifiesAnUnweldedClosedSurface)
{
    auto mesh = MakeUnweldedIcosphere(4); // 20,480 loose triangles, ~61k split vertices
    const auto originalTriCount = mesh->GetIndices().Num() / 3;

    auto lod = MeshOptimization::GenerateLODMesh(*mesh, 0.25f, 1.0f);
    ASSERT_NE(lod, nullptr);
    const auto lodTriCount = lod->GetIndices().Num() / 3;

    RecordProperty("unwelded_original_tris", std::to_string(originalTriCount));
    RecordProperty("unwelded_lod_tris", std::to_string(lodTriCount));

    EXPECT_LT(lodTriCount, originalTriCount * 3 / 4)
        << "GenerateLODMesh did not reduce an unwelded closed surface (" << originalTriCount
        << " -> " << lodTriCount << "). meshopt sees the co-located-but-distinct-index vertices "
                                    "as a disconnected triangle soup; GenerateLODMesh must position-weld the index buffer "
                                    "before simplifying (issue #653).";
    EXPECT_GT(lodTriCount, 0u);

    const auto vertCount = static_cast<u32>(lod->GetVertices().Num());
    for (i32 i = 0; i < lod->GetIndices().Num(); ++i)
    {
        EXPECT_LT(lod->GetIndices()[i], vertCount) << "invalid LOD index at " << i;
    }
}

TEST(MeshOptimization, GenerateLODWithAttributesSimplifiesAnUnweldedClosedSurface)
{
    auto mesh = MakeUnweldedIcosphere(4);
    const auto originalTriCount = mesh->GetIndices().Num() / 3;

    auto lod = MeshOptimization::GenerateLODMeshWithAttributes(*mesh, 0.25f, 1.0f);
    ASSERT_NE(lod, nullptr);
    const auto lodTriCount = lod->GetIndices().Num() / 3;

    EXPECT_LT(lodTriCount, originalTriCount * 3 / 4)
        << "attribute-aware LOD did not reduce an unwelded closed surface (" << originalTriCount
        << " -> " << lodTriCount << ") — same unweld cause as issue #653.";
    EXPECT_GT(lodTriCount, 0u);
}

// =============================================================================
// Shadow Index Buffer Tests
// =============================================================================

TEST(MeshOptimization, GenerateShadowIndicesProducesSameCount)
{
    auto mesh = MakeGridMesh(4);
    auto originalIdxCount = mesh->GetIndices().Num();

    MeshOptimization::GenerateShadowIndices(*mesh);

    EXPECT_TRUE(mesh->HasShadowIndices());
    EXPECT_EQ(mesh->GetShadowIndices().Num(), originalIdxCount);
}

TEST(MeshOptimization, GenerateShadowIndicesValidIndices)
{
    auto mesh = MakeGridMesh(4);
    MeshOptimization::GenerateShadowIndices(*mesh);

    auto vertCount = static_cast<u32>(mesh->GetVertices().Num());
    for (i32 i = 0; i < mesh->GetShadowIndices().Num(); ++i)
    {
        EXPECT_LT(mesh->GetShadowIndices()[i], vertCount) << "Invalid shadow index at position " << i;
    }
}

TEST(MeshOptimization, GenerateShadowIndicesHandlesEmptyMesh)
{
    auto mesh = Ref<MeshSource>::Create();
    MeshOptimization::GenerateShadowIndices(*mesh);
    EXPECT_FALSE(mesh->HasShadowIndices());
}

TEST(MeshOptimization, OptimizeMeshGeneratesShadowIndices)
{
    auto mesh = MakeGridMesh(4);
    MeshOptimization::OptimizeMesh(*mesh);

    // OptimizeMesh should have generated shadow indices as part of its pipeline
    EXPECT_TRUE(mesh->HasShadowIndices());
    EXPECT_EQ(mesh->GetShadowIndices().Num(), mesh->GetIndices().Num());
}

// =============================================================================
// Attribute-Aware LOD Tests
// =============================================================================

TEST(MeshOptimization, GenerateLODWithAttributesReducesTriangles)
{
    auto mesh = MakeGridMesh(16);
    auto originalTriCount = mesh->GetIndices().Num() / 3;

    auto lod = MeshOptimization::GenerateLODMeshWithAttributes(*mesh, 0.25f);

    ASSERT_NE(lod, nullptr);
    auto lodTriCount = lod->GetIndices().Num() / 3;
    EXPECT_LT(lodTriCount, originalTriCount);
    EXPECT_GT(lodTriCount, 0);
}

TEST(MeshOptimization, GenerateLODWithAttributesHandlesEmptyMesh)
{
    auto mesh = Ref<MeshSource>::Create();
    auto lod = MeshOptimization::GenerateLODMeshWithAttributes(*mesh, 0.5f);
    EXPECT_EQ(lod, nullptr);
}

// =============================================================================
// Mesh Analysis Tests
// =============================================================================

TEST(MeshOptimization, AnalyzeMeshReturnsValidStats)
{
    auto mesh = MakeGridMesh(8);
    MeshOptimization::OptimizeMesh(*mesh);

    auto analysis = MeshOptimization::AnalyzeMesh(*mesh);

    EXPECT_EQ(analysis.VertexCount, static_cast<u32>(mesh->GetVertices().Num()));
    EXPECT_EQ(analysis.TriangleCount, static_cast<u32>(mesh->GetIndices().Num()) / 3);
    EXPECT_GT(analysis.ACMR, 0.0f);
    EXPECT_GT(analysis.ATVR, 0.0f);
    EXPECT_GE(analysis.Overdraw, 1.0f); // Can't be less than 1
    EXPECT_GE(analysis.OverfetchRatio, 1.0f);
}

TEST(MeshOptimization, AnalyzeMeshHandlesEmptyMesh)
{
    auto mesh = Ref<MeshSource>::Create();
    auto analysis = MeshOptimization::AnalyzeMesh(*mesh);

    EXPECT_EQ(analysis.VertexCount, 0u);
    EXPECT_EQ(analysis.TriangleCount, 0u);
}

TEST(MeshOptimization, OptimizedMeshHasBetterCacheStats)
{
    auto mesh = MakeGridMesh(8);
    auto preAnalysis = MeshOptimization::AnalyzeMesh(*mesh);

    MeshOptimization::OptimizeMesh(*mesh);
    auto postAnalysis = MeshOptimization::AnalyzeMesh(*mesh);

    // After optimization, ACMR and ATVR should be equal or better
    EXPECT_LE(postAnalysis.ACMR, preAnalysis.ACMR + 0.01f); // Allow tiny tolerance
    EXPECT_LE(postAnalysis.ATVR, preAnalysis.ATVR + 0.01f);
}

// =============================================================================
// Meshlet Generation Tests
// =============================================================================

TEST(MeshOptimization, GenerateMeshletsProducesOutput)
{
    auto mesh = MakeGridMesh(8);
    MeshOptimization::OptimizeMesh(*mesh);

    auto meshlets = MeshOptimization::GenerateMeshlets(*mesh);

    EXPECT_FALSE(meshlets.Meshlets.empty());
    EXPECT_FALSE(meshlets.MeshletVertices.empty());
    EXPECT_FALSE(meshlets.MeshletTriangles.empty());
    EXPECT_EQ(meshlets.Bounds.size(), meshlets.Meshlets.size());
}

TEST(MeshOptimization, GenerateMeshletsRespectsLimits)
{
    auto mesh = MakeGridMesh(16);
    MeshOptimization::OptimizeMesh(*mesh);

    constexpr u32 maxVerts = 64;
    constexpr u32 maxTris = 124;
    auto meshlets = MeshOptimization::GenerateMeshlets(*mesh, maxVerts, maxTris);

    for (const auto& m : meshlets.Meshlets)
    {
        EXPECT_LE(m.VertexCount, maxVerts);
        EXPECT_LE(m.TriangleCount, maxTris);
    }
}

TEST(MeshOptimization, GenerateMeshletsHandlesEmptyMesh)
{
    auto mesh = Ref<MeshSource>::Create();
    auto meshlets = MeshOptimization::GenerateMeshlets(*mesh);

    EXPECT_TRUE(meshlets.Meshlets.empty());
}

TEST(MeshOptimization, MeshletBoundsHavePositiveRadius)
{
    auto mesh = MakeGridMesh(8);
    MeshOptimization::OptimizeMesh(*mesh);

    auto meshlets = MeshOptimization::GenerateMeshlets(*mesh);

    for (const auto& b : meshlets.Bounds)
    {
        EXPECT_GT(b.Radius, 0.0f);
    }
}

// =============================================================================
// Spatial Sort Tests
// =============================================================================

TEST(MeshOptimization, SpatialSortPreservesGeometry)
{
    auto mesh = MakeGridMesh(4);
    auto originalIdxCount = mesh->GetIndices().Num();
    auto originalVertCount = mesh->GetVertices().Num();

    MeshOptimization::SpatialSortTriangles(*mesh);

    EXPECT_EQ(mesh->GetIndices().Num(), originalIdxCount);
    EXPECT_EQ(mesh->GetVertices().Num(), originalVertCount);

    auto vertCount = static_cast<u32>(mesh->GetVertices().Num());
    for (i32 i = 0; i < mesh->GetIndices().Num(); ++i)
    {
        EXPECT_LT(mesh->GetIndices()[i], vertCount);
    }
}

TEST(MeshOptimization, SpatialSortHandlesEmptyMesh)
{
    auto mesh = Ref<MeshSource>::Create();
    MeshOptimization::SpatialSortTriangles(*mesh);
    EXPECT_TRUE(mesh->GetIndices().IsEmpty());
}

// =============================================================================
// Buffer Encoding Tests
// =============================================================================

TEST(MeshOptimization, EncodeDecodeVertexBufferRoundTrip)
{
    auto mesh = MakeGridMesh(4);
    const auto& verts = mesh->GetVertices();
    auto vertCount = static_cast<sizet>(verts.Num());

    auto encoded = MeshOptimization::EncodeVertexBuffer(verts.GetData(), vertCount, sizeof(Vertex));

    EXPECT_FALSE(encoded.Data.empty());
    EXPECT_LT(encoded.Data.size(), encoded.OriginalSize); // Should compress

    std::vector<Vertex> decoded(vertCount);
    bool ok = MeshOptimization::DecodeVertexBuffer(decoded.data(), vertCount, sizeof(Vertex), encoded);
    EXPECT_TRUE(ok);

    // Verify round-trip fidelity
    for (sizet i = 0; i < vertCount; ++i)
    {
        EXPECT_FLOAT_EQ(decoded[i].Position.x, verts[static_cast<i32>(i)].Position.x);
        EXPECT_FLOAT_EQ(decoded[i].Position.y, verts[static_cast<i32>(i)].Position.y);
        EXPECT_FLOAT_EQ(decoded[i].Position.z, verts[static_cast<i32>(i)].Position.z);
        EXPECT_FLOAT_EQ(decoded[i].Normal.x, verts[static_cast<i32>(i)].Normal.x);
        EXPECT_FLOAT_EQ(decoded[i].Normal.y, verts[static_cast<i32>(i)].Normal.y);
        EXPECT_FLOAT_EQ(decoded[i].Normal.z, verts[static_cast<i32>(i)].Normal.z);
        EXPECT_FLOAT_EQ(decoded[i].TexCoord.x, verts[static_cast<i32>(i)].TexCoord.x);
        EXPECT_FLOAT_EQ(decoded[i].TexCoord.y, verts[static_cast<i32>(i)].TexCoord.y);
    }
}

TEST(MeshOptimization, EncodeDecodeIndexBufferRoundTrip)
{
    auto mesh = MakeGridMesh(4);
    const auto& indices = mesh->GetIndices();
    auto indexCount = static_cast<sizet>(indices.Num());
    auto vertCount = static_cast<sizet>(mesh->GetVertices().Num());

    auto encoded = MeshOptimization::EncodeIndexBuffer(indices.GetData(), indexCount, vertCount);

    EXPECT_FALSE(encoded.Data.empty());
    EXPECT_LT(encoded.Data.size(), encoded.OriginalSize); // Should compress

    std::vector<u32> decoded(indexCount);
    bool ok = MeshOptimization::DecodeIndexBuffer(decoded.data(), indexCount, encoded);
    EXPECT_TRUE(ok);

    // Verify round-trip fidelity: codec may rotate vertices within a triangle
    // but preserves triangle content and winding order
    ASSERT_EQ(indexCount % 3, 0u);
    for (sizet i = 0; i < indexCount; i += 3)
    {
        std::array<u32, 3> orig = { indices[static_cast<i32>(i)], indices[static_cast<i32>(i + 1)], indices[static_cast<i32>(i + 2)] };
        std::array<u32, 3> dec = { decoded[i], decoded[i + 1], decoded[i + 2] };
        std::ranges::sort(orig);
        std::ranges::sort(dec);
        EXPECT_EQ(orig, dec) << "Triangle mismatch at index " << i;
    }
}

TEST(MeshOptimization, EncodeVertexBufferCompresses)
{
    auto mesh = MakeGridMesh(16); // Larger mesh for meaningful compression
    const auto& verts = mesh->GetVertices();
    auto vertCount = static_cast<sizet>(verts.Num());

    auto encoded = MeshOptimization::EncodeVertexBuffer(verts.GetData(), vertCount, sizeof(Vertex));

    // Encoded should be meaningfully smaller than raw data
    f32 ratio = static_cast<f32>(encoded.Data.size()) / static_cast<f32>(encoded.OriginalSize);
    EXPECT_LT(ratio, 0.95f); // At least 5% reduction
}

TEST(MeshOptimization, ShadowIndicesAreSpatialSorted)
{
    // A grid mesh has spatial structure that spatial sort can exploit.
    // Verify shadow indices are generated and contain valid data.
    auto mesh = MakeGridMesh(8);
    MeshOptimization::OptimizeMesh(*mesh);

    EXPECT_TRUE(mesh->HasShadowIndices());
    const auto& shadowIndices = mesh->GetShadowIndices();
    auto vertexCount = static_cast<u32>(mesh->GetVertices().Num());

    for (i32 i = 0; i < shadowIndices.Num(); ++i)
    {
        EXPECT_LT(shadowIndices[i], vertexCount) << "Invalid shadow index at " << i;
    }
}

TEST(MeshOptimization, AttributeAwareLODProducesValidOutput)
{
    // Verify attribute-aware LOD produces valid output
    // that preserves more attribute quality than basic simplification
    auto mesh = MakeWavyGridMesh(16);
    const auto& srcIndices = mesh->GetIndices();
    auto srcTriCount = srcIndices.Num() / 3;

    auto basicLOD = MeshOptimization::GenerateLODMesh(*mesh, 0.25f);
    auto attrLOD = MeshOptimization::GenerateLODMeshWithAttributes(*mesh, 0.25f);

    ASSERT_NE(basicLOD, nullptr);
    ASSERT_NE(attrLOD, nullptr);

    // Both should reduce triangle count
    EXPECT_LT(basicLOD->GetIndices().Num() / 3, srcTriCount);
    EXPECT_LT(attrLOD->GetIndices().Num() / 3, srcTriCount);

    // Both should produce valid indices
    auto attrVertCount = static_cast<u32>(attrLOD->GetVertices().Num());
    for (i32 i = 0; i < attrLOD->GetIndices().Num(); ++i)
    {
        EXPECT_LT(attrLOD->GetIndices()[i], attrVertCount);
    }

    // Compute average per-triangle UV span as an attribute-quality metric.
    // Lower span = triangles don't straddle UV seams = better quality.
    auto computeAvgTriUVSpan = [](const MeshSource& lod) -> f64
    {
        const auto& verts = lod.GetVertices();
        const auto& idxs = lod.GetIndices();
        auto triCount = idxs.Num() / 3;
        if (triCount == 0)
        {
            return 0.0;
        }

        f64 totalSpan = 0.0;
        for (i32 t = 0; t < triCount; ++t)
        {
            const auto& uv0 = verts[static_cast<i32>(idxs[t * 3 + 0])].TexCoord;
            const auto& uv1 = verts[static_cast<i32>(idxs[t * 3 + 1])].TexCoord;
            const auto& uv2 = verts[static_cast<i32>(idxs[t * 3 + 2])].TexCoord;
            auto maxU = std::max({ uv0.x, uv1.x, uv2.x });
            auto minU = std::min({ uv0.x, uv1.x, uv2.x });
            auto maxV = std::max({ uv0.y, uv1.y, uv2.y });
            auto minV = std::min({ uv0.y, uv1.y, uv2.y });
            totalSpan += static_cast<f64>(maxU - minU + maxV - minV);
        }
        return totalSpan / static_cast<f64>(triCount);
    };

    auto basicUVSpan = computeAvgTriUVSpan(*basicLOD);
    auto attrUVSpan = computeAvgTriUVSpan(*attrLOD);

    // Attribute-aware LOD should produce equal or smaller UV span per triangle
    // (it penalises collapsing edges that cross UV seams)
    EXPECT_LE(attrUVSpan, basicUVSpan + 1e-4);
}

// =============================================================================
// Degenerate-triangle census (issue #629)
//
// A UV-DEGENERATE triangle has zero area in texture space but real area in 3D.
// Its UV gradient is zero along at least one axis, so a tangent frame built from
// screen-space UV derivatives collapses to the zero vector and `normalize` returns
// NaN — which is exactly how Sponza's potted vines grew a white lacework through
// the G-Buffer (see docs/bug-investigations/nanite-foliage-white-fringe-*.md).
// Sponza ships 314 of them and the cluster-DAG simplifier creates more.
//
// The shader guards the collapse; these tests pin the IMPORT-side census that makes
// such an asset visible instead of silently expensive. Nothing is dropped: the
// triangles carry real 3D area (two whole Sponza submeshes are 100% UV-degenerate),
// so removing them would punch holes in the mesh.
// =============================================================================

// Three corners sharing one texcoord — the case the investigation found in Sponza
// (80 of the 314) and the one the shader probe reproduces.
TEST(MeshOptimization, DetectsTriangleWhoseCornersShareOneTexcoord)
{
    TArray<Vertex> vertices;
    vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.5f, 0.5f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.5f, 0.5f }));
    vertices.Add(Vertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.5f, 0.5f }));

    TArray<u32> indices;
    indices.Add(0);
    indices.Add(1);
    indices.Add(2);

    auto mesh = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
    const auto stats = MeshOptimization::AnalyzeDegenerateTriangles(*mesh);

    EXPECT_EQ(stats.TriangleCount, 1u);
    EXPECT_EQ(stats.ZeroUvAreaCount, 1u);
    EXPECT_EQ(stats.ZeroAreaCount, 0u); // it has REAL 3D area — that is why it cannot be dropped
    EXPECT_GT(stats.ZeroUvArea3DSum, 0.0);
    EXPECT_TRUE(stats.HasDegenerates());
}

// The majority case in Sponza (234 of the 314): three DISTINCT texcoords that are
// collinear in UV space (here, constant v). The UV area is still zero, so the
// tangent still collapses — a "do the corners share a UV?" check would miss these.
TEST(MeshOptimization, DetectsTriangleWhoseTexcoordsAreCollinearInUvSpace)
{
    TArray<Vertex> vertices;
    vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.10f, 0.78f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.40f, 0.78f }));
    vertices.Add(Vertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.70f, 0.78f }));

    TArray<u32> indices;
    indices.Add(0);
    indices.Add(1);
    indices.Add(2);

    auto mesh = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
    const auto stats = MeshOptimization::AnalyzeDegenerateTriangles(*mesh);

    EXPECT_EQ(stats.ZeroUvAreaCount, 1u);
    EXPECT_EQ(stats.ZeroAreaCount, 0u);
}

// A zero-3D-area triangle is a different animal: it is dead geometry (Assimp's
// aiProcess_FindDegenerates normally removes it) and must not be counted as a
// UV-degenerate — nothing shades it, so it costs no fragments.
TEST(MeshOptimization, ZeroAreaTriangleIsCountedSeparatelyFromUvDegenerates)
{
    TArray<Vertex> vertices;
    vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }));
    vertices.Add(Vertex({ 2.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f })); // collinear in 3D

    TArray<u32> indices;
    indices.Add(0);
    indices.Add(1);
    indices.Add(2);

    auto mesh = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
    const auto stats = MeshOptimization::AnalyzeDegenerateTriangles(*mesh);

    EXPECT_EQ(stats.TriangleCount, 1u);
    EXPECT_EQ(stats.ZeroAreaCount, 1u);
    EXPECT_EQ(stats.ZeroUvAreaCount, 0u);
    EXPECT_DOUBLE_EQ(stats.ZeroUvArea3DSum, 0.0);
}

// A healthy textured mesh must not be reported — the census is a warning channel,
// and a false positive on every quad would train everyone to ignore it.
TEST(MeshOptimization, HealthyMeshReportsNoDegenerates)
{
    auto mesh = MakeQuadMesh(); // two triangles, full 0..1 UV square
    const auto stats = MeshOptimization::AnalyzeDegenerateTriangles(*mesh);

    EXPECT_EQ(stats.TriangleCount, 2u);
    EXPECT_EQ(stats.ZeroUvAreaCount, 0u);
    EXPECT_EQ(stats.ZeroAreaCount, 0u);
    EXPECT_FALSE(stats.HasDegenerates());
}

// Mixed mesh: the census must count per triangle, not give up at the first one.
TEST(MeshOptimization, CountsDegeneratesAlongsideHealthyTrianglesInTheSameMesh)
{
    TArray<Vertex> vertices;
    // Healthy quad (2 tris)
    vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }));
    vertices.Add(Vertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }));
    // UV-degenerate triangle (real 3D area, one shared texcoord)
    vertices.Add(Vertex({ 2.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.25f, 0.25f }));
    vertices.Add(Vertex({ 3.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.25f, 0.25f }));
    vertices.Add(Vertex({ 2.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.25f, 0.25f }));

    TArray<u32> indices;
    for (u32 i : { 0u, 1u, 2u, 0u, 2u, 3u, 4u, 5u, 6u })
    {
        indices.Add(i);
    }

    auto mesh = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
    const auto stats = MeshOptimization::AnalyzeDegenerateTriangles(*mesh);

    EXPECT_EQ(stats.TriangleCount, 3u);
    EXPECT_EQ(stats.ZeroUvAreaCount, 1u);
    EXPECT_EQ(stats.ZeroAreaCount, 0u);
    EXPECT_NEAR(stats.ZeroUvArea3DSum, 0.5, 1e-6); // the 1x1 right triangle
}

// The census is pure analysis: OptimizeMesh reports it but must not delete anything.
// This is the "do not silently delete real geometry" contract.
TEST(MeshOptimization, OptimizeMeshKeepsUvDegenerateTriangles)
{
    TArray<Vertex> vertices;
    vertices.Add(Vertex({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.25f, 0.25f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.25f, 0.25f }));
    vertices.Add(Vertex({ 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.25f, 0.25f }));
    vertices.Add(Vertex({ 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f }));
    vertices.Add(Vertex({ 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f }));
    vertices.Add(Vertex({ 0.0f, 1.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f }));

    TArray<u32> indices;
    for (u32 i = 0; i < 6; ++i)
    {
        indices.Add(i);
    }

    auto mesh = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
    Submesh sub;
    sub.m_BaseVertex = 0;
    sub.m_BaseIndex = 0;
    sub.m_VertexCount = 6;
    sub.m_IndexCount = 6;
    mesh->AddSubmesh(sub);

    const u32 before = MeshOptimization::AnalyzeDegenerateTriangles(*mesh).ZeroUvAreaCount;
    ASSERT_EQ(before, 1u);

    MeshOptimization::OptimizeMesh(*mesh);

    EXPECT_EQ(mesh->GetIndices().Num(), 6); // no triangle removed
    EXPECT_EQ(MeshOptimization::AnalyzeDegenerateTriangles(*mesh).ZeroUvAreaCount, 1u);
}

// =============================================================================
// BuildAutoLODChain Tests (issue #711)
//
// The CPU half of automatic LOD generation. These assert the properties the
// pixel-error selector depends on and that nothing else would report: the chain
// really halves, its errors really accumulate, and — the one most likely to be
// silently wrong — the error is a RATIO, so it does not change when the same
// model is authored at a different scale.
// =============================================================================

// Returns the same wavy grid scaled uniformly in model space. Normals stay unit
// length and UVs are untouched, so the only difference is world size.
static Ref<MeshSource> MakeScaledWavyGridMesh(u32 gridSize, f32 scale)
{
    auto source = MakeWavyGridMesh(gridSize);
    auto& vertices = source->GetVertices();
    for (i32 i = 0; i < vertices.Num(); ++i)
    {
        vertices[i].Position *= scale;
    }
    return source;
}

TEST(MeshOptimization, AutoLODChainProducesMultipleLevels)
{
    auto mesh = MakeWavyGridMesh(64); // 8192 triangles
    const auto chain = MeshOptimization::BuildAutoLODChain(*mesh);

    ASSERT_GE(chain.size(), 4u) << "a dense wavy grid must support several halvings";
    EXPECT_FALSE(chain[0].Source) << "entry 0 is the source mesh itself";
    EXPECT_EQ(chain[0].TriangleCount, static_cast<u32>(mesh->GetIndices().Num() / 3));
    EXPECT_FLOAT_EQ(chain[0].Error, 0.0f);

    for (sizet i = 1; i < chain.size(); ++i)
    {
        ASSERT_TRUE(chain[i].Source) << "level " << i;
        EXPECT_GT(chain[i].TriangleCount, 0u) << "level " << i;
        EXPECT_EQ(chain[i].Source->GetIndices().Num() / 3, static_cast<i32>(chain[i].TriangleCount))
            << "level " << i << " triangle count must match the emitted geometry";
    }
}

TEST(MeshOptimization, AutoLODChainRoughlyHalvesTrianglesPerLevel)
{
    auto mesh = MakeWavyGridMesh(64);
    const auto chain = MeshOptimization::BuildAutoLODChain(*mesh);
    ASSERT_GE(chain.size(), 3u);

    for (sizet i = 1; i < chain.size(); ++i)
    {
        const u32 previous = chain[i - 1].TriangleCount;
        const u32 current = chain[i].TriangleCount;
        EXPECT_LT(current, previous) << "level " << i << " must be strictly coarser than its parent";
        // The simplifier can stop short of the target on topology grounds; it can
        // never overshoot it, so only the upper bound is a hard contract. The lower
        // bound is generous for the same reason.
        EXPECT_LE(current, previous / 2 + 1) << "level " << i << " must reach the halving target";
    }
}

TEST(MeshOptimization, AutoLODChainErrorsStrictlyIncrease)
{
    auto mesh = MakeWavyGridMesh(64);
    const auto chain = MeshOptimization::BuildAutoLODChain(*mesh);
    ASSERT_GE(chain.size(), 3u);

    for (sizet i = 1; i < chain.size(); ++i)
    {
        EXPECT_GT(chain[i].Error, chain[i - 1].Error)
            << "level " << i << " error must exceed its parent, or the selector can never prefer the finer one";
        EXPECT_TRUE(std::isfinite(chain[i].Error)) << "level " << i;
    }

    // Accumulated error is a fraction of the model extent; a chain that ends past
    // 1.0 has levels that no longer resemble the source at all.
    EXPECT_LT(chain.back().Error, 1.0f);
}

// The consistency property the design turns on. Error is relative to the model
// extent AND the normal weight is scaled by the NORMALIZED average vertex
// distance, so authoring the same model in centimetres rather than metres must
// not change a single level. Using a raw (unnormalized) vertex distance for the
// normal weight — the easy mistake — scales the weight with the model and
// produces a visibly different chain here.
TEST(MeshOptimization, AutoLODChainIsInvariantToModelScale)
{
    auto small = MakeScaledWavyGridMesh(48, 1.0f);
    auto large = MakeScaledWavyGridMesh(48, 100.0f);

    const auto smallChain = MeshOptimization::BuildAutoLODChain(*small);
    const auto largeChain = MeshOptimization::BuildAutoLODChain(*large);

    ASSERT_GE(smallChain.size(), 3u);
    ASSERT_EQ(smallChain.size(), largeChain.size()) << "a 100x scale change must not change the chain length";

    // Tolerances, and which half of this test actually discriminates.
    //
    // The ERROR assertion is the one with teeth. A normal weight built on a RAW
    // model-space vertex distance instead of the normalized one scales by 100 here,
    // and that moves the accumulated error by 41-72% at the deeper levels — far
    // outside the band below. (See NormalWeightMustBeNormalizedByModelExtent, which
    // measures that gap rather than assuming it.)
    //
    // The TRIANGLE-COUNT assertion is only a sanity check, and deliberately loose:
    // with `target_error = FLT_MAX` the simplifier collapses until it hits the
    // triangle target, so the counts are near-invariant under ANY weighting. It
    // catches a chain that ended early or ran away, nothing subtler.
    //
    // Neither is asserted bit-exactly, because the two runs genuinely are not: f32
    // positions 100x apart round differently, that flips the occasional quadric tie,
    // and a one-triangle difference at level 1 propagates down a nine-level chain.
    // Measured drift is ~0.1% on counts and a few percent on error.
    constexpr f32 kTriangleTolerance = 0.02f; // 2%
    constexpr f32 kErrorTolerance = 0.10f;    // 10%

    for (sizet i = 0; i < smallChain.size(); ++i)
    {
        const auto smallTris = static_cast<f32>(smallChain[i].TriangleCount);
        const auto largeTris = static_cast<f32>(largeChain[i].TriangleCount);
        EXPECT_NEAR(largeTris, smallTris, std::max(smallTris * kTriangleTolerance, 1.0f)) << "level " << i;
        EXPECT_NEAR(largeChain[i].Error, smallChain[i].Error,
                    std::max(smallChain[i].Error * kErrorTolerance, 1e-6f))
            << "level " << i << " error must be scale-invariant";
    }
}

// Gives the tolerances above their teeth. The normal weight handed to meshopt is
// `NormalImportance * (averageVertexDistance / modelExtent)`; dropping the division
// is the natural mistake, and on a 100x model it multiplies the weight by 100. This
// simulates that by scaling NormalImportance instead.
//
// Two assertions, because neither alone is enough:
//
//   * the CHOSEN GEOMETRY must differ. This is the one that proves the quadric
//     weighting itself changed. It cannot be read off the triangle counts: with
//     `target_error = FLT_MAX` the simplifier collapses until it hits the triangle
//     target, so the counts come out the same (measured: within one triangle at
//     every level) no matter what the weights are. Weights decide WHICH edges
//     collapse, not how many.
//   * the STORED ERROR must land outside the 10% band the scale-invariance test
//     tolerates, since that is the number selection actually consumes. Measured gap
//     on this mesh: 41-72% at the deeper levels. Part of that comes from the
//     `stepError / (1 + normalWeight)` renormalisation rather than from the
//     simplification, which is exactly why the geometry assertion above is here too.
TEST(MeshOptimization, NormalWeightMustBeNormalizedByModelExtent)
{
    auto mesh = MakeScaledWavyGridMesh(48, 1.0f);

    const auto baseline = MeshOptimization::BuildAutoLODChain(*mesh);
    ASSERT_GE(baseline.size(), 3u);

    MeshOptimization::AutoLODSettings unnormalized;
    unnormalized.NormalImportance *= 100.0f; // what the missing division would produce
    const auto skewed = MeshOptimization::BuildAutoLODChain(*mesh, unnormalized);
    ASSERT_GE(skewed.size(), 3u);

    const sizet common = std::min(baseline.size(), skewed.size());

    bool geometryDiffers = false;
    for (sizet i = 1; i < common && !geometryDiffers; ++i)
    {
        ASSERT_TRUE(baseline[i].Source);
        ASSERT_TRUE(skewed[i].Source);
        const auto& a = baseline[i].Source->GetIndices();
        const auto& b = skewed[i].Source->GetIndices();
        geometryDiffers = (a.Num() != b.Num()) || !std::equal(a.GetData(), a.GetData() + a.Num(), b.GetData());
    }
    EXPECT_TRUE(geometryDiffers)
        << "a 100x normal weight produced byte-identical index buffers at every level — the weight "
           "is not reaching the simplifier's quadric at all";

    const sizet deepest = common - 1;
    const f32 baseError = baseline[deepest].Error;
    ASSERT_GT(baseError, 0.0f);
    const f32 relativeGap = std::abs(skewed[deepest].Error - baseError) / baseError;
    EXPECT_GT(relativeGap, 0.10f)
        << "a 100x normal weight moved the accumulated error by only " << (relativeGap * 100.0f)
        << "% at level " << deepest
        << " — inside the band AutoLODChainIsInvariantToModelScale accepts, so that test could not "
           "tell a normalized weight from an unnormalized one";
}

TEST(MeshOptimization, AutoLODChainHonoursMaxLevels)
{
    auto mesh = MakeWavyGridMesh(64);

    MeshOptimization::AutoLODSettings settings;
    settings.MaxLevels = 3;
    const auto chain = MeshOptimization::BuildAutoLODChain(*mesh, settings);

    EXPECT_LE(chain.size(), 3u);
    EXPECT_GE(chain.size(), 2u) << "3 levels are reachable on this mesh, so the cap must be the binding limit";
}

TEST(MeshOptimization, AutoLODChainStopsAtTheTriangleFloor)
{
    auto mesh = MakeWavyGridMesh(64);

    MeshOptimization::AutoLODSettings settings;
    settings.MinTriangleCount = 1024;
    settings.MaxLevels = 32;
    settings.MaxStepError = 1.0f; // take the floor out of the error condition's way
    const auto chain = MeshOptimization::BuildAutoLODChain(*mesh, settings);

    ASSERT_GE(chain.size(), 2u);
    // The floor bounds where the chain STOPS, not each level: the last level is
    // allowed to land below it, but no level may be generated from a parent that
    // was already at or under the floor.
    EXPECT_GT(chain[chain.size() - 2].TriangleCount, settings.MinTriangleCount)
        << "the chain kept simplifying a level that was already at the floor";
}

TEST(MeshOptimization, AutoLODChainStopsWhenTheStepErrorGetsTooLarge)
{
    auto mesh = MakeWavyGridMesh(64);

    MeshOptimization::AutoLODSettings tight;
    tight.MaxStepError = 1e-5f;
    const auto tightChain = MeshOptimization::BuildAutoLODChain(*mesh, tight);

    MeshOptimization::AutoLODSettings loose;
    loose.MaxStepError = 0.5f;
    const auto looseChain = MeshOptimization::BuildAutoLODChain(*mesh, loose);

    EXPECT_LT(tightChain.size(), looseChain.size())
        << "a near-zero error budget must cut the chain shorter than the default one";
}

TEST(MeshOptimization, AutoLODChainRejectsUnsimplifiableInput)
{
    // A quad has 2 triangles — below any useful floor, so nothing is generated.
    auto quad = MakeQuadMesh();
    const auto quadChain = MeshOptimization::BuildAutoLODChain(*quad);
    ASSERT_EQ(quadChain.size(), 1u);
    EXPECT_EQ(quadChain[0].TriangleCount, 2u);

    // An empty source yields no chain at all.
    TArray<Vertex> noVertices;
    TArray<u32> noIndices;
    auto empty = Ref<MeshSource>::Create(MoveTemp(noVertices), MoveTemp(noIndices));
    EXPECT_TRUE(MeshOptimization::BuildAutoLODChain(*empty).empty());
}

TEST(MeshOptimization, AutoLODChainSanitizesNonFiniteSettings)
{
    auto mesh = MakeWavyGridMesh(32);

    MeshOptimization::AutoLODSettings broken;
    broken.MaxStepError = std::numeric_limits<f32>::quiet_NaN();
    broken.NormalImportance = -1.0f;
    broken.TexCoordWeight = std::numeric_limits<f32>::infinity();

    const auto chain = MeshOptimization::BuildAutoLODChain(*mesh, broken);
    ASSERT_GE(chain.size(), 2u) << "bad settings must fall back to the defaults, not disable generation";
    for (const auto& entry : chain)
    {
        EXPECT_TRUE(std::isfinite(entry.Error));
        EXPECT_GE(entry.Error, 0.0f);
    }
}

// Each generated level carries only the vertices its own indices reference. On a
// deep chain the alternative — copying the full source vertex buffer into every
// level — multiplies a mesh's vertex memory by the level count.
TEST(MeshOptimization, AutoLODChainCompactsEachLevelsVertexBuffer)
{
    auto mesh = MakeWavyGridMesh(64);
    const auto chain = MeshOptimization::BuildAutoLODChain(*mesh);
    ASSERT_GE(chain.size(), 3u);

    const i32 sourceVertexCount = mesh->GetVertices().Num();
    for (sizet i = 1; i < chain.size(); ++i)
    {
        EXPECT_LT(chain[i].Source->GetVertices().Num(), sourceVertexCount)
            << "level " << i << " still carries the full source vertex buffer";
        // Every index must address the compacted array.
        const auto& indices = chain[i].Source->GetIndices();
        const i32 vertexCount = chain[i].Source->GetVertices().Num();
        for (i32 k = 0; k < indices.Num(); ++k)
        {
            ASSERT_LT(static_cast<i32>(indices[k]), vertexCount) << "level " << i << " index " << k;
        }
    }
}

TEST(MeshOptimization, MeasureModelExtentMatchesTheBoundingBox)
{
    auto mesh = MakeScaledWavyGridMesh(16, 3.0f);
    // The grid spans [0,1] in x and z before scaling, and less than that in y,
    // so the largest side is the scaled 1.0.
    EXPECT_NEAR(MeshOptimization::MeasureModelExtent(*mesh), 3.0f, 1e-3f);

    TArray<Vertex> noVertices;
    TArray<u32> noIndices;
    auto empty = Ref<MeshSource>::Create(MoveTemp(noVertices), MoveTemp(noIndices));
    EXPECT_FLOAT_EQ(MeshOptimization::MeasureModelExtent(*empty), 0.0f);
}
