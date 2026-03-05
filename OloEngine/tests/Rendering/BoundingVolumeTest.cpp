#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <random>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// BoundingBox Tests
// =============================================================================

TEST(BoundingBox, ConstructFromMinMax)
{
    BoundingBox box(glm::vec3(-1.0f), glm::vec3(1.0f));
    EXPECT_EQ(box.Min, glm::vec3(-1.0f));
    EXPECT_EQ(box.Max, glm::vec3(1.0f));
}

TEST(BoundingBox, CenterIsAverage)
{
    BoundingBox box(glm::vec3(-2.0f, 0.0f, -4.0f), glm::vec3(2.0f, 6.0f, 4.0f));
    glm::vec3 center = box.GetCenter();
    EXPECT_FLOAT_EQ(center.x, 0.0f);
    EXPECT_FLOAT_EQ(center.y, 3.0f);
    EXPECT_FLOAT_EQ(center.z, 0.0f);
    ValidateVec3(center, "center");
}

TEST(BoundingBox, SizeIsCorrect)
{
    BoundingBox box(glm::vec3(-1.0f, -2.0f, -3.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    glm::vec3 size = box.GetSize();
    EXPECT_FLOAT_EQ(size.x, 2.0f);
    EXPECT_FLOAT_EQ(size.y, 4.0f);
    EXPECT_FLOAT_EQ(size.z, 6.0f);
}

TEST(BoundingBox, ExtentsAreHalfSize)
{
    BoundingBox box(glm::vec3(0.0f), glm::vec3(4.0f, 6.0f, 8.0f));
    glm::vec3 extents = box.GetExtents();
    EXPECT_FLOAT_EQ(extents.x, 2.0f);
    EXPECT_FLOAT_EQ(extents.y, 3.0f);
    EXPECT_FLOAT_EQ(extents.z, 4.0f);
}

TEST(BoundingBox, ContainsCorners)
{
    BoundingBox box(glm::vec3(-1.0f), glm::vec3(1.0f));
    // All 8 corners should be on the boundary (min <= corner <= max)
    glm::vec3 corners[8] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 }, { -1, -1, 1 }, { 1, -1, 1 }, { -1, 1, 1 }, { 1, 1, 1 }
    };
    for (u32 i = 0; i < 8; ++i)
    {
        EXPECT_TRUE(glm::all(glm::greaterThanEqual(corners[i], box.Min)))
            << "Corner " << i << " is below Min";
        EXPECT_TRUE(glm::all(glm::lessThanEqual(corners[i], box.Max)))
            << "Corner " << i << " is above Max";
    }
}

TEST(BoundingBox, UnionContainsBoth)
{
    BoundingBox a(glm::vec3(-1.0f), glm::vec3(1.0f));
    BoundingBox b(glm::vec3(0.0f), glm::vec3(3.0f));
    BoundingBox merged = a.Union(b);

    EXPECT_EQ(merged.Min, glm::vec3(-1.0f));
    EXPECT_EQ(merged.Max, glm::vec3(3.0f));

    // Both original centers must be inside the merged box
    glm::vec3 centerA = a.GetCenter();
    glm::vec3 centerB = b.GetCenter();
    EXPECT_TRUE(glm::all(glm::greaterThanEqual(centerA, merged.Min)));
    EXPECT_TRUE(glm::all(glm::lessThanEqual(centerA, merged.Max)));
    EXPECT_TRUE(glm::all(glm::greaterThanEqual(centerB, merged.Min)));
    EXPECT_TRUE(glm::all(glm::lessThanEqual(centerB, merged.Max)));
}

TEST(BoundingBox, TransformPreservesContainment)
{
    BoundingBox box(glm::vec3(-1.0f), glm::vec3(1.0f));
    glm::vec3 pointInside(0.5f, -0.5f, 0.0f);

    // Apply a rotation + translation
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f));
    transform = glm::rotate(transform, glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    BoundingBox transformedBox = box.Transform(transform);
    glm::vec4 transformedPoint4 = transform * glm::vec4(pointInside, 1.0f);
    glm::vec3 transformedPoint = glm::vec3(transformedPoint4) / transformedPoint4.w;

    // The transformed point should still be inside the transformed box
    // (conservative — the AABB of a rotated box is larger)
    EXPECT_TRUE(glm::all(glm::greaterThanEqual(transformedPoint, transformedBox.Min)))
        << "Transformed point below transformed box Min";
    EXPECT_TRUE(glm::all(glm::lessThanEqual(transformedPoint, transformedBox.Max)))
        << "Transformed point above transformed box Max";

    ValidateVec3(transformedBox.Min, "transformed Min");
    ValidateVec3(transformedBox.Max, "transformed Max");
}

TEST(BoundingBox, ConstructFromPoints)
{
    glm::vec3 points[] = {
        { 1.0f, 2.0f, 3.0f },
        { -5.0f, 0.0f, 10.0f },
        { 3.0f, -1.0f, -2.0f }
    };
    BoundingBox box(points, 3);

    EXPECT_FLOAT_EQ(box.Min.x, -5.0f);
    EXPECT_FLOAT_EQ(box.Min.y, -1.0f);
    EXPECT_FLOAT_EQ(box.Min.z, -2.0f);
    EXPECT_FLOAT_EQ(box.Max.x, 3.0f);
    EXPECT_FLOAT_EQ(box.Max.y, 2.0f);
    EXPECT_FLOAT_EQ(box.Max.z, 10.0f);
}

