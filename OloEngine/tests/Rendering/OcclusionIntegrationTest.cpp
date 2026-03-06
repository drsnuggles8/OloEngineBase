#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Frustum.h"

#include <cstring>
#include <type_traits>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Helper: Build a standard perspective frustum
// =============================================================================

static Frustum MakePerspectiveFrustum(f32 fovDeg = 60.0f, f32 aspect = 16.0f / 9.0f,
                                      f32 nearPlane = 0.1f, f32 farPlane = 100.0f,
                                      const glm::vec3& eye = glm::vec3(0, 0, 0),
                                      const glm::vec3& target = glm::vec3(0, 0, -1))
{
    glm::mat4 proj = glm::perspective(glm::radians(fovDeg), aspect, nearPlane, farPlane);
    glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
    return Frustum(proj * view);
}

// =============================================================================
// DrawMeshCommand: occlusionQueryIndex Field Tests
// =============================================================================

TEST(OcclusionIntegration, DrawMeshCommandDefaultQueryIndex)
{
    DrawMeshCommand cmd{};
    EXPECT_EQ(cmd.occlusionQueryIndex, UINT32_MAX)
        << "Default should indicate no active occlusion query";
}

TEST(OcclusionIntegration, DrawMeshCommandQueryIndexRoundTrip)
{
    DrawMeshCommand cmd = MakeSyntheticDrawMeshCommand(10, 20, 5.0f, 100);
    cmd.occlusionQueryIndex = 42;

    // Verify memcpy preserves the field (POD guarantee)
    DrawMeshCommand copy{};
    std::memcpy(&copy, &cmd, sizeof(DrawMeshCommand));

    EXPECT_EQ(copy.occlusionQueryIndex, 42u);
}

TEST(OcclusionIntegration, DrawMeshCommandStillTriviallyCopyable)
{
    // Confirm adding occlusionQueryIndex didn't break POD-ness
    static_assert(std::is_trivially_copyable_v<DrawMeshCommand>,
                  "DrawMeshCommand must remain trivially copyable after adding occlusionQueryIndex");
}

TEST(OcclusionIntegration, DrawMeshCommandSizeBound)
{
    EXPECT_LE(sizeof(DrawMeshCommand), MAX_COMMAND_SIZE)
        << "DrawMeshCommand (with occlusionQueryIndex) must fit in command packet";
}

// =============================================================================
// Frustum Culling: Particle BoundingSphere
// =============================================================================

TEST(OcclusionIntegration, ParticleSphereInsideFrustum)
{
    Frustum frustum = MakePerspectiveFrustum();
    BoundingSphere sphere(glm::vec3(0.0f, 0.0f, -10.0f), 2.0f);

    EXPECT_TRUE(frustum.IsBoundingSphereVisible(sphere))
        << "Particle sphere at center of view should be visible";
}

TEST(OcclusionIntegration, ParticleSphereBehindCamera)
{
    Frustum frustum = MakePerspectiveFrustum();
    BoundingSphere sphere(glm::vec3(0.0f, 0.0f, 10.0f), 2.0f);

    EXPECT_FALSE(frustum.IsBoundingSphereVisible(sphere))
        << "Particle sphere behind camera should be culled";
}

TEST(OcclusionIntegration, ParticleSpherePartiallyIntersecting)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    // Sphere centered at far plane with large radius should intersect
    BoundingSphere sphere(glm::vec3(0.0f, 0.0f, -100.0f), 5.0f);

    EXPECT_TRUE(frustum.IsBoundingSphereVisible(sphere))
        << "Sphere intersecting far plane should be visible (conservative)";
}

TEST(OcclusionIntegration, ParticleSphereBeyondFarPlane)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    // Sphere well beyond far plane (center at -200, radius 1)
    BoundingSphere sphere(glm::vec3(0.0f, 0.0f, -200.0f), 1.0f);

    EXPECT_FALSE(frustum.IsBoundingSphereVisible(sphere))
        << "Sphere fully beyond far plane should be culled";
}

// =============================================================================
// Frustum Culling: Foliage BoundingBox
// =============================================================================

TEST(OcclusionIntegration, FoliageBoundsInsideFrustum)
{
    Frustum frustum = MakePerspectiveFrustum();
    BoundingBox bounds(glm::vec3(-5.0f, 0.0f, -15.0f), glm::vec3(5.0f, 5.0f, -5.0f));

    EXPECT_TRUE(frustum.IsBoundingBoxVisible(bounds))
        << "Foliage layer bounds in front of camera should be visible";
}

TEST(OcclusionIntegration, FoliageBoundsBehindCamera)
{
    Frustum frustum = MakePerspectiveFrustum();
    BoundingBox bounds(glm::vec3(-5.0f, 0.0f, 5.0f), glm::vec3(5.0f, 5.0f, 15.0f));

    EXPECT_FALSE(frustum.IsBoundingBoxVisible(bounds))
        << "Foliage layer bounds behind camera should be culled";
}

TEST(OcclusionIntegration, FoliageBoundsFarAway)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    BoundingBox bounds(glm::vec3(-1.0f, -1.0f, -300.0f), glm::vec3(1.0f, 1.0f, -200.0f));

    EXPECT_FALSE(frustum.IsBoundingBoxVisible(bounds))
        << "Foliage bounds beyond far plane should be culled";
}

// =============================================================================
// Per-Instance Frustum Culling Simulation
// =============================================================================

