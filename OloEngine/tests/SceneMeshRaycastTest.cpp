#include "OloEnginePCH.h"

// =============================================================================
// SceneMeshRaycastTest — unit test (headless, no GL).
//
// Pins OloEngine::SceneMeshRaycaster, the world-space closest-hit raycast over
// a scene's mesh entities that backs the instance scatter brush's mesh-surface
// placement (docs/GPU_INSTANCING_FUTURE_IMPROVEMENTS.md §1.2). The local-space
// BVH itself is pinned by MeshBVHRaycastTest; what's under test here is the
// scene layer on top:
//   - the world↔local transform dance (translate / rotate / non-uniform scale,
//     normal via inverse-transpose) against geometric ground truth,
//   - closest-hit selection across multiple entities and component types,
//   - submesh index-range isolation (a SubmeshComponent must only be hittable
//     through its own triangle range),
//   - TMax clamping (how EditorLayer resolves terrain-vs-mesh precedence),
//   - BVH cache reuse, geometry-fingerprint invalidation, and dead-source
//     pruning.
//
// MeshSources are built from raw vertex/index arrays — no MeshSource::Build(),
// no render context.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneMeshRaycast.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <limits>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    constexpr f32 kTol = 1e-4f;

    Vertex MakeVertex(const glm::vec3& position)
    {
        // Normal/UV are irrelevant to the raycast (only Position is read), but
        // the Vertex ctor wants them.
        return Vertex(position, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec2(0.0f));
    }

    // Append an axis-aligned cube (8 corners, 12 outward-wound triangles)
    // centred at `centre` with half-extent `half` to the given arrays.
    void AppendCube(TArray<Vertex>& vertices, TArray<u32>& indices, const glm::vec3& centre, f32 half)
    {
        const auto base = static_cast<u32>(vertices.Num());
        const glm::vec3 corners[8] = {
            { -half, -half, -half },
            { half, -half, -half },
            { half, half, -half },
            { -half, half, -half },
            { -half, -half, half },
            { half, -half, half },
            { half, half, half },
            { -half, half, half },
        };
        for (const auto& c : corners)
            vertices.Add(MakeVertex(centre + c));

        const u32 faces[36] = {
            0,
            2,
            1,
            0,
            3,
            2, // back   (-Z)
            4,
            5,
            6,
            4,
            6,
            7, // front  (+Z)
            0,
            4,
            7,
            0,
            7,
            3, // left   (-X)
            1,
            2,
            6,
            1,
            6,
            5, // right  (+X)
            0,
            1,
            5,
            0,
            5,
            4, // bottom (-Y)
            3,
            7,
            6,
            3,
            6,
            2, // top    (+Y)
        };
        for (u32 idx : faces)
            indices.Add(base + idx);
    }

    // Submesh entry covering an index range; the raycaster only reads
    // m_BaseIndex / m_IndexCount.
    Submesh MakeSubmesh(u32 baseIndex, u32 indexCount, u32 baseVertex, u32 vertexCount)
    {
        Submesh submesh;
        submesh.m_BaseIndex = baseIndex;
        submesh.m_IndexCount = indexCount;
        submesh.m_BaseVertex = baseVertex;
        submesh.m_VertexCount = vertexCount;
        return submesh;
    }

    // A unit cube MeshSource centred at the local origin, with one submesh
    // spanning the whole geometry (mirrors what the importer produces).
    Ref<MeshSource> MakeCubeSource(f32 half = 1.0f)
    {
        TArray<Vertex> vertices;
        TArray<u32> indices;
        AppendCube(vertices, indices, glm::vec3(0.0f), half);
        auto source = Ref<MeshSource>::Create(vertices, indices);
        source->AddSubmesh(MakeSubmesh(0, static_cast<u32>(indices.Num()), 0, static_cast<u32>(vertices.Num())));
        return source;
    }

    // A single-triangle MeshSource (arbitrary vertices), one full-range submesh.
    Ref<MeshSource> MakeTriangleSource(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2)
    {
        TArray<Vertex> vertices;
        vertices.Add(MakeVertex(v0));
        vertices.Add(MakeVertex(v1));
        vertices.Add(MakeVertex(v2));
        TArray<u32> indices;
        indices.Add(0);
        indices.Add(1);
        indices.Add(2);
        auto source = Ref<MeshSource>::Create(vertices, indices);
        source->AddSubmesh(MakeSubmesh(0, 3, 0, 3));
        return source;
    }

    Entity MakeMeshEntity(Scene& scene, const Ref<MeshSource>& source, const std::string& name)
    {
        Entity entity = scene.CreateEntity(name);
        entity.AddComponent<MeshComponent>(source);
        return entity;
    }
} // namespace

// ---- World-space hit mapping ------------------------------------------------