TEST(BoundingBox, DegenerateZeroSizeBox)
{
    BoundingBox box(glm::vec3(5.0f), glm::vec3(5.0f));
    glm::vec3 center = box.GetCenter();
    glm::vec3 size = box.GetSize();

    EXPECT_EQ(center, glm::vec3(5.0f));
    EXPECT_EQ(size, glm::vec3(0.0f));
    ValidateVec3(center, "degenerate center");
    ValidateVec3(size, "degenerate size");
}

TEST(BoundingBox, ConstructFromZeroPoints)
{
    BoundingBox box(nullptr, 0);
    EXPECT_EQ(box.Min, glm::vec3(0.0f));
    EXPECT_EQ(box.Max, glm::vec3(0.0f));
}

// =============================================================================
// BoundingSphere Tests
// =============================================================================

TEST(BoundingSphere, ConstructFromCenterRadius)
{
    BoundingSphere sphere(glm::vec3(1.0f, 2.0f, 3.0f), 5.0f);
    EXPECT_EQ(sphere.Center, glm::vec3(1.0f, 2.0f, 3.0f));
    EXPECT_FLOAT_EQ(sphere.Radius, 5.0f);
}

TEST(BoundingSphere, ContainsCenter)
{
    BoundingSphere sphere(glm::vec3(10.0f, 20.0f, 30.0f), 1.0f);
    glm::vec3 expectedCenter(10.0f, 20.0f, 30.0f);
    EXPECT_EQ(sphere.Center, expectedCenter)
        << "Constructor should set Center correctly";
    f32 distFromExpected = glm::length(sphere.Center - expectedCenter);
    EXPECT_LE(distFromExpected, sphere.Radius)
        << "Expected center should be contained in sphere";
}

TEST(BoundingSphere, ConstructFromBoundingBox)
{
    BoundingBox box(glm::vec3(-2.0f), glm::vec3(2.0f));
    BoundingSphere sphere(box);

    EXPECT_EQ(sphere.Center, box.GetCenter());
    EXPECT_FLOAT_EQ(sphere.Radius, glm::length(box.GetExtents()));
    ValidateVec3(sphere.Center, "sphere center from box");
}

TEST(BoundingSphere, ConstructFromPoints)
{
    glm::vec3 points[] = {
        { 0.0f, 0.0f, 0.0f },
        { 2.0f, 0.0f, 0.0f },
        { 0.0f, 2.0f, 0.0f },
        { 0.0f, 0.0f, 2.0f }
    };
    BoundingSphere sphere(points, 4);

    // Center should be the average
    glm::vec3 expectedCenter(0.5f, 0.5f, 0.5f);
    EXPECT_NEAR(sphere.Center.x, expectedCenter.x, 1e-5f);
    EXPECT_NEAR(sphere.Center.y, expectedCenter.y, 1e-5f);
    EXPECT_NEAR(sphere.Center.z, expectedCenter.z, 1e-5f);

    // Radius should be max distance from center to any point
    for (u32 i = 0; i < 4; ++i)
    {
        f32 dist = glm::length(points[i] - sphere.Center);
        EXPECT_LE(dist, sphere.Radius + 1e-5f)
            << "Point " << i << " outside constructed sphere";
    }
}

TEST(BoundingSphere, DegenerateZeroRadius)
{
    BoundingSphere sphere(glm::vec3(1.0f, 2.0f, 3.0f), 0.0f);
    EXPECT_FLOAT_EQ(sphere.Radius, 0.0f);
    ValidateVec3(sphere.Center, "zero-radius center");
}

TEST(BoundingSphere, TransformPreservesContainment)
{
    BoundingSphere sphere(glm::vec3(0.0f), 2.0f);

    // Apply uniform scale + translation
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    transform = glm::scale(transform, glm::vec3(2.0f));

    BoundingSphere transformed = sphere.Transform(transform);

    // The transformed center should be at (10, 0, 0)
    EXPECT_NEAR(transformed.Center.x, 10.0f, 1e-4f);
    EXPECT_NEAR(transformed.Center.y, 0.0f, 1e-4f);
    EXPECT_NEAR(transformed.Center.z, 0.0f, 1e-4f);

    // Radius should be scaled (with 1.05x safety margin from the implementation)
    EXPECT_GE(transformed.Radius, 2.0f * 2.0f) << "Transformed radius should be at least scale * original";
    ValidateVec3(transformed.Center, "transformed sphere center");
}

TEST(BoundingSphere, ConstructFromZeroPoints)
{
    BoundingSphere sphere(nullptr, 0);
    EXPECT_EQ(sphere.Center, glm::vec3(0.0f));
    EXPECT_FLOAT_EQ(sphere.Radius, 0.0f);
}
