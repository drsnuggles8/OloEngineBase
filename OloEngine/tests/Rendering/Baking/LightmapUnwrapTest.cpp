// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Baking/LightmapUnwrap.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Ref.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstring>
#include <limits>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// LightmapUnwrap — xatlas-based UV2 parameterization (issue #439)
//
// Headless, no GL: MeshSource is used CPU-side only (Build() is never called;
// LightmapUnwrap must not need a context). Geometry is built in code — a
// 24-vertex/36-index cube whose 6 faces carry distinct normals, so xatlas
// charts each face separately and the seam-split / xref-rebuild contract is
// exercised end to end.
//
// Pinned contracts: in-place rebuild preserves Position/Normal/TexCoord
// bit-exactly via xref, indices stay GLOBAL into the rebuilt vertex array,
// per-submesh Base/Count ranges stay valid, the UV2 stream is normalized to
// [0,1] and parallel to the vertices, failure leaves the source untouched,
// and identical inputs produce bit-identical outputs.
// =============================================================================

namespace
{
    struct MeshData
    {
        TArray<Vertex> Vertices;
        TArray<u32> Indices;
    };

    // Appends one quad (two CCW triangles) whose normal is cross(uAxis, vAxis).
    void AppendFace(MeshData& mesh, const glm::vec3& origin, const glm::vec3& uAxis, const glm::vec3& vAxis)
    {
        const glm::vec3 normal = glm::normalize(glm::cross(uAxis, vAxis));
        const u32 base = static_cast<u32>(mesh.Vertices.Num());
        mesh.Vertices.Add(Vertex(origin, normal, glm::vec2(0.0f, 0.0f)));
        mesh.Vertices.Add(Vertex(origin + uAxis, normal, glm::vec2(1.0f, 0.0f)));
        mesh.Vertices.Add(Vertex(origin + uAxis + vAxis, normal, glm::vec2(1.0f, 1.0f)));
        mesh.Vertices.Add(Vertex(origin + vAxis, normal, glm::vec2(0.0f, 1.0f)));
        mesh.Indices.Add(base + 0);
        mesh.Indices.Add(base + 1);
        mesh.Indices.Add(base + 2);
        mesh.Indices.Add(base + 0);
        mesh.Indices.Add(base + 2);
        mesh.Indices.Add(base + 3);
    }

    // Axis-aligned cube: 24 vertices (4 per face, per-face normals), 36 indices,
    // outward winding. Face order: -Z, +Z, -X, +X, -Y, +Y.
    MeshData MakeCube(const glm::vec3& center, f32 half)
    {
        MeshData mesh;
        const f32 s = 2.0f * half;
        const glm::vec3 c = center;
        AppendFace(mesh, c + glm::vec3(half, -half, -half), glm::vec3(-s, 0, 0), glm::vec3(0, s, 0)); // -Z
        AppendFace(mesh, c + glm::vec3(-half, -half, half), glm::vec3(s, 0, 0), glm::vec3(0, s, 0));  // +Z
        AppendFace(mesh, c + glm::vec3(-half, -half, -half), glm::vec3(0, 0, s), glm::vec3(0, s, 0)); // -X
        AppendFace(mesh, c + glm::vec3(half, -half, half), glm::vec3(0, 0, -s), glm::vec3(0, s, 0));  // +X
        AppendFace(mesh, c + glm::vec3(-half, -half, -half), glm::vec3(s, 0, 0), glm::vec3(0, 0, s)); // -Y
        AppendFace(mesh, c + glm::vec3(-half, half, half), glm::vec3(s, 0, 0), glm::vec3(0, 0, -s));  // +Y
        return mesh;
    }

    Submesh MakeSubmesh(u32 baseVertex, u32 baseIndex, u32 vertexCount, u32 indexCount)
    {
        Submesh sub;
        sub.m_BaseVertex = baseVertex;
        sub.m_BaseIndex = baseIndex;
        sub.m_VertexCount = vertexCount;
        sub.m_IndexCount = indexCount;
        return sub;
    }

    // Single-submesh cube MeshSource (the common case). Heap via Ref: Asset deletes
    // copy AND move, so MeshSource cannot be returned by value.
    Ref<MeshSource> MakeCubeSource()
    {
        const MeshData cube = MakeCube(glm::vec3(0.0f), 0.5f);
        auto source = Ref<MeshSource>::Create(cube.Vertices, cube.Indices);
        source->AddSubmesh(MakeSubmesh(0, 0, static_cast<u32>(cube.Vertices.Num()),
                                       static_cast<u32>(cube.Indices.Num())));
        return source;
    }

