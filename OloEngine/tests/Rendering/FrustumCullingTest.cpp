#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <random>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// Helper to build a standard perspective frustum
static Frustum MakePerspectiveFrustum(f32 fovDeg = 60.0f, f32 aspect = 16.0f / 9.0f,
                                      f32 nearPlane = 0.1f, f32 farPlane = 100.0f)
{
    glm::mat4 proj = glm::perspective(glm::radians(fovDeg), aspect, nearPlane, farPlane);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    return Frustum(proj * view);
}

// Helper to build an orthographic frustum
static Frustum MakeOrthoFrustum(f32 left = -10.0f, f32 right = 10.0f,
                                f32 bottom = -10.0f, f32 top = 10.0f,
                                f32 nearPlane = 0.1f, f32 farPlane = 100.0f)
{
    glm::mat4 proj = glm::ortho(left, right, bottom, top, nearPlane, farPlane);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    return Frustum(proj * view);
}

// =============================================================================
// Point Visibility
// =============================================================================

TEST(Frustum, OriginVisibleInDefaultFrustum)
{
    // Camera at origin looking down -Z, near=0.1, far=100
    Frustum frustum = MakePerspectiveFrustum();

    // A point slightly in front of the camera, well within the frustum
    EXPECT_TRUE(frustum.IsPointVisible(glm::vec3(0.0f, 0.0f, -10.0f)));
}

TEST(Frustum, PointBehindCameraNotVisible)
{
    Frustum frustum = MakePerspectiveFrustum();

    // A point behind the camera (positive Z)
    EXPECT_FALSE(frustum.IsPointVisible(glm::vec3(0.0f, 0.0f, 10.0f)));
}

TEST(Frustum, PointBeyondFarPlaneNotVisible)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);

    // Point way beyond the far plane
    EXPECT_FALSE(frustum.IsPointVisible(glm::vec3(0.0f, 0.0f, -200.0f)));
}

TEST(Frustum, PointBeforeNearPlaneNotVisible)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 1.0f, 100.0f);

    // Point between camera and near plane
    EXPECT_FALSE(frustum.IsPointVisible(glm::vec3(0.0f, 0.0f, -0.01f)));
}

// =============================================================================
// Sphere Visibility — Conservative Culling
// =============================================================================

TEST(Frustum, FullyInsideSphereNeverCulled)
{
    Frustum frustum = MakePerspectiveFrustum();

    // Sphere entirely inside the frustum
    glm::vec3 center(0.0f, 0.0f, -10.0f);
    f32 radius = 1.0f;

    EXPECT_TRUE(frustum.IsSphereVisible(center, radius))
        << "A sphere fully inside the frustum must never be culled";
}

TEST(Frustum, FullyOutsideSphereAlwaysCulled)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);

    // Sphere far behind the camera
    EXPECT_FALSE(frustum.IsSphereVisible(glm::vec3(0.0f, 0.0f, 50.0f), 1.0f))
        << "A sphere fully behind the camera should be culled";

    // Sphere far to the right, outside the frustum
    EXPECT_FALSE(frustum.IsSphereVisible(glm::vec3(1000.0f, 0.0f, -10.0f), 1.0f))
        << "A sphere far to the right should be culled";
}

TEST(Frustum, SphereTouchingPlaneIsVisible)
{
    // Conservative culling: a sphere touching the frustum boundary should NOT be culled
    // (false positives are OK, false negatives are bugs)
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);

    // Sphere centered just at the far plane with radius 1
    glm::vec3 center(0.0f, 0.0f, -100.0f);
    f32 radius = 1.0f;

    // This sphere intersects the far plane — conservative test should pass
    EXPECT_TRUE(frustum.IsSphereVisible(center, radius))
        << "Sphere intersecting the far plane should be visible (conservative)";
}

TEST(Frustum, BoundingSphereVisibility)
{
    Frustum frustum = MakePerspectiveFrustum();

    BoundingSphere inside(glm::vec3(0.0f, 0.0f, -10.0f), 1.0f);
    BoundingSphere outside(glm::vec3(0.0f, 0.0f, 50.0f), 1.0f);

    EXPECT_TRUE(frustum.IsBoundingSphereVisible(inside));
    EXPECT_FALSE(frustum.IsBoundingSphereVisible(outside));
}

// =============================================================================
// Box Visibility
// =============================================================================

TEST(Frustum, BoxInsideFrustumVisible)
{
    Frustum frustum = MakePerspectiveFrustum();

    EXPECT_TRUE(frustum.IsBoxVisible(glm::vec3(-1.0f, -1.0f, -11.0f),
                                     glm::vec3(1.0f, 1.0f, -9.0f)));
}

TEST(Frustum, BoxOutsideFrustumNotVisible)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);

    // Box far behind the camera
    EXPECT_FALSE(frustum.IsBoxVisible(glm::vec3(-1.0f, -1.0f, 50.0f),
                                      glm::vec3(1.0f, 1.0f, 52.0f)));
}

TEST(Frustum, BoundingBoxVisibility)
{
    Frustum frustum = MakePerspectiveFrustum();

    BoundingBox inside(glm::vec3(-1.0f, -1.0f, -11.0f), glm::vec3(1.0f, 1.0f, -9.0f));
    BoundingBox outside(glm::vec3(-1.0f, -1.0f, 50.0f), glm::vec3(1.0f, 1.0f, 52.0f));

    EXPECT_TRUE(frustum.IsBoundingBoxVisible(inside));
    EXPECT_FALSE(frustum.IsBoundingBoxVisible(outside));
}