TEST(SceneMeshRaycastTest, HitsTranslatedCube)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    Entity cube = MakeMeshEntity(*scene, MakeCubeSource(), "Cube");
    cube.GetComponent<TransformComponent>().Translation = { 10.0f, 0.0f, 0.0f };

    const Ray ray(glm::vec3(10.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    SceneMeshRayHit hit;
    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));

    EXPECT_NEAR(hit.Distance, 9.0f, kTol);
    EXPECT_NEAR(hit.Point.x, 10.0f, kTol);
    EXPECT_NEAR(hit.Point.y, 0.0f, kTol);
    EXPECT_NEAR(hit.Point.z, 1.0f, kTol);
    // +Z face normal, oriented back toward the ray origin.
    EXPECT_NEAR(hit.Normal.x, 0.0f, kTol);
    EXPECT_NEAR(hit.Normal.y, 0.0f, kTol);
    EXPECT_NEAR(hit.Normal.z, 1.0f, kTol);
    EXPECT_EQ(hit.HitEntity, cube);
}

TEST(SceneMeshRaycastTest, MissReturnsFalseAndResetsHit)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    MakeMeshEntity(*scene, MakeCubeSource(), "Cube");

    // Fire away from the cube; pre-poison outHit to prove it gets reset.
    const Ray ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    SceneMeshRayHit hit;
    hit.Hit = true;
    hit.Distance = 123.0f;
    EXPECT_FALSE(raycaster.CastRay(*scene, ray, hit));
    EXPECT_FALSE(hit.Hit);
    EXPECT_FALSE(hit.HitEntity);
}

TEST(SceneMeshRaycastTest, RotatedEntityMapsNormalBack)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    // Quad in the local XY plane (geometric normal ±Z), rotated -90° about X:
    // the quad now lies in the world XZ plane, so a downward ray must report
    // an up-facing world normal.
    TArray<Vertex> vertices;
    vertices.Add(MakeVertex({ -1.0f, -1.0f, 0.0f }));
    vertices.Add(MakeVertex({ 1.0f, -1.0f, 0.0f }));
    vertices.Add(MakeVertex({ 1.0f, 1.0f, 0.0f }));
    vertices.Add(MakeVertex({ -1.0f, 1.0f, 0.0f }));
    TArray<u32> indices;
    for (u32 idx : { 0u, 1u, 2u, 0u, 2u, 3u })
        indices.Add(idx);
    auto source = Ref<MeshSource>::Create(vertices, indices);
    source->AddSubmesh(MakeSubmesh(0, 6, 0, 4));

    Entity quad = MakeMeshEntity(*scene, source, "Quad");
    quad.GetComponent<TransformComponent>().SetRotationEuler({ -glm::half_pi<f32>(), 0.0f, 0.0f });

    const Ray ray(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    SceneMeshRayHit hit;
    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));

    EXPECT_NEAR(hit.Distance, 5.0f, kTol);
    EXPECT_NEAR(hit.Normal.x, 0.0f, kTol);
    EXPECT_NEAR(hit.Normal.y, 1.0f, kTol);
    EXPECT_NEAR(hit.Normal.z, 0.0f, kTol);
}

TEST(SceneMeshRaycastTest, NonUniformScaleNormalMatchesGeometricGroundTruth)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    // Slanted triangle under a non-uniform scale. Ground truth is computed
    // from the *transformed* vertex positions (cross of the world-space
    // edges), independent of the inverse-transpose path under test — a plain
    // rotation of the local normal would NOT stay perpendicular here.
    const glm::vec3 v0(0.0f, 0.0f, 0.0f);
    const glm::vec3 v1(2.0f, 0.0f, 0.0f);
    const glm::vec3 v2(0.0f, 2.0f, 2.0f);
    Entity tri = MakeMeshEntity(*scene, MakeTriangleSource(v0, v1, v2), "Slant");
    const glm::vec3 scale(1.0f, 1.0f, 3.0f);
    tri.GetComponent<TransformComponent>().Scale = scale;

    glm::vec3 expectedNormal = glm::normalize(glm::cross(scale * v1 - scale * v0, scale * v2 - scale * v0));

    // Aim at the transformed triangle's interior from the side the expected
    // normal faces; flip the expectation to oppose the ray like the contract
    // demands.
    const glm::vec3 target = scale * ((v0 + v1 + v2) / 3.0f);
    const glm::vec3 origin = target + expectedNormal * 10.0f;
    const glm::vec3 dir = -expectedNormal;

    SceneMeshRayHit hit;
    ASSERT_TRUE(raycaster.CastRay(*scene, Ray(origin, dir), hit));

    EXPECT_NEAR(hit.Distance, 10.0f, 1e-3f);
    EXPECT_NEAR(hit.Normal.x, expectedNormal.x, kTol);
    EXPECT_NEAR(hit.Normal.y, expectedNormal.y, kTol);
    EXPECT_NEAR(hit.Normal.z, expectedNormal.z, kTol);
}