    // Bit-exact comparison — deliberately memcmp, not float ==: the xref contract is
    // "copy of the original", so the bits must match, including -0.0f vs 0.0f.
    bool SameVertexBits(const Vertex& a, const Vertex& b)
    {
        return std::memcmp(&a.Position, &b.Position, sizeof(glm::vec3)) == 0 &&
               std::memcmp(&a.Normal, &b.Normal, sizeof(glm::vec3)) == 0 &&
               std::memcmp(&a.TexCoord, &b.TexCoord, sizeof(glm::vec2)) == 0;
    }

    f32 TriangleUVArea(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c)
    {
        const glm::vec2 e1 = b - a;
        const glm::vec2 e2 = c - a;
        return 0.5f * std::abs(e1.x * e2.y - e1.y * e2.x);
    }

    // Classifies a unit-ish axis normal into a face id 0..5 (-Z,+Z,-X,+X,-Y,+Y), -1 otherwise.
    i32 FaceIdFromNormal(const glm::vec3& n)
    {
        constexpr f32 kAxisThreshold = 0.9f;
        if (n.z < -kAxisThreshold)
            return 0;
        if (n.z > kAxisThreshold)
            return 1;
        if (n.x < -kAxisThreshold)
            return 2;
        if (n.x > kAxisThreshold)
            return 3;
        if (n.y < -kAxisThreshold)
            return 4;
        if (n.y > kAxisThreshold)
            return 5;
        return -1;
    }

    struct UVBounds
    {
        glm::vec2 Min{ std::numeric_limits<f32>::max() };
        glm::vec2 Max{ std::numeric_limits<f32>::lowest() };
        bool Touched = false;

        void Extend(const glm::vec2& uv)
        {
            Min = glm::min(Min, uv);
            Max = glm::max(Max, uv);
            Touched = true;
        }
    };
} // namespace

TEST(LightmapUnwrapTest, GeneratesCubeParameterization)
{
    Ref<MeshSource> source = MakeCubeSource();
    const TArray<Vertex> originalVertices = source->GetVertices(); // copy for xref check
    const i32 originalVertexCount = originalVertices.Num();

    ASSERT_TRUE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));

    EXPECT_TRUE(source->HasLightmapUVs());
    // Index count unchanged: still 12 triangles.
    ASSERT_EQ(source->GetIndices().Num(), 36);
    // Seam splits only ever ADD vertices on a fully-referenced mesh.
    EXPECT_GE(source->GetVertices().Num(), originalVertexCount);
    // UV2 stream is parallel to the vertices.
    ASSERT_EQ(source->GetLightmapUVs().Num(), source->GetVertices().Num());

    // Every UV2 is finite and normalized to [0,1].
    const auto& uvs = source->GetLightmapUVs();
    for (i32 i = 0; i < uvs.Num(); ++i)
    {
        ASSERT_TRUE(std::isfinite(uvs[i].x)) << "UV2.x not finite at " << i;
        ASSERT_TRUE(std::isfinite(uvs[i].y)) << "UV2.y not finite at " << i;
        EXPECT_GE(uvs[i].x, 0.0f) << "UV2.x below 0 at " << i;
        EXPECT_LE(uvs[i].x, 1.0f) << "UV2.x above 1 at " << i;
        EXPECT_GE(uvs[i].y, 0.0f) << "UV2.y below 0 at " << i;
        EXPECT_LE(uvs[i].y, 1.0f) << "UV2.y above 1 at " << i;
    }

    // xref preservation: every rebuilt vertex is a bit-exact copy of SOME original.
    const auto& rebuilt = source->GetVertices();
    for (i32 i = 0; i < rebuilt.Num(); ++i)
    {
        bool found = false;
        for (i32 j = 0; j < originalVertices.Num() && !found; ++j)
        {
            found = SameVertexBits(rebuilt[i], originalVertices[j]);
        }
        EXPECT_TRUE(found) << "rebuilt vertex " << i << " matches no original vertex bit-exactly";
    }

    // Every rebuilt index is in range and inside the (single) submesh's vertex window.
    ASSERT_EQ(source->GetSubmeshes().Num(), 1);
    const Submesh& sub = source->GetSubmeshes()[0];
    EXPECT_EQ(sub.m_IndexCount, 36u);
    EXPECT_EQ(sub.m_VertexCount, static_cast<u32>(source->GetVertices().Num()));
    const auto& indices = source->GetIndices();
    for (i32 i = 0; i < indices.Num(); ++i)
    {
        ASSERT_LT(indices[i], static_cast<u32>(source->GetVertices().Num()));
        EXPECT_GE(indices[i], sub.m_BaseVertex);
        EXPECT_LT(indices[i], sub.m_BaseVertex + sub.m_VertexCount);
    }
}

