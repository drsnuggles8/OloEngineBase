#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Ray.h"
#include "OloEngine/Renderer/BoundingVolumeHierarchy.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Containers/Array.h"

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Mesh BVH ray-vs-mesh queries
//
// This pins the contract for BoundingVolumeHierarchy and the Ray.h intersection
// helpers that back it (Möller–Trumbore ray/triangle + Kay–Kajiya ray/AABB slab).
// Pure CPU/glm math, no GL context — see docs/design/GPU_INSTANCING_FUTURE_IMPROVEMENTS.md
// §1.2 for why this accelerator exists (scatter-brush mesh raycast, gizmo
// snapping, click-to-select).
//
// The capstone is a brute-force parity sweep: for a cube and a UV sphere, every
// ray's BVH closest-hit must match an O(n) reference that tests every triangle.
// That proves the accelerator and its pruning never disagree with the ground
// truth.
// =============================================================================

namespace
{
    constexpr f32 kTol = 1e-4f;

    // ---- Geometry generators (no GL: we never call MeshSource::Build) ----------

    struct MeshData
    {
        TArray<Vertex> Vertices;
        TArray<u32> Indices;
    };

    Vertex MakeVertex(const glm::vec3& position)
    {
        // Normal/UV are irrelevant to the BVH (it reads only Position), but the
        // Vertex ctor wants them.
        return Vertex(position, glm::normalize(position + glm::vec3(0.0f, 1e-3f, 0.0f)), glm::vec2(0.0f));
    }

    // Axis-aligned cube centered at the origin with half-extent `half`,
    // 8 shared corners and 12 outward-wound triangles.
    MeshData MakeCube(f32 half)
    {
        MeshData mesh;
        const glm::vec3 corners[8] = {
            { -half, -half, -half }, // 0
            { half, -half, -half },  // 1
            { half, half, -half },   // 2
            { -half, half, -half },  // 3
            { -half, -half, half },  // 4
            { half, -half, half },   // 5
            { half, half, half },    // 6
            { -half, half, half },   // 7
        };
        for (const auto& c : corners)
            mesh.Vertices.Add(MakeVertex(c));

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
            mesh.Indices.Add(idx);

        return mesh;
    }

    // UV (lat/long) sphere centered at the origin. `stacks`/`slices` control
    // triangle density — enough to force a multi-level BVH.
    MeshData MakeUVSphere(f32 radius, u32 stacks, u32 slices)
    {
        MeshData mesh;

        constexpr f32 pi = 3.14159265358979323846f;
        for (u32 stack = 0; stack <= stacks; ++stack)
        {
            const f32 phi = pi * static_cast<f32>(stack) / static_cast<f32>(stacks); // 0..pi
            const f32 y = std::cos(phi);
            const f32 r = std::sin(phi);
            for (u32 slice = 0; slice <= slices; ++slice)
            {
                const f32 theta = 2.0f * pi * static_cast<f32>(slice) / static_cast<f32>(slices);
                const glm::vec3 p(radius * r * std::cos(theta), radius * y, radius * r * std::sin(theta));
                mesh.Vertices.Add(MakeVertex(p));
            }
        }

        const u32 ring = slices + 1;
        for (u32 stack = 0; stack < stacks; ++stack)
        {
            for (u32 slice = 0; slice < slices; ++slice)
            {
                const u32 a = stack * ring + slice;
                const u32 b = a + ring;
                mesh.Indices.Add(a);
                mesh.Indices.Add(b);
                mesh.Indices.Add(a + 1);
                mesh.Indices.Add(a + 1);
                mesh.Indices.Add(b);
                mesh.Indices.Add(b + 1);
            }
        }

        return mesh;
    }

    // O(n) reference: the strictly-nearest triangle hit. The `t < closest` guard
    // (RayTriangle already clips to t <= closest via clipped.TMax) keeps an
    // equal-distance later triangle from displacing the current winner. The BVH
    // may break an exact-distance tie differently — it visits triangles in
    // traversal order, not index order — but that only changes which triangle
    // index wins; the distance and hit point are identical, and the parity
    // assertions compare exactly those, never the index.
    RayHit BruteForceClosest(const MeshData& mesh, const Ray& ray)
    {
        RayHit best;
        f32 closest = ray.TMax;
        const sizet triCount = static_cast<sizet>(mesh.Indices.Num()) / 3;
        for (sizet tri = 0; tri < triCount; ++tri)
        {
            const glm::vec3& p0 = mesh.Vertices[mesh.Indices[static_cast<i32>(tri * 3 + 0)]].Position;
            const glm::vec3& p1 = mesh.Vertices[mesh.Indices[static_cast<i32>(tri * 3 + 1)]].Position;
            const glm::vec3& p2 = mesh.Vertices[mesh.Indices[static_cast<i32>(tri * 3 + 2)]].Position;

            Ray clipped = ray;
            clipped.TMax = closest;

            f32 t = 0.0f;
            f32 u = 0.0f;
            f32 v = 0.0f;
            bool front = false;
            if (RayIntersect::RayTriangle(clipped, p0, p1, p2, t, u, v, front) && t < closest)
            {
                closest = t;
                best.Hit = true;
                best.Distance = t;
                best.TriangleIndex = static_cast<u32>(tri);
                best.Point = ray.At(t);
            }
        }
        return best;
    }