// ---- Closest-hit selection ----------------------------------------------------

TEST(SceneMeshRaycastTest, ClosestOfTwoEntitiesWins)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    Entity nearCube = MakeMeshEntity(*scene, MakeCubeSource(), "Near");
    nearCube.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
    Entity farCube = MakeMeshEntity(*scene, MakeCubeSource(), "Far");
    farCube.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, -10.0f };

    const Ray ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    SceneMeshRayHit hit;
    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));

    EXPECT_EQ(hit.HitEntity, nearCube);
    EXPECT_NEAR(hit.Distance, 9.0f, kTol);
}

TEST(SceneMeshRaycastTest, TMaxClampRejectsHitsBeyondIt)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    MakeMeshEntity(*scene, MakeCubeSource(), "Cube"); // front face 9 units away

    // This is exactly how EditorLayer resolves terrain-vs-mesh precedence: the
    // terrain hit distance becomes TMax, so only a mesh in front can win.
    const glm::vec3 origin(0.0f, 0.0f, 10.0f);
    const glm::vec3 dir(0.0f, 0.0f, -1.0f);

    SceneMeshRayHit hit;
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(origin, dir, 0.0f, 5.0f), hit)); // "terrain" closer
    EXPECT_TRUE(raycaster.CastRay(*scene, Ray(origin, dir, 0.0f, 50.0f), hit)); // mesh closer
    EXPECT_NEAR(hit.Distance, 9.0f, kTol);
}

// ---- Component-type coverage ----------------------------------------------------

TEST(SceneMeshRaycastTest, SubmeshComponentOnlyHitsItsOwnRange)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    // One MeshSource holding two cubes as two submeshes: cube 0 at the local
    // origin, cube 1 at local (10, 0, 0).
    TArray<Vertex> vertices;
    TArray<u32> indices;
    AppendCube(vertices, indices, glm::vec3(0.0f), 1.0f);
    AppendCube(vertices, indices, glm::vec3(10.0f, 0.0f, 0.0f), 1.0f);
    auto source = Ref<MeshSource>::Create(vertices, indices);
    source->AddSubmesh(MakeSubmesh(0, 36, 0, 8));
    source->AddSubmesh(MakeSubmesh(36, 36, 8, 8));

    // The entity references ONLY submesh 1 (the offset cube).
    Entity entity = scene->CreateEntity("SubmeshOnly");
    entity.AddComponent<SubmeshComponent>(Ref<Mesh>::Create(source, 1));

    const glm::vec3 down(0.0f, -1.0f, 0.0f);
    SceneMeshRayHit hit;

    // Above cube 0's geometry — not part of the referenced submesh: miss.
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(glm::vec3(0.0f, 5.0f, 0.0f), down), hit));

    // Above cube 1: hit on its top face.
    ASSERT_TRUE(raycaster.CastRay(*scene, Ray(glm::vec3(10.0f, 5.0f, 0.0f), down), hit));
    EXPECT_NEAR(hit.Distance, 4.0f, kTol);
    EXPECT_EQ(hit.HitEntity, entity);
}

TEST(SceneMeshRaycastTest, InvisibleSubmeshComponentIsSkipped)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    auto source = MakeCubeSource();
    Entity entity = scene->CreateEntity("Hidden");
    auto& submeshComp = entity.AddComponent<SubmeshComponent>(Ref<Mesh>::Create(source, 0));
    submeshComp.m_Visible = false;

    SceneMeshRayHit hit;
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)), hit));
}

TEST(SceneMeshRaycastTest, SkeletonEntityIsSkipped)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    // GPU-skinned geometry must not be raycast at bind pose — mirror the
    // static render path's SkeletonComponent skip.
    Entity entity = MakeMeshEntity(*scene, MakeCubeSource(), "Skinned");
    entity.AddComponent<SkeletonComponent>();

    SceneMeshRayHit hit;
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)), hit));
}

// ---- Cache behaviour ----------------------------------------------------

TEST(SceneMeshRaycastTest, CacheReusesOneEntryPerMeshRange)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    MakeMeshEntity(*scene, MakeCubeSource(), "Cube");

    const Ray ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    SceneMeshRayHit hit;
    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));
    EXPECT_EQ(raycaster.GetCacheEntryCount(), 1u);
    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));
    EXPECT_EQ(raycaster.GetCacheEntryCount(), 1u);
}