TEST(LightmapUnwrapTest, EveryTriangleHasRealUVArea)
{
    Ref<MeshSource> source = MakeCubeSource();
    ASSERT_TRUE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));

    const auto& indices = source->GetIndices();
    const auto& uvs = source->GetLightmapUVs();
    ASSERT_EQ(indices.Num() % 3, 0);

    f64 totalArea = 0.0;
    for (i32 t = 0; t < indices.Num(); t += 3)
    {
        const glm::vec2& a = uvs[static_cast<i32>(indices[t + 0])];
        const glm::vec2& b = uvs[static_cast<i32>(indices[t + 1])];
        const glm::vec2& c = uvs[static_cast<i32>(indices[t + 2])];
        const f32 area = TriangleUVArea(a, b, c);
        ASSERT_TRUE(std::isfinite(area)) << "NaN/inf UV area for triangle " << t / 3;
        EXPECT_GT(area, 1.0e-9f) << "zero UV area for triangle " << t / 3 << " — chart collapsed";
        totalArea += static_cast<f64>(area);
    }
    EXPECT_GT(totalArea, 0.0);
}

TEST(LightmapUnwrapTest, ChartsAreSeparatedInUVSpace)
{
    Ref<MeshSource> source = MakeCubeSource();
    const LightmapUnwrapOptions options{}; // Resolution 512, Padding 4
    ASSERT_TRUE(LightmapUnwrap::Generate(*source, options));

    const auto& vertices = source->GetVertices();
    const auto& indices = source->GetIndices();
    const auto& uvs = source->GetLightmapUVs();

    // Total chart area fits the atlas and is not degenerate.
    f64 totalArea = 0.0;
    for (i32 t = 0; t < indices.Num(); t += 3)
    {
        totalArea += static_cast<f64>(TriangleUVArea(uvs[static_cast<i32>(indices[t + 0])],
                                                     uvs[static_cast<i32>(indices[t + 1])],
                                                     uvs[static_cast<i32>(indices[t + 2])]));
    }
    EXPECT_LE(totalArea, 1.0 + 1.0e-4);
    EXPECT_GE(totalArea, 0.05);

    // Per-face UV bounding boxes via the (bit-preserved) normals — face identity does
    // not depend on triangle order surviving the rebuild. Opposite cube faces are
    // distinct charts, so their boxes (shrunk by ~1 texel for the border) must not overlap.
    UVBounds faceBounds[6];
    for (i32 t = 0; t < indices.Num(); t += 3)
    {
        const i32 face = FaceIdFromNormal(vertices[static_cast<i32>(indices[t])].Normal);
        ASSERT_GE(face, 0) << "triangle " << t / 3 << " has a non-axis normal";
        for (i32 k = 0; k < 3; ++k)
        {
            faceBounds[face].Extend(uvs[static_cast<i32>(indices[t + k])]);
        }
    }

    const f32 shrink = 1.0f / 512.0f; // ~1 texel at the pack resolution
    const i32 oppositePairs[3][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 } };
    for (const auto& pair : oppositePairs)
    {
        const UVBounds& a = faceBounds[pair[0]];
        const UVBounds& b = faceBounds[pair[1]];
        ASSERT_TRUE(a.Touched);
        ASSERT_TRUE(b.Touched);
        const bool overlap = (a.Min.x + shrink) < (b.Max.x - shrink) &&
                             (b.Min.x + shrink) < (a.Max.x - shrink) &&
                             (a.Min.y + shrink) < (b.Max.y - shrink) &&
                             (b.Min.y + shrink) < (a.Max.y - shrink);
        EXPECT_FALSE(overlap) << "opposite faces " << pair[0] << " and " << pair[1]
                              << " overlap in UV space — charts were not separated";
    }
}

