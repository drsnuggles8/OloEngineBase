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
    // Bezier has no tangents at this call site, so ApplyInterp degrades to Linear.
    EXPECT_NEAR(CinematicCurve::ApplyInterp(CinematicInterp::Bezier, 0.3f), 0.3f, kEps);
}

// ============================ Bezier tangent ease ============================
// BezierBlend is the pure 1D cubic-Hermite reparam of the segment parameter. Its
// two anchor identities (flat -> smoothstep, unit -> linear) are what let a Bezier
// key default to the old EaseInOut behaviour and only deviate once a handle moves.

TEST(CinematicCurveTest, BezierBlendEndpointsAreClamped01)
{
    // Regardless of the tangents, the segment must start at the left value (0)
    // and end at the right value (1) — otherwise keys wouldn't be honoured.
    for (const f32 mIn : { -3.0f, 0.0f, 1.0f, 4.0f })
    {
        for (const f32 mOut : { -3.0f, 0.0f, 1.0f, 4.0f })
        {
            EXPECT_NEAR(CinematicCurve::BezierBlend(0.0f, mOut, mIn), 0.0f, kEps);
            EXPECT_NEAR(CinematicCurve::BezierBlend(1.0f, mOut, mIn), 1.0f, kEps);
        }
    }
}

TEST(CinematicCurveTest, BezierBlendZeroTangentsIsSmoothstep)
{
    // Flat in/out tangents reproduce EaseInOut (smoothstep) exactly — the chosen
    // "neutral" default so a fresh Bezier key matches the prior behaviour.
    for (const f32 u : { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f })
    {
        EXPECT_NEAR(CinematicCurve::BezierBlend(u, 0.0f, 0.0f),
                    CinematicCurve::ApplyInterp(CinematicInterp::EaseInOut, u), kEps)
            << "u=" << u;
    }
}

TEST(CinematicCurveTest, BezierBlendUnitTangentsIsLinear)
{
    // Slopes of 1 at both ends mean the cubic collapses to the straight chord.
    for (const f32 u : { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f })
    {
        EXPECT_NEAR(CinematicCurve::BezierBlend(u, 1.0f, 1.0f), u, kEps) << "u=" << u;
    }
}

TEST(CinematicCurveTest, BezierBlendSteepOutTangentLeadsFaster)
{
    // A steeper out-tangent makes the segment cover ground faster early on than
    // the smoothstep default.
    EXPECT_GT(CinematicCurve::BezierBlend(0.25f, 3.0f, 0.0f),
              CinematicCurve::BezierBlend(0.25f, 0.0f, 0.0f));
    // A large out-tangent overshoots past the right value mid-segment (the point
    // of tangent handles — anticipation / overshoot for float & vec3 channels).
    EXPECT_GT(CinematicCurve::BezierBlend(0.6f, 4.0f, 0.0f), 1.0f);
}

TEST(CinematicCurveTest, FloatChannelBezierFlatTangentsMatchEaseInOut)
{
    CinematicFloatChannel bez;
    bez.Keys.push_back({ 0.0f, 0.0f, CinematicInterp::Bezier }); // tangents default 0
    bez.Keys.push_back({ 2.0f, 10.0f, CinematicInterp::Linear });

    CinematicFloatChannel ease;
    ease.Keys.push_back({ 0.0f, 0.0f, CinematicInterp::EaseInOut });
    ease.Keys.push_back({ 2.0f, 10.0f, CinematicInterp::Linear });

    for (const f32 t : { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f })
    {
        EXPECT_NEAR(bez.Evaluate(t), ease.Evaluate(t), kEps) << "t=" << t;
    }
}

TEST(CinematicCurveTest, FloatChannelBezierUnitTangentsMatchLinear)
{
    // Left key Bezier with out-tangent 1, right key in-tangent 1 => straight line.
    CinematicFloatChannel bez;
    bez.Keys.push_back({ 0.0f, 0.0f, CinematicInterp::Bezier, /*In*/ 0.0f, /*Out*/ 1.0f });
    bez.Keys.push_back({ 2.0f, 10.0f, CinematicInterp::Linear, /*In*/ 1.0f, /*Out*/ 0.0f });

    EXPECT_NEAR(bez.Evaluate(0.5f), 2.5f, kEps);
    EXPECT_NEAR(bez.Evaluate(1.0f), 5.0f, kEps);
    EXPECT_NEAR(bez.Evaluate(1.5f), 7.5f, kEps);
}

TEST(CinematicCurveTest, FloatChannelBezierUsesRightKeyInTangent)
{
    // The segment reads the *right* key's InTangent even when the right key's own
    // mode isn't Bezier — proving in/out are paired correctly across the segment.
    CinematicFloatChannel a;
    a.Keys.push_back({ 0.0f, 0.0f, CinematicInterp::Bezier, 0.0f, 0.0f }); // out = 0
    a.Keys.push_back({ 1.0f, 1.0f, CinematicInterp::Linear, 0.0f, 0.0f }); // in  = 0

    CinematicFloatChannel b = a;
    b.Keys[1].InTangent = 2.0f; // steepen arrival only

    // Changing the right key's in-tangent must change the sampled mid value.
    EXPECT_GT(std::abs(a.Evaluate(0.75f) - b.Evaluate(0.75f)), 1e-3f);
}