TEST(OcclusionIntegration, PerInstanceCullingFiltersCorrectly)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);

    // Simulate 5 instance transforms: 3 visible, 2 behind camera
    std::vector<glm::mat4> transforms = {
        glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -10)), // visible
        glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 50)),  // behind camera
        glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -30)), // visible
        glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 100)), // behind camera
        glm::translate(glm::mat4(1.0f), glm::vec3(2, 0, -5)),  // visible
    };

    // Simulate the per-instance culling logic from DrawMeshInstanced:
    // For each instance, test its position against the frustum
    std::vector<glm::mat4> activeTransforms;
    for (const auto& t : transforms)
    {
        glm::vec3 pos = glm::vec3(t[3]); // Extract translation
        if (frustum.IsPointVisible(pos))
        {
            activeTransforms.push_back(t);
        }
    }

    EXPECT_EQ(activeTransforms.size(), 3u)
        << "Only 3 of 5 instances should pass frustum test";
}

TEST(OcclusionIntegration, PerInstanceCullingAllVisible)
{
    Frustum frustum = MakePerspectiveFrustum(90.0f, 1.0f, 0.1f, 1000.0f);

    std::vector<glm::mat4> transforms;
    for (i32 i = 0; i < 10; ++i)
    {
        transforms.push_back(
            glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -static_cast<f32>(i + 1))));
    }

    u32 visibleCount = 0;
    for (const auto& t : transforms)
    {
        glm::vec3 pos = glm::vec3(t[3]);
        if (frustum.IsPointVisible(pos))
        {
            ++visibleCount;
        }
    }

    EXPECT_EQ(visibleCount, 10u)
        << "All instances in front of camera should be visible";
}

TEST(OcclusionIntegration, PerInstanceCullingNoneVisible)
{
    Frustum frustum = MakePerspectiveFrustum(60.0f, 1.0f, 0.1f, 50.0f);

    std::vector<glm::mat4> transforms;
    for (i32 i = 0; i < 5; ++i)
    {
        // All behind camera
        transforms.push_back(
            glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, static_cast<f32>(i + 10))));
    }

    u32 visibleCount = 0;
    for (const auto& t : transforms)
    {
        glm::vec3 pos = glm::vec3(t[3]);
        if (frustum.IsPointVisible(pos))
        {
            ++visibleCount;
        }
    }

    EXPECT_EQ(visibleCount, 0u)
        << "All instances behind camera should be culled";
}

// =============================================================================
// BoundingBox Center/Extents for Occlusion Proxy Transform
// =============================================================================

TEST(OcclusionIntegration, BoundingBoxCenterExtentsForProxy)
{
    // This tests the AABB → model-matrix computation used in OcclusionCuller
    BoundingBox box(glm::vec3(-3.0f, -1.0f, -5.0f), glm::vec3(3.0f, 1.0f, 5.0f));

    glm::vec3 center = box.GetCenter();
    glm::vec3 extents = box.GetExtents();

    EXPECT_FLOAT_EQ(center.x, 0.0f);
    EXPECT_FLOAT_EQ(center.y, 0.0f);
    EXPECT_FLOAT_EQ(center.z, 0.0f);
    EXPECT_FLOAT_EQ(extents.x, 3.0f);
    EXPECT_FLOAT_EQ(extents.y, 1.0f);
    EXPECT_FLOAT_EQ(extents.z, 5.0f);

    // The proxy model matrix should translate to center and scale by extents
    glm::mat4 model = glm::translate(glm::mat4(1.0f), center);
    model = glm::scale(model, extents);

    // A unit cube corner (-1,-1,-1) transformed should land at box Min
    glm::vec4 corner = model * glm::vec4(-1.0f, -1.0f, -1.0f, 1.0f);
    EXPECT_FLOAT_EQ(corner.x, box.Min.x);
    EXPECT_FLOAT_EQ(corner.y, box.Min.y);
    EXPECT_FLOAT_EQ(corner.z, box.Min.z);

    // And (+1,+1,+1) should land at box Max
    glm::vec4 maxCorner = model * glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(maxCorner.x, box.Max.x);
    EXPECT_FLOAT_EQ(maxCorner.y, box.Max.y);
    EXPECT_FLOAT_EQ(maxCorner.z, box.Max.z);
}

TEST(OcclusionIntegration, BoundingBoxAsymmetricProxy)
{
    BoundingBox box(glm::vec3(10.0f, 20.0f, 30.0f), glm::vec3(14.0f, 25.0f, 36.0f));

    glm::vec3 center = box.GetCenter();
    glm::vec3 extents = box.GetExtents();

    EXPECT_FLOAT_EQ(center.x, 12.0f);
    EXPECT_FLOAT_EQ(center.y, 22.5f);
    EXPECT_FLOAT_EQ(center.z, 33.0f);
    EXPECT_FLOAT_EQ(extents.x, 2.0f);
    EXPECT_FLOAT_EQ(extents.y, 2.5f);
    EXPECT_FLOAT_EQ(extents.z, 3.0f);
}

// =============================================================================
// BoundingSphere from BoundingBox (used for particle bounding volumes)
// =============================================================================

TEST(OcclusionIntegration, BoundingSphereFromBoundingBox)
{
    BoundingBox box(glm::vec3(-2.0f, -2.0f, -2.0f), glm::vec3(2.0f, 2.0f, 2.0f));
    BoundingSphere sphere(box);

    EXPECT_FLOAT_EQ(sphere.Center.x, 0.0f);
    EXPECT_FLOAT_EQ(sphere.Center.y, 0.0f);
    EXPECT_FLOAT_EQ(sphere.Center.z, 0.0f);

    // Radius = length of extents = length(2,2,2) = sqrt(12)
    f32 expectedRadius = glm::length(glm::vec3(2.0f, 2.0f, 2.0f));
    EXPECT_NEAR(sphere.Radius, expectedRadius, 0.001f);
}
