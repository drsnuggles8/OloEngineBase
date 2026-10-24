// OLO_TEST_LAYER: unit
//
// PickVolumesTest — the pure half of PickVolumesComponent (issue #1559): the
// ray/capsule broad phase, the ray/triangle narrow phase and the region map.
// The scene half (fitting, posing, choosing the nearest skin) is exercised on the
// real dogs by DogPlayEvidenceTest.

#include "OloEngine/Scene/PickVolumes.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace OloEngine;

TEST(PickVolumes, ARayEntersACapsuleAtItsSurface)
{
    f32 t = 0.0f;
    // A capsule along X through the origin, radius 0.5; a ray down from y = 2.
    ASSERT_TRUE(PickVolumes::RayCapsule({ 0.3f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
                                        { 1.0f, 0.0f, 0.0f }, 0.5f, 10.0f, t));
    EXPECT_NEAR(t, 1.5f, 1e-5f);
    // Past an end, the hemispherical cap: a ray down at x = 1.3 meets the cap's sphere.
    ASSERT_TRUE(PickVolumes::RayCapsule({ 1.3f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
                                        { 1.0f, 0.0f, 0.0f }, 0.5f, 10.0f, t));
    EXPECT_NEAR(t, 2.0f - std::sqrt(0.25f - 0.09f), 1e-5f);
    // A sphere (a == b), and a ray that starts inside it enters at 0.
    ASSERT_TRUE(PickVolumes::RayCapsule({ 0.0f, 0.1f, 0.0f }, { 1.0f, 0.0f, 0.0f }, {}, {}, 0.5f, 10.0f, t));
    EXPECT_FLOAT_EQ(t, 0.0f);
}

TEST(PickVolumes, ACapsuleARayMissesOrMeetsTooFarAwayIsNotHit)
{
    f32 t = 0.0f;
    EXPECT_FALSE(PickVolumes::RayCapsule({ 0.0f, 2.0f, 2.0f }, { 0.0f, -1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
                                         { 1.0f, 0.0f, 0.0f }, 0.5f, 10.0f, t))
        << "passes beside it";
    EXPECT_FALSE(PickVolumes::RayCapsule({ 0.0f, 2.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
                                         { 1.0f, 0.0f, 0.0f }, 0.5f, 10.0f, t))
        << "points away";
    EXPECT_FALSE(PickVolumes::RayCapsule({ 0.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
                                         { 1.0f, 0.0f, 0.0f }, 0.5f, 1.0f, t))
        << "the surface is past the ray's reach";
    EXPECT_FALSE(PickVolumes::RayCapsule({ 0.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, {}, {}, 0.0f, 10.0f, t))
        << "a zero radius is nothing to hit";
}

TEST(PickVolumes, ARayCrossesATriangleFromEitherSideAndMissesBesideIt)
{
    const glm::vec3 a{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 b{ 1.0f, 0.0f, 0.0f };
    const glm::vec3 c{ 0.0f, 0.0f, 1.0f };
    f32 t = 0.0f;
    ASSERT_TRUE(PickVolumes::RayTriangle({ 0.2f, 1.0f, 0.2f }, { 0.0f, -1.0f, 0.0f }, a, b, c, 10.0f, t));
    EXPECT_NEAR(t, 1.0f, 1e-6f);
    ASSERT_TRUE(PickVolumes::RayTriangle({ 0.2f, -2.0f, 0.2f }, { 0.0f, 1.0f, 0.0f }, a, b, c, 10.0f, t)) << "the back face";
    EXPECT_NEAR(t, 2.0f, 1e-6f);
    EXPECT_FALSE(PickVolumes::RayTriangle({ 0.8f, 1.0f, 0.8f }, { 0.0f, -1.0f, 0.0f }, a, b, c, 10.0f, t)) << "beyond the hypotenuse";
    EXPECT_FALSE(PickVolumes::RayTriangle({ 0.2f, 1.0f, 0.2f }, { 1.0f, 0.0f, 0.0f }, a, b, c, 10.0f, t)) << "parallel";
    EXPECT_FALSE(PickVolumes::RayTriangle({ 0.2f, 1.0f, 0.2f }, { 0.0f, 1.0f, 0.0f }, a, b, c, 10.0f, t)) << "behind the origin";
    EXPECT_FALSE(PickVolumes::RayTriangle({ 0.2f, 1.0f, 0.2f }, { 0.0f, -1.0f, 0.0f }, a, b, c, 0.5f, t)) << "past maxDistance";
}

TEST(PickVolumes, RegionsMapBonesToNamesAndIgnoreMalformedParts)
{
    const auto map = PickVolumes::ParseRegions(" Ears : ear_L_01, ear_R_01 ;Chest:spine_03,  ;: orphan; NoColon; Ears2: ear_L_01");
    ASSERT_EQ(map.size(), 3u);
    EXPECT_EQ(map.at("ear_L_01"), "Ears") << "a bone listed twice keeps its first region";
    EXPECT_EQ(map.at("ear_R_01"), "Ears");
    EXPECT_EQ(map.at("spine_03"), "Chest");
    EXPECT_FALSE(map.contains("orphan")) << "a region with no name contributes nothing";
    EXPECT_TRUE(PickVolumes::ParseRegions("").empty());
}