TEST(CinematicCurveTest, FloatChannelBezierOvershoots)
{
    // out-tangent 4 pushes the blend > 1 mid-segment, so the value exceeds the
    // right key's value (overshoot is allowed for scalar/vector channels).
    CinematicFloatChannel ch;
    ch.Keys.push_back({ 0.0f, 0.0f, CinematicInterp::Bezier, 0.0f, 4.0f });
    ch.Keys.push_back({ 1.0f, 10.0f, CinematicInterp::Linear, 0.0f, 0.0f });
    EXPECT_GT(ch.Evaluate(0.6f), 10.0f);
    // Endpoints are still honoured exactly.
    EXPECT_NEAR(ch.Evaluate(0.0f), 0.0f, kEps);
    EXPECT_NEAR(ch.Evaluate(1.0f), 10.0f, kEps);
}

TEST(CinematicCurveTest, Vec3ChannelBezierEasesUniformly)
{
    // Flat-tangent Bezier eases every component by the same smoothstep blend.
    CinematicVec3Channel ch;
    ch.Keys.push_back({ 0.0f, glm::vec3(0.0f), CinematicInterp::Bezier });
    ch.Keys.push_back({ 1.0f, glm::vec3(2.0f, 4.0f, 6.0f), CinematicInterp::Linear });

    const f32 blend = CinematicCurve::ApplyInterp(CinematicInterp::EaseInOut, 0.25f); // smoothstep(0.25)
    const glm::vec3 v = ch.Evaluate(0.25f);
    EXPECT_NEAR(v.x, 2.0f * blend, kEps);
    EXPECT_NEAR(v.y, 4.0f * blend, kEps);
    EXPECT_NEAR(v.z, 6.0f * blend, kEps);
}

TEST(CinematicCurveTest, QuatChannelBezierStaysFiniteUnitAndClamped)
{
    // Even with steep tangents, the quaternion ease is clamped to [0,1] before
    // slerp, so the result stays finite, unit, and never rotates past the
    // endpoints. Flat tangents must also match the EaseInOut slerp.
    CinematicQuatChannel bez;
    bez.Keys.push_back({ 0.0f, glm::angleAxis(0.0f, glm::vec3(0, 1, 0)), CinematicInterp::Bezier, 0.0f, 6.0f });
    bez.Keys.push_back({ 1.0f, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)), CinematicInterp::Linear, -4.0f, 0.0f });

    const glm::quat a = glm::angleAxis(0.0f, glm::vec3(0, 1, 0));
    const glm::quat b = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));
    for (const f32 t : { 0.0f, 0.2f, 0.5f, 0.8f, 1.0f })
    {
        const glm::quat q = bez.Evaluate(t);
        EXPECT_TRUE(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w)) << "t=" << t;
        EXPECT_NEAR(glm::length(q), 1.0f, 1e-4f) << "t=" << t;
        // Clamped blend => the rotation never leaves the [a, b] arc: its angle to
        // a never exceeds the full 90deg sweep (allowing a tiny epsilon).
        const f32 angToA = glm::degrees(glm::angle(glm::normalize(q * glm::inverse(a))));
        EXPECT_LE(angToA, 90.0f + 0.5f) << "t=" << t;
    }

    // Flat-tangent Bezier == EaseInOut slerp.
    CinematicQuatChannel flat;
    flat.Keys.push_back({ 0.0f, a, CinematicInterp::Bezier });
    flat.Keys.push_back({ 1.0f, b, CinematicInterp::Linear });
    CinematicQuatChannel ease;
    ease.Keys.push_back({ 0.0f, a, CinematicInterp::EaseInOut });
    ease.Keys.push_back({ 1.0f, b, CinematicInterp::Linear });
    for (const f32 t : { 0.25f, 0.5f, 0.75f })
    {
        EXPECT_NEAR(std::abs(glm::dot(flat.Evaluate(t), ease.Evaluate(t))), 1.0f, 1e-4f) << "t=" << t;
    }
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

TEST(CinematicCurveTest, QuatChannelDegenerateKeyDoesNotNaN)
{
    // A zero quaternion would make glm::normalize divide by zero and seed a NaN
    // that propagates into transforms. SafeNormalizeQuat must guard it.
    CinematicQuatChannel ch;
    ch.Keys.push_back({ 0.0f, glm::quat(0.0f, 0.0f, 0.0f, 0.0f), CinematicInterp::Linear });
    ch.Keys.push_back({ 1.0f, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)), CinematicInterp::Linear });

    for (const f32 t : { 0.0f, 0.5f, 1.0f })
    {
        const glm::quat q = ch.Evaluate(t);
        EXPECT_TRUE(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w))
            << "degenerate quat key produced a non-finite result at t=" << t;
    }
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
