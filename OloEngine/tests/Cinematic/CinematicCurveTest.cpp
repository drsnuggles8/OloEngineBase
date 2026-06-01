#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Cinematic/CinematicCurve.h"
#include "OloEngine/Cinematic/CinematicTrack.h"

#include <glm/gtc/quaternion.hpp>

// =============================================================================
// CinematicCurveTest — unit / property tests for the keyframe sampling math
// that underpins every cinematic track. Pure CPU contract: empty-channel
// fallback, endpoint clamping, the three interpolation modes, quaternion
// slerp, degenerate-segment safety, and discrete visibility step semantics.
// =============================================================================

using namespace OloEngine;

namespace
{
    constexpr f32 kEps = 1e-5f;

    CinematicFloatChannel MakeFloatChannel()
    {
        CinematicFloatChannel ch;
        ch.Keys.push_back({ 0.0f, 0.0f, CinematicInterp::Linear });
        ch.Keys.push_back({ 2.0f, 10.0f, CinematicInterp::Linear });
        return ch;
    }
} // namespace

TEST(CinematicCurveTest, EmptyFloatChannelReturnsFallback)
{
    CinematicFloatChannel ch;
    EXPECT_FALSE(ch.HasKeys());
    EXPECT_NEAR(ch.Evaluate(1.0f, 42.0f), 42.0f, kEps);
}

TEST(CinematicCurveTest, FloatChannelClampsOutsideRange)
{
    const CinematicFloatChannel ch = MakeFloatChannel();
    // Before the first key holds the first value; after the last holds the last.
    EXPECT_NEAR(ch.Evaluate(-5.0f), 0.0f, kEps);
    EXPECT_NEAR(ch.Evaluate(99.0f), 10.0f, kEps);
    EXPECT_NEAR(ch.Evaluate(0.0f), 0.0f, kEps);
    EXPECT_NEAR(ch.Evaluate(2.0f), 10.0f, kEps);
}

TEST(CinematicCurveTest, FloatChannelLinearMidpoint)
{
    const CinematicFloatChannel ch = MakeFloatChannel();
    EXPECT_NEAR(ch.Evaluate(1.0f), 5.0f, kEps); // halfway
    EXPECT_NEAR(ch.Evaluate(0.5f), 2.5f, kEps); // quarter
    EXPECT_NEAR(ch.Evaluate(1.5f), 7.5f, kEps); // three-quarter
}

TEST(CinematicCurveTest, FloatChannelConstantHoldsLeftKey)
{
    CinematicFloatChannel ch;
    ch.Keys.push_back({ 0.0f, 3.0f, CinematicInterp::Constant });
    ch.Keys.push_back({ 1.0f, 9.0f, CinematicInterp::Linear });
    // Anywhere inside the first segment holds the left value (step), then snaps.
    EXPECT_NEAR(ch.Evaluate(0.0f), 3.0f, kEps);
    EXPECT_NEAR(ch.Evaluate(0.5f), 3.0f, kEps);
    EXPECT_NEAR(ch.Evaluate(0.999f), 3.0f, kEps);
    EXPECT_NEAR(ch.Evaluate(1.0f), 9.0f, kEps);
}

TEST(CinematicCurveTest, FloatChannelEaseInOutIsSmoothstep)
{
    CinematicFloatChannel ch;
    ch.Keys.push_back({ 0.0f, 0.0f, CinematicInterp::EaseInOut });
    ch.Keys.push_back({ 1.0f, 1.0f, CinematicInterp::Linear });
    // smoothstep(0.5) == 0.5, but the curve eases at the ends:
    EXPECT_NEAR(ch.Evaluate(0.5f), 0.5f, kEps);
    // smoothstep(0.25) = 0.15625, slower than linear's 0.25.
    EXPECT_NEAR(ch.Evaluate(0.25f), 0.15625f, kEps);
    EXPECT_LT(ch.Evaluate(0.25f), 0.25f);
    // smoothstep(0.75) = 0.84375, faster than linear's 0.75.
    EXPECT_NEAR(ch.Evaluate(0.75f), 0.84375f, kEps);
    EXPECT_GT(ch.Evaluate(0.75f), 0.75f);
}