// =============================================================================
// Frustum from Orthographic Projection
// =============================================================================

TEST(Frustum, OrthographicFrustumCulling)
{
    Frustum frustum = MakeOrthoFrustum(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f);

    // Inside
    EXPECT_TRUE(frustum.IsPointVisible(glm::vec3(0.0f, 0.0f, -50.0f)));

    // Outside left
    EXPECT_FALSE(frustum.IsPointVisible(glm::vec3(-20.0f, 0.0f, -50.0f)));

    // Outside right
    EXPECT_FALSE(frustum.IsPointVisible(glm::vec3(20.0f, 0.0f, -50.0f)));
}

// =============================================================================
// Frustum Update
// =============================================================================

TEST(Frustum, UpdateChangesPlanes)
{
    Frustum frustum = MakePerspectiveFrustum();

    // Point that IS visible with default camera
    glm::vec3 testPoint(0.0f, 0.0f, -10.0f);
    EXPECT_TRUE(frustum.IsPointVisible(testPoint));

    // Rotate camera 180 degrees — now looking at +Z
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0));
    frustum.Update(proj * view);

    // The same point at -Z should now be behind the camera
    EXPECT_FALSE(frustum.IsPointVisible(testPoint));
}

// =============================================================================
// Plane Access
// =============================================================================

TEST(Frustum, PlaneNormalsAreNormalized)
{
    Frustum frustum = MakePerspectiveFrustum();

    for (i32 i = 0; i < static_cast<i32>(Frustum::Planes::Count); ++i)
    {
        const Plane& plane = frustum.GetPlane(static_cast<Frustum::Planes>(i));
        f32 normalLength = glm::length(plane.Normal);
        EXPECT_NEAR(normalLength, 1.0f, 1e-4f)
            << "Plane " << i << " normal is not normalized (length=" << normalLength << ")";
        ValidateVec3(plane.Normal, "plane normal");
    }
}

// =============================================================================
// Stress Test — Random Spheres
// =============================================================================

TEST(Frustum, StressRandomSpheres_NoNaN)
{
    Frustum frustum = MakePerspectiveFrustum();
    auto rng = MakeTestRNG();
    std::uniform_real_distribution<f32> posDist(-200.0f, 200.0f);
    std::uniform_real_distribution<f32> radDist(0.01f, 50.0f);

    for (i32 i = 0; i < 10000; ++i)
    {
        glm::vec3 center(posDist(rng), posDist(rng), posDist(rng));
        f32 radius = radDist(rng);

        // Verify sphere parameters are finite
        ASSERT_TRUE(std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.z))
            << "Sphere center contains non-finite value at iteration " << i;
        ASSERT_TRUE(std::isfinite(radius))
            << "Sphere radius is non-finite at iteration " << i;

        // Should not crash or produce NaN
        bool result = frustum.IsSphereVisible(center, radius);

        // If the sphere is marked as visible, verify plane distances are finite
        // and ALL planes have a signed distance >= -radius
        if (result)
        {
            bool allPlanesPass = true;
            for (i32 p = 0; p < static_cast<i32>(Frustum::Planes::Count); ++p)
            {
                const Plane& plane = frustum.GetPlane(static_cast<Frustum::Planes>(p));
                f32 signedDist = plane.GetSignedDistance(center);
                EXPECT_TRUE(std::isfinite(signedDist))
                    << "Plane " << p << " signed distance is non-finite for sphere at ("
                    << center.x << "," << center.y << "," << center.z << ") r=" << radius;
                if (signedDist < -radius)
                {
                    allPlanesPass = false;
                    break;
                }
            }
            EXPECT_TRUE(allPlanesPass)
                << "Sphere at (" << center.x << "," << center.y << "," << center.z
                << ") r=" << radius << " marked visible but fails a plane test";
        }
    }
}

// =============================================================================
// Plane Construction
// =============================================================================

TEST(Plane, ConstructFromNormalAndDistance)
{
    Plane p(glm::vec3(0, 1, 0), -5.0f);
    EXPECT_EQ(p.Normal, glm::vec3(0, 1, 0));
    EXPECT_FLOAT_EQ(p.Distance, -5.0f);
}

TEST(Plane, ConstructFromNormalAndPoint)
{
    Plane p(glm::vec3(0, 1, 0), glm::vec3(0, 5, 0));
    EXPECT_NEAR(p.Normal.y, 1.0f, 1e-5f);
    EXPECT_NEAR(p.Distance, -5.0f, 1e-5f);
}

TEST(Plane, SignedDistanceAbovePositive)
{
    Plane p(glm::vec3(0, 1, 0), 0.0f); // y=0 plane, normal pointing up
    EXPECT_GT(p.GetSignedDistance(glm::vec3(0, 5, 0)), 0.0f);
}

TEST(Plane, SignedDistanceBelowNegative)
{
    Plane p(glm::vec3(0, 1, 0), 0.0f); // y=0 plane, normal pointing up
    EXPECT_LT(p.GetSignedDistance(glm::vec3(0, -5, 0)), 0.0f);
}

TEST(Plane, SignedDistanceOnPlaneZero)
{
    Plane p(glm::vec3(0, 1, 0), 0.0f);
    EXPECT_NEAR(p.GetSignedDistance(glm::vec3(100, 0, 200)), 0.0f, 1e-5f);
}