TEST(SceneMeshRaycastTest, CacheRebuildsWhenGeometryFingerprintChanges)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    auto source = MakeCubeSource(); // front face at local z = +1
    MakeMeshEntity(*scene, MakeCubeSource(0.5f), "Decoy");
    Entity cube = MakeMeshEntity(*scene, source, "Cube");
    cube.GetComponent<TransformComponent>().Translation = { 50.0f, 0.0f, 0.0f };

    const Ray ray(glm::vec3(50.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    SceneMeshRayHit hit;
    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));
    EXPECT_NEAR(hit.Distance, 9.0f, kTol);

    // Grow the cube's geometry in place: append a triangle 2 units in front of
    // the +Z face, widen the submesh range, and refresh the bounds — the
    // size/pointer fingerprint must invalidate the cached BVH so the next cast
    // sees the new triangle.
    auto& vertices = source->GetVertices();
    auto& indices = source->GetIndices();
    const auto base = static_cast<u32>(vertices.Num());
    vertices.Add(MakeVertex({ -2.0f, -2.0f, 3.0f }));
    vertices.Add(MakeVertex({ 2.0f, -2.0f, 3.0f }));
    vertices.Add(MakeVertex({ 0.0f, 2.0f, 3.0f }));
    indices.Add(base);
    indices.Add(base + 1);
    indices.Add(base + 2);
    TArray<Submesh> submeshes;
    submeshes.Add(MakeSubmesh(0, static_cast<u32>(indices.Num()), 0, static_cast<u32>(vertices.Num())));
    source->SetSubmeshes(submeshes); // also recalculates source bounds

    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));
    EXPECT_NEAR(hit.Distance, 7.0f, kTol); // new triangle at z = +3
}

TEST(SceneMeshRaycastTest, CachePrunesDeadMeshSources)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    const Ray ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    SceneMeshRayHit hit;

    {
        Entity cube = MakeMeshEntity(*scene, MakeCubeSource(), "Doomed");
        ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));
        EXPECT_EQ(raycaster.GetCacheEntryCount(), 1u);
        scene->DestroyEntity(cube); // drops the component's MeshSource ref
    }

    // Next cast prunes the dead entry before tracing.
    EXPECT_FALSE(raycaster.CastRay(*scene, ray, hit));
    EXPECT_EQ(raycaster.GetCacheEntryCount(), 0u);
}

TEST(SceneMeshRaycastTest, ClearCacheDropsEverything)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    MakeMeshEntity(*scene, MakeCubeSource(), "Cube");

    const Ray ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    SceneMeshRayHit hit;
    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));
    EXPECT_EQ(raycaster.GetCacheEntryCount(), 1u);

    raycaster.ClearCache();
    EXPECT_EQ(raycaster.GetCacheEntryCount(), 0u);

    // Still functional after the wipe (rebuilds on demand).
    ASSERT_TRUE(raycaster.CastRay(*scene, ray, hit));
    EXPECT_EQ(raycaster.GetCacheEntryCount(), 1u);
}

// ---- Ray sanitization at the API boundary ----------------------------------

TEST(SceneMeshRaycastTest, NonUnitDirectionStillReportsWorldDistances)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    MakeMeshEntity(*scene, MakeCubeSource(), "Cube"); // front face 9 units away

    // CastRay normalizes internally: a 5x-length direction must yield the same
    // world-space Distance/Point, and TMin/TMax stay world units throughout.
    const Ray scaledRay(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -5.0f));
    SceneMeshRayHit hit;
    ASSERT_TRUE(raycaster.CastRay(*scene, scaledRay, hit));
    EXPECT_NEAR(hit.Distance, 9.0f, kTol);
    EXPECT_NEAR(hit.Point.z, 1.0f, kTol);

    // The world-unit TMax contract holds for non-unit directions too.
    EXPECT_FALSE(raycaster.CastRay(
        *scene, Ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -5.0f), 0.0f, 5.0f), hit));
}

TEST(SceneMeshRaycastTest, DegenerateRaysMiss)
{
    auto scene = Ref<Scene>::Create();
    SceneMeshRaycaster raycaster;

    MakeMeshEntity(*scene, MakeCubeSource(), "Cube");

    const glm::vec3 origin(0.0f, 0.0f, 10.0f);
    const glm::vec3 toward(0.0f, 0.0f, -1.0f);
    constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

    SceneMeshRayHit hit;
    // Zero-length direction.
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(origin, glm::vec3(0.0f)), hit));
    // Non-finite direction / origin.
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(origin, glm::vec3(0.0f, kNaN, -1.0f)), hit));
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(glm::vec3(kNaN), toward), hit));
    // NaN or swapped [TMin, TMax] bounds.
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(origin, toward, kNaN, 100.0f), hit));
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(origin, toward, 0.0f, kNaN), hit));
    EXPECT_FALSE(raycaster.CastRay(*scene, Ray(origin, toward, 50.0f, 1.0f), hit));

    // A well-formed ray still works afterwards.
    EXPECT_TRUE(raycaster.CastRay(*scene, Ray(origin, toward), hit));
}