    // Deterministic Fibonacci-sphere directions — generic (never axis-aligned),
    // so the slab test's 1/dir reciprocals stay finite.
    std::vector<glm::vec3> FibonacciSphere(u32 count, f32 radius)
    {
        std::vector<glm::vec3> points;
        points.reserve(count);
        constexpr f32 goldenAngle = 2.39996322972865332f;
        for (u32 k = 0; k < count; ++k)
        {
            const f32 y = 1.0f - 2.0f * (static_cast<f32>(k) + 0.5f) / static_cast<f32>(count);
            const f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const f32 theta = goldenAngle * static_cast<f32>(k);
            points.emplace_back(radius * r * std::cos(theta), radius * y, radius * r * std::sin(theta));
        }
        return points;
    }
} // namespace

// -----------------------------------------------------------------------------
// Ray–triangle intersection (Möller–Trumbore)
// -----------------------------------------------------------------------------

TEST(MeshBVHRayTriangle, HitsThroughInteriorWithCorrectBarycentrics)
{
    // Right triangle in the z=0 plane: v0 origin, along +X and +Y.
    const glm::vec3 v0(0.0f, 0.0f, 0.0f);
    const glm::vec3 v1(1.0f, 0.0f, 0.0f);
    const glm::vec3 v2(0.0f, 1.0f, 0.0f);

    // Straight down from above the point (0.25, 0.25): P = v0 + u*(v1-v0) + v*(v2-v0)
    // = (u, v, 0), so u=0.25, v=0.25.
    Ray ray(glm::vec3(0.25f, 0.25f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f));

    f32 t = 0.0f;
    f32 u = 0.0f;
    f32 v = 0.0f;
    bool front = false;
    ASSERT_TRUE(RayIntersect::RayTriangle(ray, v0, v1, v2, t, u, v, front));
    EXPECT_NEAR(t, 1.0f, kTol);
    EXPECT_NEAR(u, 0.25f, kTol);
    EXPECT_NEAR(v, 0.25f, kTol);
    // Geometric normal cross(edge1,edge2) = +Z; ray travels -Z, so it strikes the front.
    EXPECT_TRUE(front);
}

TEST(MeshBVHRayTriangle, MissesOutsideTheTriangle)
{
    const glm::vec3 v0(0.0f, 0.0f, 0.0f);
    const glm::vec3 v1(1.0f, 0.0f, 0.0f);
    const glm::vec3 v2(0.0f, 1.0f, 0.0f);

    // (0.9, 0.9) is in the plane but outside the triangle (u+v = 1.8 > 1).
    Ray ray(glm::vec3(0.9f, 0.9f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f));

    f32 t, u, v;
    bool front;
    EXPECT_FALSE(RayIntersect::RayTriangle(ray, v0, v1, v2, t, u, v, front));
}

