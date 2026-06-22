#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/AI/Perception/PerceptionMath.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

// ============================================================================
// PerceptionMathTest — unit test for the sight-cone membership predicate that
// backs the AI sight-perception system (PerceptionMath::IsInSightCone).
//
// This is the cheapest layer that pins the range + field-of-view contract:
// a point is visible iff it is within range AND the angle between the look
// direction and the direction to the target is at most half the FOV. The
// full PerceptionSystem (entity iteration, team filtering, LOS raycast,
// blackboard write) is exercised on the Functional axis driving a real
// Scene::OnUpdateRuntime; here we isolate the geometry.
//
// Expected values are derived from the cone definition, not hand-tuned:
//   in-cone  <=>  dot(normalize(forward), normalize(target-eye))
//                     >= cos(radians(fov / 2))
// ============================================================================

using namespace OloEngine;

namespace
{
    // A direction `angleDeg` away from -Z forward, in the XZ plane.
    glm::vec3 DirAtAngleFromForward(f32 angleDeg)
    {
        const f32 a = glm::radians(angleDeg);
        return { std::sin(a), 0.0f, -std::cos(a) };
    }
} // namespace

TEST(PerceptionMathTest, TargetDirectlyAheadInRangeIsSeen)
{
    const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    const glm::vec3 target{ 0.0f, 0.0f, -5.0f };

    EXPECT_TRUE(PerceptionMath::IsInSightCone(eye, forward, target, 10.0f, 90.0f));
}

TEST(PerceptionMathTest, TargetBeyondRangeIsNotSeen)
{
    const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    const glm::vec3 target{ 0.0f, 0.0f, -5.0f };

    // Same direction (on-axis), but range shorter than the distance.
    EXPECT_FALSE(PerceptionMath::IsInSightCone(eye, forward, target, 4.0f, 90.0f));
}

TEST(PerceptionMathTest, RangeBoundaryIsInclusive)
{
    const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    const glm::vec3 target{ 0.0f, 0.0f, -5.0f };

    // distance == range → inside (the predicate uses distSq > range*range to reject).
    EXPECT_TRUE(PerceptionMath::IsInSightCone(eye, forward, target, 5.0f, 90.0f));
    // A hair beyond → rejected.
    EXPECT_FALSE(PerceptionMath::IsInSightCone(eye, forward, target, 4.999f, 90.0f));
}

TEST(PerceptionMathTest, TargetBehindIsNotSeen)
{
    const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    const glm::vec3 behind{ 0.0f, 0.0f, 5.0f }; // 180° from forward

    EXPECT_FALSE(PerceptionMath::IsInSightCone(eye, forward, behind, 10.0f, 90.0f));
}

TEST(PerceptionMathTest, FovHalfAngleGatesTheCone)
{
    const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    const f32 fov = 90.0f; // half-angle = 45°

    // 40° off-axis → inside the 45° half-angle.
    const glm::vec3 inside = 5.0f * DirAtAngleFromForward(40.0f);
    EXPECT_TRUE(PerceptionMath::IsInSightCone(eye, forward, inside, 10.0f, fov));

    // 50° off-axis → outside the 45° half-angle.
    const glm::vec3 outside = 5.0f * DirAtAngleFromForward(50.0f);
    EXPECT_FALSE(PerceptionMath::IsInSightCone(eye, forward, outside, 10.0f, fov));
}

TEST(PerceptionMathTest, NarrowFovRejectsModerateOffAxis)
{
    const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    const f32 fov = 10.0f; // half-angle = 5°

    EXPECT_TRUE(PerceptionMath::IsInSightCone(eye, forward, 5.0f * DirAtAngleFromForward(3.0f), 10.0f, fov));
    EXPECT_FALSE(PerceptionMath::IsInSightCone(eye, forward, 5.0f * DirAtAngleFromForward(8.0f), 10.0f, fov));
}

TEST(PerceptionMathTest, FullCircleFovSeesEvenBehind)
{
    const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    const glm::vec3 behind{ 0.0f, 0.0f, 5.0f };

    // fov 360 → half-angle 180 → cos = -1 → anything within range is in the cone.
    EXPECT_TRUE(PerceptionMath::IsInSightCone(eye, forward, behind, 10.0f, 360.0f));
    // ...but range still applies.
    EXPECT_FALSE(PerceptionMath::IsInSightCone(eye, forward, behind, 4.0f, 360.0f));
}

TEST(PerceptionMathTest, CoincidentTargetIsInside)
{
    const glm::vec3 eye{ 1.0f, 2.0f, 3.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };

    // Target on top of the eye → direction undefined → treated as inside.
    EXPECT_TRUE(PerceptionMath::IsInSightCone(eye, forward, eye, 10.0f, 1.0f));
}

TEST(PerceptionMathTest, ForwardNeedNotBeNormalized)
{
    const glm::vec3 eye{ 0.0f, 0.0f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -7.5f }; // un-normalized, still -Z
    const glm::vec3 target{ 0.0f, 0.0f, -5.0f };

    EXPECT_TRUE(PerceptionMath::IsInSightCone(eye, forward, target, 10.0f, 90.0f));
}

TEST(PerceptionMathTest, EyeOffsetShiftsTheConeOrigin)
{
    // Eye lifted to head height; a target on the ground straight ahead is still
    // inside a generous cone, confirming the predicate honours the eye origin.
    const glm::vec3 eye{ 0.0f, 1.7f, 0.0f };
    const glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    const glm::vec3 target{ 0.0f, 0.0f, -5.0f };

    EXPECT_TRUE(PerceptionMath::IsInSightCone(eye, forward, target, 10.0f, 120.0f));
}