TEST(CinematicCurveTest, ApplyInterpContract)
{
    EXPECT_NEAR(CinematicCurve::ApplyInterp(CinematicInterp::Constant, 0.7f), 0.0f, kEps);
    EXPECT_NEAR(CinematicCurve::ApplyInterp(CinematicInterp::Linear, 0.7f), 0.7f, kEps);
    EXPECT_NEAR(CinematicCurve::ApplyInterp(CinematicInterp::EaseInOut, 0.0f), 0.0f, kEps);
    EXPECT_NEAR(CinematicCurve::ApplyInterp(CinematicInterp::EaseInOut, 1.0f), 1.0f, kEps);
}

TEST(CinematicCurveTest, Vec3ChannelLinearMidpoint)
{
    CinematicVec3Channel ch;
    ch.Keys.push_back({ 0.0f, glm::vec3(0.0f), CinematicInterp::Linear });
    ch.Keys.push_back({ 1.0f, glm::vec3(2.0f, 4.0f, 6.0f), CinematicInterp::Linear });
    const glm::vec3 mid = ch.Evaluate(0.5f);
    EXPECT_NEAR(mid.x, 1.0f, kEps);
    EXPECT_NEAR(mid.y, 2.0f, kEps);
    EXPECT_NEAR(mid.z, 3.0f, kEps);
}

TEST(CinematicCurveTest, Vec3ChannelDegenerateSegmentIsSafe)
{
    // Two keys at the same time must not divide by zero; the left value holds.
    CinematicVec3Channel ch;
    ch.Keys.push_back({ 1.0f, glm::vec3(5.0f), CinematicInterp::Linear });
    ch.Keys.push_back({ 1.0f, glm::vec3(9.0f), CinematicInterp::Linear });
    const glm::vec3 v = ch.Evaluate(1.0f);
    EXPECT_TRUE(std::isfinite(v.x));
    EXPECT_NEAR(v.x, 5.0f, kEps);
}

TEST(CinematicCurveTest, QuatChannelSlerpMidpointAndNormalization)
{
    CinematicQuatChannel ch;
    const glm::quat a = glm::angleAxis(0.0f, glm::vec3(0, 1, 0));
    const glm::quat b = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    ch.Keys.push_back({ 0.0f, a, CinematicInterp::Linear });
    ch.Keys.push_back({ 1.0f, b, CinematicInterp::Linear });

    const glm::quat mid = ch.Evaluate(0.5f);
    const glm::quat expected = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 1, 0));
    // Slerp halfway between 0 and 90 degrees == 45 degrees. Compare via dot
    // (sign-agnostic) — |dot| ~ 1 means equal rotation.
    EXPECT_NEAR(std::abs(glm::dot(mid, expected)), 1.0f, 1e-4f);
    EXPECT_NEAR(glm::length(mid), 1.0f, 1e-4f); // result stays unit
}

TEST(CinematicCurveTest, QuatChannelEmptyReturnsFallback)
{
    CinematicQuatChannel ch;
    const glm::quat fallback = glm::angleAxis(glm::radians(30.0f), glm::vec3(1, 0, 0));
    const glm::quat got = ch.Evaluate(1.0f, fallback);
    EXPECT_NEAR(std::abs(glm::dot(got, fallback)), 1.0f, 1e-4f);
}

TEST(CinematicCurveTest, VisibilityTrackStepSemantics)
{
    CinematicVisibilityTrack track;
    track.Keys.push_back({ 0.0f, false });
    track.Keys.push_back({ 1.0f, true });
    track.Keys.push_back({ 2.0f, false });

    EXPECT_FALSE(track.EvaluateAt(0.0f, true));
    EXPECT_FALSE(track.EvaluateAt(0.5f, true)); // latest key <= t is the t=0 false
    EXPECT_TRUE(track.EvaluateAt(1.0f, false));
    EXPECT_TRUE(track.EvaluateAt(1.9f, false));
    EXPECT_FALSE(track.EvaluateAt(2.0f, true));
    EXPECT_FALSE(track.EvaluateAt(100.0f, true));
}

TEST(CinematicCurveTest, VisibilityTrackBeforeFirstKeyUsesFallback)
{
    CinematicVisibilityTrack track;
    track.Keys.push_back({ 5.0f, false });
    EXPECT_TRUE(track.EvaluateAt(0.0f, true));  // before first key -> fallback
    EXPECT_FALSE(track.EvaluateAt(5.0f, true)); // at/after the key -> key value
}