TEST(MeshBVHRayTriangle, ParallelRayMisses)
{
    const glm::vec3 v0(0.0f, 0.0f, 0.0f);
    const glm::vec3 v1(1.0f, 0.0f, 0.0f);
    const glm::vec3 v2(0.0f, 1.0f, 0.0f);

    // Ray travels within the z=0 plane: parallel to the triangle, |det| ~ 0.
    Ray ray(glm::vec3(-1.0f, 0.25f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    f32 t, u, v;
    bool front;
    EXPECT_FALSE(RayIntersect::RayTriangle(ray, v0, v1, v2, t, u, v, front));
}

TEST(MeshBVHRayTriangle, BackFaceReportsFrontFaceFalse)
{
    const glm::vec3 v0(0.0f, 0.0f, 0.0f);
    const glm::vec3 v1(1.0f, 0.0f, 0.0f);
    const glm::vec3 v2(0.0f, 1.0f, 0.0f);

    // Approach the same triangle from below, travelling +Z: hits the back face.
    Ray ray(glm::vec3(0.25f, 0.25f, -1.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    f32 t = 0.0f;
    f32 u, v;
    bool front = true;
    ASSERT_TRUE(RayIntersect::RayTriangle(ray, v0, v1, v2, t, u, v, front));
    EXPECT_NEAR(t, 1.0f, kTol);
    EXPECT_FALSE(front);
}

TEST(MeshBVHRayTriangle, RespectsTMinAndTMax)
{
    const glm::vec3 v0(0.0f, 0.0f, 0.0f);
    const glm::vec3 v1(1.0f, 0.0f, 0.0f);
    const glm::vec3 v2(0.0f, 1.0f, 0.0f);

    f32 t, u, v;
    bool front;

    // Hit is at t=1, but TMin clips it away.
    Ray tooNear(glm::vec3(0.25f, 0.25f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f), /*TMin*/ 2.0f);
    EXPECT_FALSE(RayIntersect::RayTriangle(tooNear, v0, v1, v2, t, u, v, front));

    // ... and TMax clips it the other way.
    Ray tooFar(glm::vec3(0.25f, 0.25f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f), /*TMin*/ 0.0f, /*TMax*/ 0.5f);
    EXPECT_FALSE(RayIntersect::RayTriangle(tooFar, v0, v1, v2, t, u, v, front));
}

TEST(MeshBVHRayTriangle, TriangleBehindOriginMisses)
{
    const glm::vec3 v0(0.0f, 0.0f, 0.0f);
    const glm::vec3 v1(1.0f, 0.0f, 0.0f);
    const glm::vec3 v2(0.0f, 1.0f, 0.0f);

    // Travelling -Z from below the plane: the triangle is behind (t < 0).
    Ray ray(glm::vec3(0.25f, 0.25f, -1.0f), glm::vec3(0.0f, 0.0f, -1.0f));

    f32 t, u, v;
    bool front;
    EXPECT_FALSE(RayIntersect::RayTriangle(ray, v0, v1, v2, t, u, v, front));
}

// -----------------------------------------------------------------------------
// Ray–AABB slab test
// -----------------------------------------------------------------------------

TEST(MeshBVHRayAABB, HitsBoxAndReportsEntryDistance)
{
    const glm::vec3 boxMin(-1.0f);
    const glm::vec3 boxMax(1.0f);
    const glm::vec3 origin(5.0f, 5.0f, 5.0f);
    const glm::vec3 dir = glm::normalize(glm::vec3(-1.0f, -1.0f, -1.0f));
    const glm::vec3 invDir = 1.0f / dir;

    f32 tNear = -1.0f;
    ASSERT_TRUE(RayIntersect::RayAABB(origin, invDir, boxMin, boxMax, 0.0f, 1e30f, tNear));
    // Enters when the leading axis crosses +1: Δ=4 along a unit-(-1,-1,-1)/√3 dir → 4√3.
    EXPECT_NEAR(tNear, 4.0f * std::sqrt(3.0f), 1e-3f);
}

TEST(MeshBVHRayAABB, MissesWhenAxisIntervalsDoNotOverlap)
{
    const glm::vec3 boxMin(-1.0f);
    const glm::vec3 boxMax(1.0f);
    // The x-slab is entered at positive t while the y-slab is only entered at
    // negative t — no common interval, so the ray misses the box.
    const glm::vec3 origin(5.0f, -5.0f, 5.0f);
    const glm::vec3 dir = glm::normalize(glm::vec3(-1.0f, -1.0f, -1.0f));
    const glm::vec3 invDir = 1.0f / dir;

    f32 tNear = 0.0f;
    EXPECT_FALSE(RayIntersect::RayAABB(origin, invDir, boxMin, boxMax, 0.0f, 1e30f, tNear));
}

TEST(MeshBVHRayAABB, OriginInsideBoxHitsAtTMin)
{
    const glm::vec3 boxMin(-1.0f);
    const glm::vec3 boxMax(1.0f);
    const glm::vec3 origin(0.0f, 0.0f, 0.0f);
    const glm::vec3 dir = glm::normalize(glm::vec3(1.0f, 0.3f, 0.2f));
    const glm::vec3 invDir = 1.0f / dir;

    f32 tNear = -1.0f;
    ASSERT_TRUE(RayIntersect::RayAABB(origin, invDir, boxMin, boxMax, 0.0f, 1e30f, tNear));
    EXPECT_NEAR(tNear, 0.0f, kTol); // entry distance clamped to TMin for an inside origin
}

TEST(MeshBVHRayAABB, TMaxPrunesAFarBox)
{
    const glm::vec3 boxMin(-1.0f);
    const glm::vec3 boxMax(1.0f);
    const glm::vec3 origin(5.0f, 5.0f, 5.0f);
    const glm::vec3 dir = glm::normalize(glm::vec3(-1.0f, -1.0f, -1.0f));
    const glm::vec3 invDir = 1.0f / dir;

    // Box entry is ~6.93; a TMax of 1.0 must reject it (the BVH uses this to skip
    // subtrees beyond the current closest hit).
    f32 tNear = 0.0f;
    EXPECT_FALSE(RayIntersect::RayAABB(origin, invDir, boxMin, boxMax, 0.0f, 1.0f, tNear));
}

// Regression: the parity sweeps use FibonacciSphere, which deliberately avoids
// axis-aligned directions, so the slab test's invDir == +/-inf path — and the
// 0*inf == NaN trap that fires when the origin sits exactly on a slab plane —
// would otherwise be untested. These pin the explicit axis-parallel handling.

TEST(MeshBVHRayAABB, AxisParallelRayOnSlabBoundaryHitsAndStaysFinite)
{
    const glm::vec3 boxMin(-1.0f);
    const glm::vec3 boxMax(1.0f);
    // -Z ray whose X origin lies exactly on the +X face (x == 1). dir = (0,0,-1)
    // gives invDir = (+inf, +inf, -1): the X term is the 0*inf == NaN trap. With
    // inclusive bounds the grazing ray is a hit — but the naive form returned a
    // spurious miss here because NaN poisoned the X slab's far distance to -inf.
    const glm::vec3 origin(1.0f, 0.0f, 5.0f);
    const glm::vec3 dir(0.0f, 0.0f, -1.0f);
    const glm::vec3 invDir = 1.0f / dir;

    f32 tNear = -1.0f;
    ASSERT_TRUE(RayIntersect::RayAABB(origin, invDir, boxMin, boxMax, 0.0f, 1e30f, tNear));
    EXPECT_TRUE(std::isfinite(tNear)); // never NaN/inf
    EXPECT_NEAR(tNear, 4.0f, kTol);    // enters the +Z face at z=1, from z=5
}

TEST(MeshBVHRayAABB, AxisParallelRayOutsideParallelSlabMisses)
{
    const glm::vec3 boxMin(-1.0f);
    const glm::vec3 boxMax(1.0f);
    // +X ray (invDir = (1, +inf, +inf)) whose Y origin (5) is outside the Y slab:
    // parallel to Y and never within it, so it must miss — without a NaN result.
    const glm::vec3 origin(-5.0f, 5.0f, 0.0f);
    const glm::vec3 dir(1.0f, 0.0f, 0.0f);
    const glm::vec3 invDir = 1.0f / dir;

    f32 tNear = 0.0f;
    EXPECT_FALSE(RayIntersect::RayAABB(origin, invDir, boxMin, boxMax, 0.0f, 1e30f, tNear));
}

TEST(MeshBVHRayAABB, AxisParallelRayThroughInteriorHits)
{
    const glm::vec3 boxMin(-1.0f);
    const glm::vec3 boxMax(1.0f);
    // +X ray with Y/Z origins strictly inside their slabs: both parallel axes are
    // unconstraining, and the finite X axis sets the entry distance.
    const glm::vec3 origin(-5.0f, 0.25f, -0.25f);
    const glm::vec3 dir(1.0f, 0.0f, 0.0f);
    const glm::vec3 invDir = 1.0f / dir;

    f32 tNear = -1.0f;
    ASSERT_TRUE(RayIntersect::RayAABB(origin, invDir, boxMin, boxMax, 0.0f, 1e30f, tNear));
    EXPECT_NEAR(tNear, 4.0f, kTol); // enters the -X face at x=-1, from x=-5
}

// -----------------------------------------------------------------------------
// BVH build / structure
// -----------------------------------------------------------------------------

TEST(MeshBVHBuild, EmptyBVHReportsUnbuiltAndNeverHits)
{
    BoundingVolumeHierarchy bvh;
    EXPECT_FALSE(bvh.IsBuilt());
    EXPECT_EQ(bvh.GetTriangleCount(), 0u);
    EXPECT_EQ(bvh.GetNodeCount(), 0u);

    Ray ray(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    RayHit hit;
    EXPECT_FALSE(bvh.CastRay(ray, hit));
    EXPECT_FALSE(hit.Hit);
    EXPECT_FALSE(bvh.CastRayAny(ray));
}

TEST(MeshBVHBuild, SkipsTrianglesWithOutOfRangeIndices)
{
    MeshData mesh;
    mesh.Vertices.Add(MakeVertex({ 0.0f, 0.0f, 0.0f }));
    mesh.Vertices.Add(MakeVertex({ 1.0f, 0.0f, 0.0f }));
    mesh.Vertices.Add(MakeVertex({ 0.0f, 1.0f, 0.0f }));

    // One valid triangle, then one that references a vertex that doesn't exist.
    mesh.Indices.Add(0);
    mesh.Indices.Add(1);
    mesh.Indices.Add(2);
    mesh.Indices.Add(0);
    mesh.Indices.Add(1);
    mesh.Indices.Add(99);

    BoundingVolumeHierarchy bvh;
    bvh.Build(mesh.Vertices.GetData(), static_cast<sizet>(mesh.Vertices.Num()),
              mesh.Indices.GetData(), static_cast<sizet>(mesh.Indices.Num()));

    EXPECT_TRUE(bvh.IsBuilt());
    EXPECT_EQ(bvh.GetTriangleCount(), 1u); // the bogus triangle was dropped
}

TEST(MeshBVHBuild, RootBoundsEncloseTheMesh)
{
    const MeshData cube = MakeCube(0.5f);
    BoundingVolumeHierarchy bvh;
    bvh.Build(cube.Vertices.GetData(), static_cast<sizet>(cube.Vertices.Num()),
              cube.Indices.GetData(), static_cast<sizet>(cube.Indices.Num()));

    const BoundingBox bounds = bvh.GetBounds();
    EXPECT_NEAR(bounds.Min.x, -0.5f, kTol);
    EXPECT_NEAR(bounds.Min.y, -0.5f, kTol);
    EXPECT_NEAR(bounds.Min.z, -0.5f, kTol);
    EXPECT_NEAR(bounds.Max.x, 0.5f, kTol);
    EXPECT_NEAR(bounds.Max.y, 0.5f, kTol);
    EXPECT_NEAR(bounds.Max.z, 0.5f, kTol);
}

// -----------------------------------------------------------------------------
// BVH query semantics
// -----------------------------------------------------------------------------

TEST(MeshBVHQuery, SingleTriangleHitReportsIndexPointAndNormal)
{
    MeshData mesh;
    mesh.Vertices.Add(MakeVertex({ -1.0f, -1.0f, 0.0f }));
    mesh.Vertices.Add(MakeVertex({ 1.0f, -1.0f, 0.0f }));
    mesh.Vertices.Add(MakeVertex({ 0.0f, 1.0f, 0.0f }));
    mesh.Indices.Add(0);
    mesh.Indices.Add(1);
    mesh.Indices.Add(2);

    BoundingVolumeHierarchy bvh;
    bvh.Build(mesh.Vertices.GetData(), static_cast<sizet>(mesh.Vertices.Num()),
              mesh.Indices.GetData(), static_cast<sizet>(mesh.Indices.Num()));

    Ray ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    RayHit hit;
    ASSERT_TRUE(bvh.CastRay(ray, hit));
    EXPECT_TRUE(hit.Hit);
    EXPECT_EQ(hit.TriangleIndex, 0u);
    EXPECT_NEAR(hit.Distance, 3.0f, kTol);
    EXPECT_NEAR(hit.Point.x, 0.0f, kTol);
    EXPECT_NEAR(hit.Point.y, 0.0f, kTol);
    EXPECT_NEAR(hit.Point.z, 0.0f, kTol);
    // Normal must oppose the ray direction (points back toward the origin, +Z).
    EXPECT_LE(glm::dot(hit.Normal, ray.Direction), 0.0f);
    EXPECT_NEAR(hit.Normal.z, 1.0f, kTol);

    // A ray that misses the triangle returns false.
    Ray miss(glm::vec3(5.0f, 5.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    RayHit noHit;
    EXPECT_FALSE(bvh.CastRay(miss, noHit));
}

TEST(MeshBVHQuery, ReturnsClosestOfManyStackedTriangles)
{
    // Five parallel triangles stacked along Z at z = 0,1,2,3,4. A ray from far
    // +Z travelling -Z must hit the nearest (highest-z, triangle index 4).
    MeshData mesh;
    for (u32 i = 0; i < 5; ++i)
    {
        const f32 z = static_cast<f32>(i);
        const u32 base = i * 3;
        mesh.Vertices.Add(MakeVertex({ -1.0f, -1.0f, z }));
        mesh.Vertices.Add(MakeVertex({ 1.0f, -1.0f, z }));
        mesh.Vertices.Add(MakeVertex({ 0.0f, 1.0f, z }));
        mesh.Indices.Add(base + 0);
        mesh.Indices.Add(base + 1);
        mesh.Indices.Add(base + 2);
    }

    BoundingVolumeHierarchy bvh;
    bvh.Build(mesh.Vertices.GetData(), static_cast<sizet>(mesh.Vertices.Num()),
              mesh.Indices.GetData(), static_cast<sizet>(mesh.Indices.Num()));

    Ray ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    RayHit hit;
    ASSERT_TRUE(bvh.CastRay(ray, hit));
    EXPECT_EQ(hit.TriangleIndex, 4u);      // z=4 plane is nearest the origin
    EXPECT_NEAR(hit.Distance, 6.0f, kTol); // 10 - 4
}

TEST(MeshBVHQuery, RayFromInsideClosedMeshHitsAWall)
{
    // Build a unit cube and fire from its center along +X; the ray must hit the
    // +X face from the inside at distance 0.5.
    const MeshData cube = MakeCube(0.5f);
    BoundingVolumeHierarchy bvh;
    bvh.Build(cube.Vertices.GetData(), static_cast<sizet>(cube.Vertices.Num()),
              cube.Indices.GetData(), static_cast<sizet>(cube.Indices.Num()));

    Ray ray(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    RayHit hit;
    ASSERT_TRUE(bvh.CastRay(ray, hit));
    EXPECT_NEAR(hit.Distance, 0.5f, kTol);
    EXPECT_NEAR(hit.Point.x, 0.5f, kTol);
    EXPECT_TRUE(bvh.CastRayAny(ray));
}

TEST(MeshBVHQuery, CastRayAnyAgreesWithCastRayHitMiss)
{
    const MeshData cube = MakeCube(0.5f);
    BoundingVolumeHierarchy bvh;
    bvh.Build(cube.Vertices.GetData(), static_cast<sizet>(cube.Vertices.Num()),
              cube.Indices.GetData(), static_cast<sizet>(cube.Indices.Num()));

    // Straight at the cube → both hit.
    Ray hitRay(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    RayHit hit;
    EXPECT_TRUE(bvh.CastRay(hitRay, hit));
    EXPECT_TRUE(bvh.CastRayAny(hitRay));

    // Pointing away from the cube → both miss.
    Ray awayRay(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    RayHit away;
    EXPECT_FALSE(bvh.CastRay(awayRay, away));
    EXPECT_FALSE(bvh.CastRayAny(awayRay));
}

TEST(MeshBVHQuery, BuildsFromMeshSourceOverload)
{
    // The MeshSource overload must produce the same hit as the raw-pointer build.
    const MeshData cube = MakeCube(0.5f);
    auto source = Ref<MeshSource>::Create(cube.Vertices, cube.Indices);
    ASSERT_TRUE(source);

    BoundingVolumeHierarchy bvh;
    bvh.Build(*source);
    EXPECT_TRUE(bvh.IsBuilt());
    EXPECT_EQ(bvh.GetTriangleCount(), 12u);

    Ray ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    RayHit hit;
    ASSERT_TRUE(bvh.CastRay(ray, hit));
    EXPECT_NEAR(hit.Distance, 2.5f, kTol); // origin z=3, +Z face at z=0.5
}

// -----------------------------------------------------------------------------
// Brute-force parity — the accelerator must match the O(n) ground truth.
// -----------------------------------------------------------------------------

namespace
{
    void ExpectBVHMatchesBruteForce(const MeshData& mesh, u32 originCount)
    {
        BoundingVolumeHierarchy bvh;
        bvh.Build(mesh.Vertices.GetData(), static_cast<sizet>(mesh.Vertices.Num()),
                  mesh.Indices.GetData(), static_cast<sizet>(mesh.Indices.Num()));
        ASSERT_TRUE(bvh.IsBuilt());

        // Generic origins on a radius-5 shell; targets mix interior (guaranteed
        // hit) and exterior (graze / miss) so we exercise both branches.
        const std::vector<glm::vec3> origins = FibonacciSphere(originCount, 5.0f);
        std::vector<glm::vec3> targets = FibonacciSphere(13, 0.9f);
        const std::vector<glm::vec3> outer = FibonacciSphere(13, 1.35f);
        targets.insert(targets.end(), outer.begin(), outer.end());
        targets.emplace_back(0.0f, 0.0f, 0.0f);

        u32 hitCount = 0;
        u32 missCount = 0;
        for (const glm::vec3& origin : origins)
        {
            for (const glm::vec3& target : targets)
            {
                const glm::vec3 delta = target - origin;
                const f32 len = glm::length(delta);
                if (len < 1e-4f)
                    continue;
                const Ray ray(origin, delta / len, 0.0f, 100.0f);

                RayHit bvhHit;
                const bool bvhResult = bvh.CastRay(ray, bvhHit);
                const RayHit reference = BruteForceClosest(mesh, ray);

                ASSERT_EQ(bvhResult, reference.Hit)
                    << "hit/miss disagreement at origin (" << origin.x << "," << origin.y << "," << origin.z
                    << ") dir (" << ray.Direction.x << "," << ray.Direction.y << "," << ray.Direction.z << ")";
                ASSERT_EQ(bvh.CastRayAny(ray), reference.Hit) << "CastRayAny disagreed with brute force";

                if (reference.Hit)
                {
                    ++hitCount;
                    EXPECT_NEAR(bvhHit.Distance, reference.Distance, kTol);
                    EXPECT_NEAR(bvhHit.Point.x, reference.Point.x, kTol);
                    EXPECT_NEAR(bvhHit.Point.y, reference.Point.y, kTol);
                    EXPECT_NEAR(bvhHit.Point.z, reference.Point.z, kTol);
                }
                else
                {
                    ++missCount;
                }
            }
        }

        // Guard against a degenerate sweep that accidentally tests only hits or
        // only misses (which would make the parity check vacuous).
        EXPECT_GT(hitCount, 0u);
        EXPECT_GT(missCount, 0u);
    }
} // namespace

TEST(MeshBVHParity, MatchesBruteForceOverCube)
{
    ExpectBVHMatchesBruteForce(MakeCube(1.0f), /*originCount*/ 32);
}

TEST(MeshBVHParity, MatchesBruteForceOverSphere)
{
    // 16x24 UV sphere = 768 triangles → a genuinely multi-level BVH.
    const MeshData sphere = MakeUVSphere(1.0f, 16, 24);
    BoundingVolumeHierarchy bvh;
    bvh.Build(sphere.Vertices.GetData(), static_cast<sizet>(sphere.Vertices.Num()),
              sphere.Indices.GetData(), static_cast<sizet>(sphere.Indices.Num()));
    ASSERT_TRUE(bvh.IsBuilt());
    EXPECT_GT(bvh.GetNodeCount(), 1u) << "sphere should produce an internal tree, not one leaf";

    ExpectBVHMatchesBruteForce(sphere, /*originCount*/ 32);
}

// -----------------------------------------------------------------------------
// Regression: heap-constructing a MeshSource by COPY must not corrupt the heap.
//
// MeshSource(const TArray<Vertex>&, const TArray<u32>&) used to copy-initialise
// its TArray members in place. Under an optimised (Release/LTO) build that path
// overran a heap-allocated MeshSource (the Ref<MeshSource>::Create route) and
// clobbered adjacent heap allocations. The fault was invisible on the stack
// (slack absorbed the overrun) and to ASan (the engine's custom FMemory
// allocator is not instrumented), so it only surfaced as an intermittent access
// violation in a later, unrelated allocation -- the SEH 0xC0000005 that crashed
// MeshBVHQuery.BuildsFromMeshSourceOverload in the single-process cross-vendor
// job. The copy constructor now delegates to the (unaffected) move constructor;
// this test stresses that path: it kept many heap MeshSources alive (so an
// overrun would clobber a live neighbour) and verifies their data, then runs the
// full create -> BVH build -> query -> destroy cycle under heap churn.
// -----------------------------------------------------------------------------
TEST(MeshSourceHeapCopyRegression, RepeatedHeapCopyConstructionKeepsHeapIntact)
{
    const MeshData cube = MakeCube(0.5f);

    constexpr int kCount = 4096;
    std::vector<Ref<MeshSource>> sources;
    sources.reserve(static_cast<sizet>(kCount));
    for (int i = 0; i < kCount; ++i)
    {
        sources.push_back(Ref<MeshSource>::Create(cube.Vertices, cube.Indices));
        ASSERT_TRUE(sources.back()) << "Ref<MeshSource>::Create returned null at " << i;
        // Unrelated heap traffic of varying size, to drive allocator reuse the way
        // the surrounding gtest machinery did when the crash first reproduced.
        std::vector<u8> churn(static_cast<sizet>(13 + (i % 97)), static_cast<u8>(i));
        ASSERT_FALSE(churn.empty());
    }

    for (int i = 0; i < kCount; ++i)
    {
        const MeshSource& ms = *sources[static_cast<sizet>(i)];
        ASSERT_EQ(ms.GetVertices().Num(), cube.Vertices.Num()) << "vertex count corrupted at " << i;
        ASSERT_EQ(ms.GetIndices().Num(), cube.Indices.Num()) << "index count corrupted at " << i;
        const BoundingBox& bb = ms.GetBoundingBox();
        ASSERT_TRUE(std::isfinite(bb.Min.x) && std::isfinite(bb.Max.x)) << "bounds corrupted at " << i;
        EXPECT_NEAR(bb.Min.x, -0.5f, kTol);
        EXPECT_NEAR(bb.Max.x, 0.5f, kTol);
    }
    sources.clear();

    for (int i = 0; i < kCount; ++i)
    {
        auto source = Ref<MeshSource>::Create(cube.Vertices, cube.Indices);
        ASSERT_TRUE(source);
        BoundingVolumeHierarchy bvh;
        bvh.Build(*source);
        ASSERT_EQ(bvh.GetTriangleCount(), 12u);
        Ray ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
        RayHit hit;
        ASSERT_TRUE(bvh.CastRay(ray, hit));
        EXPECT_NEAR(hit.Distance, 2.5f, kTol);
        std::vector<char> churn(static_cast<sizet>(17 + (i % 64)), 'x');
        ASSERT_FALSE(churn.empty());
    }
}

TEST(MeshBVHParity, MatchesBruteForceForAxisAlignedRays)
{
    // Axis-aligned rays (a zero direction component => invDir == +/-inf) are the
    // case FibonacciSphere deliberately skips. Drive the full BVH query path for
    // all six axis directions and confirm it still matches the O(n) reference —
    // this catches a RayAABB that mishandles infinite invDir and wrongly prunes a
    // real hit. (Brute force uses only RayTriangle, so any AABB-pruning bug shows
    // up as a divergence.)
    const MeshData cube = MakeCube(1.0f);
    BoundingVolumeHierarchy bvh;
    bvh.Build(cube.Vertices.GetData(), static_cast<sizet>(cube.Vertices.Num()),
              cube.Indices.GetData(), static_cast<sizet>(cube.Indices.Num()));
    ASSERT_TRUE(bvh.IsBuilt());

    const glm::vec3 axisDirs[6] = {
        { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f }
    };
    // Offsets on the two axes perpendicular to dir: |offset| < 1 lands inside the
    // cube cross-section (a clean hit), |offset| > 1 misses. None are exactly +/-1,
    // so the triangle hits stay off the face edges and remain unambiguous.
    const f32 offsets[5] = { -1.4f, -0.5f, 0.0f, 0.6f, 1.4f };

    u32 hitCount = 0;
    u32 missCount = 0;
    for (const glm::vec3& dir : axisDirs)
    {
        // Start 5 units outside the cube along -dir, then sweep the perpendicular plane.
        const glm::vec3 along = dir * -5.0f;
        for (f32 a : offsets)
        {
            for (f32 b : offsets)
            {
                glm::vec3 perp(0.0f);
                if (std::abs(dir.x) > 0.5f)
                    perp = glm::vec3(0.0f, a, b);
                else if (std::abs(dir.y) > 0.5f)
                    perp = glm::vec3(a, 0.0f, b);
                else
                    perp = glm::vec3(a, b, 0.0f);

                const Ray ray(along + perp, dir, 0.0f, 100.0f);

                RayHit bvhHit;
                const bool bvhResult = bvh.CastRay(ray, bvhHit);
                const RayHit reference = BruteForceClosest(cube, ray);

                ASSERT_EQ(bvhResult, reference.Hit)
                    << "axis dir (" << dir.x << "," << dir.y << "," << dir.z << ") offsets (" << a << "," << b << ")";
                ASSERT_EQ(bvh.CastRayAny(ray), reference.Hit) << "CastRayAny disagreed with brute force";

                if (reference.Hit)
                {
                    ++hitCount;
                    EXPECT_NEAR(bvhHit.Distance, reference.Distance, kTol);
                    EXPECT_NEAR(bvhHit.Point.x, reference.Point.x, kTol);
                    EXPECT_NEAR(bvhHit.Point.y, reference.Point.y, kTol);
                    EXPECT_NEAR(bvhHit.Point.z, reference.Point.z, kTol);
                }
                else
                {
                    ++missCount;
                }
            }
        }
    }

    EXPECT_GT(hitCount, 0u);
    EXPECT_GT(missCount, 0u);
}