TEST(LightmapUnwrapTest, SecondGenerateIsIdempotent)
{
    Ref<MeshSource> source = MakeCubeSource();
    ASSERT_TRUE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));

    const i32 vertexCountAfterFirst = source->GetVertices().Num();
    const TArray<glm::vec2> uvsAfterFirst = source->GetLightmapUVs(); // copy
    const TArray<u32> indicesAfterFirst = source->GetIndices();       // copy

    ASSERT_TRUE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));

    EXPECT_EQ(source->GetVertices().Num(), vertexCountAfterFirst);
    ASSERT_EQ(source->GetLightmapUVs().Num(), uvsAfterFirst.Num());
    EXPECT_EQ(0, std::memcmp(source->GetLightmapUVs().GetData(), uvsAfterFirst.GetData(),
                             sizeof(glm::vec2) * static_cast<sizet>(uvsAfterFirst.Num())));
    ASSERT_EQ(source->GetIndices().Num(), indicesAfterFirst.Num());
    EXPECT_EQ(0, std::memcmp(source->GetIndices().GetData(), indicesAfterFirst.GetData(),
                             sizeof(u32) * static_cast<sizet>(indicesAfterFirst.Num())));
}

TEST(LightmapUnwrapTest, RefusesBoneInfluencedMeshUntouched)
{
    Ref<MeshSource> source = MakeCubeSource();

    // Fake real skinning data: one influence with a nonzero weight.
    BoneInfluence influence;
    influence.SetBoneData(0, 0, 1.0f);
    source->SetVertexBoneData(0, influence);
    ASSERT_TRUE(source->HasBoneInfluences());

    const i32 vertexCountBefore = source->GetVertices().Num();
    const TArray<u32> indicesBefore = source->GetIndices(); // copy

    EXPECT_FALSE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));

    // Completely untouched.
    EXPECT_FALSE(source->HasLightmapUVs());
    EXPECT_EQ(source->GetVertices().Num(), vertexCountBefore);
    ASSERT_EQ(source->GetIndices().Num(), indicesBefore.Num());
    EXPECT_EQ(0, std::memcmp(source->GetIndices().GetData(), indicesBefore.GetData(),
                             sizeof(u32) * static_cast<sizet>(indicesBefore.Num())));
    EXPECT_EQ(source->GetSubmeshes().Num(), 1);
}

TEST(LightmapUnwrapTest, RefusesEmptyMesh)
{
    auto source = Ref<MeshSource>::Create();
    EXPECT_FALSE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));
    EXPECT_FALSE(source->HasLightmapUVs());
}

TEST(LightmapUnwrapTest, IdenticalInputsProduceBitIdenticalOutput)
{
    Ref<MeshSource> a = MakeCubeSource();
    Ref<MeshSource> b = MakeCubeSource();

    ASSERT_TRUE(LightmapUnwrap::Generate(*a, LightmapUnwrapOptions{}));
    ASSERT_TRUE(LightmapUnwrap::Generate(*b, LightmapUnwrapOptions{}));

    ASSERT_EQ(a->GetVertices().Num(), b->GetVertices().Num());
    ASSERT_EQ(a->GetIndices().Num(), b->GetIndices().Num());
    ASSERT_EQ(a->GetLightmapUVs().Num(), b->GetLightmapUVs().Num());

    EXPECT_EQ(0, std::memcmp(a->GetVertices().GetData(), b->GetVertices().GetData(),
                             sizeof(Vertex) * static_cast<sizet>(a->GetVertices().Num())));
    EXPECT_EQ(0, std::memcmp(a->GetIndices().GetData(), b->GetIndices().GetData(),
                             sizeof(u32) * static_cast<sizet>(a->GetIndices().Num())));
    EXPECT_EQ(0, std::memcmp(a->GetLightmapUVs().GetData(), b->GetLightmapUVs().GetData(),
                             sizeof(glm::vec2) * static_cast<sizet>(a->GetLightmapUVs().Num())));
}

