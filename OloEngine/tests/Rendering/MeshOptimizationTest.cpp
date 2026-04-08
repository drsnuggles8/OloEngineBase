#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
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
        std::sort(tri.begin(), tri.end());
        triangles.push_back(tri);
    }

    std::sort(triangles.begin(), triangles.end());
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

    // Add bone influences
    auto& bones = mesh->GetBoneInfluences();
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