TEST(LightmapUnwrapTest, MultiSubmeshRangesStayValid)
{
    // Two cubes concatenated into one MeshSource: indices are GLOBAL (second cube's
    // indices start at 24), one submesh per cube — the same layout the model importer
    // produces and the renderer draws via BaseIndex/IndexCount off the shared buffers.
    const MeshData cubeA = MakeCube(glm::vec3(0.0f), 0.5f);
    const MeshData cubeB = MakeCube(glm::vec3(2.0f, 0.0f, 0.0f), 0.5f);

    TArray<Vertex> vertices = cubeA.Vertices;
    TArray<u32> indices = cubeA.Indices;
    const u32 baseVertexB = static_cast<u32>(vertices.Num());
    const u32 baseIndexB = static_cast<u32>(indices.Num());
    for (i32 i = 0; i < cubeB.Vertices.Num(); ++i)
    {
        vertices.Add(cubeB.Vertices[i]);
    }
    for (i32 i = 0; i < cubeB.Indices.Num(); ++i)
    {
        indices.Add(cubeB.Indices[i] + baseVertexB);
    }

    auto source = Ref<MeshSource>::Create(vertices, indices);
    source->AddSubmesh(MakeSubmesh(0, 0, static_cast<u32>(cubeA.Vertices.Num()),
                                   static_cast<u32>(cubeA.Indices.Num())));
    source->AddSubmesh(MakeSubmesh(baseVertexB, baseIndexB, static_cast<u32>(cubeB.Vertices.Num()),
                                   static_cast<u32>(cubeB.Indices.Num())));

    ASSERT_TRUE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));

    ASSERT_EQ(source->GetSubmeshes().Num(), 2);
    ASSERT_EQ(source->GetIndices().Num(), 72);
    ASSERT_EQ(source->GetLightmapUVs().Num(), source->GetVertices().Num());

    const auto& newIndices = source->GetIndices();
    u32 coveredIndices = 0;
    u32 coveredVertices = 0;
    for (i32 s = 0; s < source->GetSubmeshes().Num(); ++s)
    {
        const Submesh& sub = source->GetSubmeshes()[s];
        EXPECT_EQ(sub.m_IndexCount, 36u) << "submesh " << s << " lost triangles";
        ASSERT_LE(sub.m_BaseIndex + sub.m_IndexCount, static_cast<u32>(newIndices.Num()));
        ASSERT_LE(sub.m_BaseVertex + sub.m_VertexCount, static_cast<u32>(source->GetVertices().Num()));

        // Every index inside the submesh's range references its declared vertex window —
        // the invariant AnimatedModel's submesh extraction and the draw path rely on.
        for (u32 i = sub.m_BaseIndex; i < sub.m_BaseIndex + sub.m_IndexCount; ++i)
        {
            const u32 v = newIndices[static_cast<i32>(i)];
            EXPECT_GE(v, sub.m_BaseVertex) << "submesh " << s << " index " << i << " below BaseVertex";
            EXPECT_LT(v, sub.m_BaseVertex + sub.m_VertexCount) << "submesh " << s << " index " << i << " past vertex range";
        }
        coveredIndices += sub.m_IndexCount;
        coveredVertices += sub.m_VertexCount;
    }
    EXPECT_EQ(coveredIndices, static_cast<u32>(newIndices.Num()));
    EXPECT_EQ(coveredVertices, static_cast<u32>(source->GetVertices().Num()));
}

TEST(LightmapUnwrapTest, ShadowIndicesAreRegeneratedNotStale)
{
    Ref<MeshSource> source = MakeCubeSource();
    MeshOptimization::GenerateShadowIndices(*source);
    ASSERT_TRUE(source->HasShadowIndices());

    ASSERT_TRUE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));

    // Regenerated against the rebuilt arrays: right count, every entry a valid NEW vertex.
    ASSERT_TRUE(source->HasShadowIndices());
    ASSERT_EQ(source->GetShadowIndices().Num(), source->GetIndices().Num());
    const u32 vertexCount = static_cast<u32>(source->GetVertices().Num());
    const auto& shadowIndices = source->GetShadowIndices();
    for (i32 i = 0; i < shadowIndices.Num(); ++i)
    {
        EXPECT_LT(shadowIndices[i], vertexCount) << "stale shadow index at " << i;
    }
}

TEST(LightmapUnwrapTest, NoSubmeshMeshUnwrapsAsWholeMeshRange)
{
    const MeshData cube = MakeCube(glm::vec3(0.0f), 0.5f);
    auto source = Ref<MeshSource>::Create(cube.Vertices, cube.Indices); // no submeshes at all

    ASSERT_TRUE(LightmapUnwrap::Generate(*source, LightmapUnwrapOptions{}));

    EXPECT_TRUE(source->HasLightmapUVs());
    EXPECT_EQ(source->GetSubmeshes().Num(), 0); // still no submeshes
    ASSERT_EQ(source->GetIndices().Num(), 36);
    ASSERT_EQ(source->GetLightmapUVs().Num(), source->GetVertices().Num());
    const u32 vertexCount = static_cast<u32>(source->GetVertices().Num());
    for (i32 i = 0; i < source->GetIndices().Num(); ++i)
    {
        ASSERT_LT(source->GetIndices()[i], vertexCount);
    }
}
