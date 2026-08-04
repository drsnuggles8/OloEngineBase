// OLO_TEST_LAYER: unit
// =============================================================================
// PlayerRigTest.cpp
//
// Unit tests for the reusable player + camera rig kernels (issue #645):
// PlayerRigSystem's look/movement math and CameraRigSystem's spring arm.
//
// These are the pure functions the two scheduler nodes are built from, pulled
// out precisely so the parts that CAN be tested without a window, a GL context
// or a Jolt world actually are. The seams they can't cover — that the input
// half lands before the physics kick, that the boom probe excludes the player's
// own capsule, that a wall between camera and target really does shorten the
// arm — are covered by the scheduler seam test and the Functional rig tests.
//
// Two properties here are worth more than they look:
//
//   * Frame-rate independence of the follow (SmoothTowards / AdvanceBoom).
//     A per-frame lerp factor is the classic wrong answer and passes every
//     single-step test; it only misbehaves when the frame rate changes, which
//     no assertion on one dt can see. So the assertions compare ONE step at dt
//     against TWO steps at dt/2 and demand they agree.
//
//   * Yaw wrap has no discontinuity. Spinning past ±180° must be a continuous
//     turn, not a 358° snap back — and TurnTowardsYaw must take the short way
//     round across the seam.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Gameplay/PlayerRig/PlayerRigComponents.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigPresets.h"
#include "OloEngine/Gameplay/PlayerRig/PlayerRigSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kEpsilon = 1e-4f;

        // The rig's canonical basis: -Z forward, +X right, +Y up.
        constexpr glm::vec3 kForward{ 0.0f, 0.0f, -1.0f };
        constexpr glm::vec3 kRight{ 1.0f, 0.0f, 0.0f };

        void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, f32 tolerance = kEpsilon)
        {
            EXPECT_NEAR(actual.x, expected.x, tolerance);
            EXPECT_NEAR(actual.y, expected.y, tolerance);
            EXPECT_NEAR(actual.z, expected.z, tolerance);
        }
    } // namespace

    // ── Look ─────────────────────────────────────────────────────────────────

    TEST(PlayerRigLook, YawWrapsIntoTheHalfOpenRangeWithNoDiscontinuity)
    {
        EXPECT_NEAR(PlayerRigSystem::WrapDegrees(0.0f), 0.0f, kEpsilon);
        // The range is the half-open [-180, 180), so both spellings of "half a
        // turn" collapse onto the same representative. Which endpoint is
        // included is arbitrary — they are the same direction — but callers and
        // tests must agree, so pin it.
        EXPECT_NEAR(PlayerRigSystem::WrapDegrees(180.0f), -180.0f, kEpsilon);
        EXPECT_NEAR(PlayerRigSystem::WrapDegrees(-180.0f), -180.0f, kEpsilon);
        EXPECT_NEAR(PlayerRigSystem::WrapDegrees(190.0f), -170.0f, kEpsilon);
        EXPECT_NEAR(PlayerRigSystem::WrapDegrees(-190.0f), 170.0f, kEpsilon);
        // Several turns in either direction must land in range, not accumulate.
        EXPECT_NEAR(PlayerRigSystem::WrapDegrees(720.0f + 45.0f), 45.0f, 1e-3f);
        EXPECT_NEAR(PlayerRigSystem::WrapDegrees(-720.0f - 45.0f), -45.0f, 1e-3f);
        // Non-finite input must not poison the angle.
        EXPECT_NEAR(PlayerRigSystem::WrapDegrees(std::numeric_limits<f32>::quiet_NaN()), 0.0f, kEpsilon);
    }

    TEST(PlayerRigLook, SpinningPastTheSeamIsContinuous)
    {
        PlayerRigComponent rig;
        rig.m_LookSensitivity = 1.0f; // 1 degree per unit, so the math is readable
        rig.m_YawDeg = 179.0f;

        // Turning LEFT (negative screen-x delta yaws positive) past +180 must
        // come out just below -180, i.e. the direction is unchanged. If the
        // implementation clamped instead of wrapping, this would stick at 180.
        PlayerRigSystem::ApplyLookDelta(rig, { -2.0f, 0.0f });
        EXPECT_NEAR(rig.m_YawDeg, -179.0f, kEpsilon);

        // The forward vector either side of the seam must differ by ~2°, not
        // by ~358°: that's what "no discontinuity" actually means visually.
        const glm::vec3 afterSeam = PlayerRigSystem::YawRotation(rig.m_YawDeg) * kForward;
        const glm::vec3 beforeSeam = PlayerRigSystem::YawRotation(179.0f) * kForward;
        const f32 angleBetween = glm::degrees(std::acos(std::clamp(glm::dot(afterSeam, beforeSeam), -1.0f, 1.0f)));
        EXPECT_NEAR(angleBetween, 2.0f, 0.05f);
    }

    TEST(PlayerRigLook, PitchClampsToTheAuthoredLimits)
    {
        PlayerRigComponent rig;
        rig.m_LookSensitivity = 1.0f;
        rig.m_MinPitchDeg = -80.0f;
        rig.m_MaxPitchDeg = 80.0f;

        // Screen +Y is down, so a negative delta.y looks UP.
        PlayerRigSystem::ApplyLookDelta(rig, { 0.0f, -30.0f });
        EXPECT_NEAR(rig.m_PitchDeg, 30.0f, kEpsilon);

        // Far past the pole in one go — the clamp must hold, not wrap (a
        // wrapped pitch would flip the camera upside-down at the zenith).
        PlayerRigSystem::ApplyLookDelta(rig, { 0.0f, -1000.0f });
        EXPECT_NEAR(rig.m_PitchDeg, 80.0f, kEpsilon);

        PlayerRigSystem::ApplyLookDelta(rig, { 0.0f, 1000.0f });
        EXPECT_NEAR(rig.m_PitchDeg, -80.0f, kEpsilon);

        // The look direction never becomes parallel to world up, which is what
        // the sub-90° limits exist to guarantee (a parallel look would make the
        // camera basis degenerate).
        const glm::vec3 forward = PlayerRigSystem::LookRotation(rig.m_YawDeg, rig.m_PitchDeg) * kForward;
        EXPECT_LT(std::abs(forward.y), 0.999f);
    }

    TEST(PlayerRigLook, InvertYFlipsOnlyThePitchAxis)
    {
        PlayerRigComponent normal;
        normal.m_LookSensitivity = 1.0f;
        PlayerRigComponent inverted = normal;
        inverted.m_InvertLookY = true;

        PlayerRigSystem::ApplyLookDelta(normal, { -10.0f, -10.0f });
        PlayerRigSystem::ApplyLookDelta(inverted, { -10.0f, -10.0f });

        EXPECT_NEAR(normal.m_YawDeg, inverted.m_YawDeg, kEpsilon) << "invert-Y must not touch yaw";
        EXPECT_NEAR(normal.m_PitchDeg, -inverted.m_PitchDeg, kEpsilon);
    }

    TEST(PlayerRigLook, LookDeltaIsADisplacementNotARate)
    {
        // Two half-deltas must produce exactly the same angle as one whole
        // delta. This is the property that makes a mouse consumed once per
        // FRAME behave identically however many fixed sim ticks that frame
        // runs — and it is why the delta is never multiplied by dt.
        PlayerRigComponent oneStep;
        oneStep.m_LookSensitivity = 0.15f;
        PlayerRigComponent twoSteps = oneStep;

        PlayerRigSystem::ApplyLookDelta(oneStep, { -40.0f, -20.0f });
        PlayerRigSystem::ApplyLookDelta(twoSteps, { -20.0f, -10.0f });
        PlayerRigSystem::ApplyLookDelta(twoSteps, { -20.0f, -10.0f });

        EXPECT_NEAR(oneStep.m_YawDeg, twoSteps.m_YawDeg, kEpsilon);
        EXPECT_NEAR(oneStep.m_PitchDeg, twoSteps.m_PitchDeg, kEpsilon);
    }

    TEST(PlayerRigLook, NonFiniteDeltaIsIgnoredRatherThanPoisoningTheAngles)
    {
        PlayerRigComponent rig;
        rig.m_YawDeg = 33.0f;
        rig.m_PitchDeg = -7.0f;

        PlayerRigSystem::ApplyLookDelta(rig, { std::numeric_limits<f32>::quiet_NaN(), 0.0f });
        PlayerRigSystem::ApplyLookDelta(rig, { 0.0f, std::numeric_limits<f32>::infinity() });

        EXPECT_NEAR(rig.m_YawDeg, 33.0f, kEpsilon);
        EXPECT_NEAR(rig.m_PitchDeg, -7.0f, kEpsilon);
    }

    // ── Movement basis ───────────────────────────────────────────────────────

    TEST(PlayerRigMovement, WishDirectionUsesTheEngineForwardRightBasis)
    {
        ExpectVec3Near(PlayerRigSystem::WishDirection({ 0.0f, 1.0f }, 0.0f), kForward);
        ExpectVec3Near(PlayerRigSystem::WishDirection({ 0.0f, -1.0f }, 0.0f), -kForward);
        ExpectVec3Near(PlayerRigSystem::WishDirection({ 1.0f, 0.0f }, 0.0f), kRight);
        ExpectVec3Near(PlayerRigSystem::WishDirection({ -1.0f, 0.0f }, 0.0f), -kRight);
    }

    TEST(PlayerRigMovement, WishDirectionRotatesWithYawAndStaysHorizontal)
    {
        // Yawing 90° must turn "forward" into the direction that was "left".
        const glm::vec3 forwardAt90 = PlayerRigSystem::WishDirection({ 0.0f, 1.0f }, 90.0f);
        ExpectVec3Near(forwardAt90, -kRight);

        // The wish direction is always planar, whatever the yaw — vertical
        // movement is gravity's and the jump's business, never the stick's.
        for (f32 yaw = -180.0f; yaw <= 180.0f; yaw += 37.0f)
        {
            const glm::vec3 dir = PlayerRigSystem::WishDirection({ 0.6f, 0.8f }, yaw);
            EXPECT_NEAR(dir.y, 0.0f, kEpsilon) << "yaw=" << yaw;
            EXPECT_NEAR(glm::length(dir), 1.0f, kEpsilon) << "yaw=" << yaw;
        }
    }

    TEST(PlayerRigMovement, DiagonalIsNotFasterThanStraightButAnalogueInputStillScales)
    {
        // Full deflection on both axes: direction normalised, magnitude 1 —
        // so a diagonal walk is exactly walk speed, not walk speed × √2.
        EXPECT_NEAR(glm::length(PlayerRigSystem::WishDirection({ 1.0f, 1.0f }, 0.0f)), 1.0f, kEpsilon);
        EXPECT_NEAR(PlayerRigSystem::MoveMagnitude({ 1.0f, 1.0f }), 1.0f, kEpsilon);
        EXPECT_NEAR(PlayerRigSystem::MoveMagnitude({ 0.0f, 1.0f }), 1.0f, kEpsilon);

        // A half-deflected analogue stick still walks at half speed — the clamp
        // is to a maximum, not a normalisation.
        EXPECT_NEAR(PlayerRigSystem::MoveMagnitude({ 0.0f, 0.5f }), 0.5f, kEpsilon);
        EXPECT_NEAR(PlayerRigSystem::MoveMagnitude({ 0.0f, 0.0f }), 0.0f, kEpsilon);
    }

    TEST(PlayerRigMovement, ZeroAndNonFiniteInputProduceNoDirection)
    {
        ExpectVec3Near(PlayerRigSystem::WishDirection({ 0.0f, 0.0f }, 45.0f), glm::vec3(0.0f));
        ExpectVec3Near(
            PlayerRigSystem::WishDirection({ std::numeric_limits<f32>::quiet_NaN(), 1.0f }, 45.0f),
            glm::vec3(0.0f));
        EXPECT_NEAR(PlayerRigSystem::MoveMagnitude({ std::numeric_limits<f32>::infinity(), 0.0f }), 0.0f, kEpsilon);
    }

    TEST(PlayerRigMovement, TurnTowardsYawTakesTheShortWayRoundAcrossTheSeam)
    {
        // 179° -> -179° is a 2° turn, not 358°. At 720 deg/s and dt = 1/60 the
        // step budget is 12°, so a correct implementation arrives immediately.
        EXPECT_NEAR(PlayerRigSystem::TurnTowardsYaw(179.0f, -179.0f, 720.0f, 1.0f / 60.0f), -179.0f, kEpsilon);

        // Rate limiting: 90° of error with a 12° budget advances 12°.
        EXPECT_NEAR(PlayerRigSystem::TurnTowardsYaw(0.0f, 90.0f, 720.0f, 1.0f / 60.0f), 12.0f, kEpsilon);
        EXPECT_NEAR(PlayerRigSystem::TurnTowardsYaw(0.0f, -90.0f, 720.0f, 1.0f / 60.0f), -12.0f, kEpsilon);

        // A zero turn rate freezes the facing rather than snapping it.
        EXPECT_NEAR(PlayerRigSystem::TurnTowardsYaw(10.0f, 90.0f, 0.0f, 1.0f / 60.0f), 10.0f, kEpsilon);
    }

    TEST(PlayerRigMovement, TurnTowardsYawIsFrameRateIndependent)
    {
        constexpr f32 kDt = 1.0f / 30.0f;
        constexpr f32 kRate = 180.0f;

        const f32 oneStep = PlayerRigSystem::TurnTowardsYaw(0.0f, 170.0f, kRate, kDt);
        f32 twoHalfSteps = PlayerRigSystem::TurnTowardsYaw(0.0f, 170.0f, kRate, kDt * 0.5f);
        twoHalfSteps = PlayerRigSystem::TurnTowardsYaw(twoHalfSteps, 170.0f, kRate, kDt * 0.5f);

        EXPECT_NEAR(oneStep, twoHalfSteps, kEpsilon);
    }

    // ── Spring arm ───────────────────────────────────────────────────────────

    TEST(CameraRigBoom, ClearanceBacksOffFromTheHitAndRespectsBothBounds)
    {
        // Hit 3 m along a 4 m boom with a 0.25 m probe: stop 2.75 m out.
        EXPECT_NEAR(CameraRigSystem::ClearanceFromHit(3.0f, 4.0f, 0.25f, 0.6f), 2.75f, kEpsilon);

        // A hit right on top of the pivot floors at the minimum rather than
        // collapsing the camera into the character's head.
        EXPECT_NEAR(CameraRigSystem::ClearanceFromHit(0.1f, 4.0f, 0.25f, 0.6f), 0.6f, kEpsilon);

        // A hit beyond the boom can never EXTEND it past what was authored.
        EXPECT_NEAR(CameraRigSystem::ClearanceFromHit(50.0f, 4.0f, 0.25f, 0.6f), 4.0f, kEpsilon);

        // …and the floor never overrides the authored boom either, so a
        // first-person rig (boom 0) stays at 0 whatever the minimum says.
        EXPECT_NEAR(CameraRigSystem::ClearanceFromHit(0.05f, 0.0f, 0.25f, 0.6f), 0.0f, kEpsilon);
    }

    TEST(CameraRigBoom, ShorteningIsInstantAndLengtheningIsRateLimited)
    {
        constexpr f32 kDt = 1.0f / 60.0f;

        // Pull IN: instant. Easing here would leave the camera inside the wall
        // for those frames, which is the one artefact the probe exists to stop.
        EXPECT_NEAR(CameraRigSystem::AdvanceBoom(4.0f, 1.0f, 6.0f, kDt), 1.0f, kEpsilon);

        // Push OUT: rate limited to 6 m/s => 0.1 m per 1/60 s tick.
        EXPECT_NEAR(CameraRigSystem::AdvanceBoom(1.0f, 4.0f, 6.0f, kDt), 1.1f, kEpsilon);

        // Never overshoots the allowed length on the last tick of the return.
        EXPECT_NEAR(CameraRigSystem::AdvanceBoom(3.95f, 4.0f, 6.0f, kDt), 4.0f, kEpsilon);

        // A zero return speed makes extension instant too (documented escape
        // hatch), rather than freezing the arm at its pulled-in length forever.
        EXPECT_NEAR(CameraRigSystem::AdvanceBoom(1.0f, 4.0f, 0.0f, kDt), 4.0f, kEpsilon);
    }

    TEST(CameraRigBoom, ExtensionIsFrameRateIndependent)
    {
        constexpr f32 kDt = 1.0f / 30.0f;

        const f32 oneStep = CameraRigSystem::AdvanceBoom(1.0f, 4.0f, 6.0f, kDt);
        f32 twoHalfSteps = CameraRigSystem::AdvanceBoom(1.0f, 4.0f, 6.0f, kDt * 0.5f);
        twoHalfSteps = CameraRigSystem::AdvanceBoom(twoHalfSteps, 4.0f, 6.0f, kDt * 0.5f);

        EXPECT_NEAR(oneStep, twoHalfSteps, kEpsilon);
    }

    // ── Follow smoothing ─────────────────────────────────────────────────────

    TEST(CameraRigSmoothing, SameTotalDisplacementAtOneStepAndTwoHalfSteps)
    {
        // The headline frame-rate-independence property: exponential smoothing
        // composes exactly, so 1 x dt and 2 x (dt/2) land in the same place.
        // A naive `current + (target-current) * k` per frame does NOT, and
        // would sail through every single-step assertion in this file.
        constexpr glm::vec3 kStart{ 0.0f, 0.0f, 0.0f };
        constexpr glm::vec3 kTarget{ 10.0f, 4.0f, -6.0f };
        constexpr f32 kSmoothTime = 0.1f;
        constexpr f32 kDt = 1.0f / 30.0f;

        const glm::vec3 oneStep = CameraRigSystem::SmoothTowards(kStart, kTarget, kSmoothTime, kDt);
        glm::vec3 twoHalfSteps = CameraRigSystem::SmoothTowards(kStart, kTarget, kSmoothTime, kDt * 0.5f);
        twoHalfSteps = CameraRigSystem::SmoothTowards(twoHalfSteps, kTarget, kSmoothTime, kDt * 0.5f);

        ExpectVec3Near(twoHalfSteps, oneStep, 1e-4f);

        // Four quarter-steps too, so this is a real identity and not a lucky
        // coincidence at one subdivision.
        glm::vec3 fourQuarterSteps = kStart;
        for (int i = 0; i < 4; ++i)
            fourQuarterSteps = CameraRigSystem::SmoothTowards(fourQuarterSteps, kTarget, kSmoothTime, kDt * 0.25f);
        ExpectVec3Near(fourQuarterSteps, oneStep, 1e-4f);
    }

    TEST(CameraRigSmoothing, ZeroSmoothTimeSnapsAndSmoothingConvergesMonotonically)
    {
        constexpr glm::vec3 kStart{ 0.0f, 0.0f, 0.0f };
        constexpr glm::vec3 kTarget{ 5.0f, 0.0f, 0.0f };

        // Rigid mode — what a first-person rig uses so the view can't lag the head.
        ExpectVec3Near(CameraRigSystem::SmoothTowards(kStart, kTarget, 0.0f, 1.0f / 60.0f), kTarget);

        glm::vec3 position = kStart;
        f32 previousGap = glm::length(kTarget - position);
        for (int i = 0; i < 120; ++i)
        {
            position = CameraRigSystem::SmoothTowards(position, kTarget, 0.08f, 1.0f / 60.0f);
            const f32 gap = glm::length(kTarget - position);
            EXPECT_LE(gap, previousGap) << "smoothing must never move away from the target (i=" << i << ")";
            previousGap = gap;
        }
        EXPECT_LT(previousGap, 1e-3f) << "smoothing must actually converge, not asymptote short";
    }

    // ── The shipped templates ────────────────────────────────────────────────

    TEST(PlayerRigPresets, FirstPersonIsAZeroLengthBoomAndThirdPersonIsNot)
    {
        // The whole data model hangs off this: there is no mode enum, so if
        // the first-person preset ever grows a boom it silently becomes a
        // third-person rig with first-person tuning.
        const CameraRigComponent firstPerson = PlayerRigPresets::FirstPersonCamera();
        EXPECT_NEAR(firstPerson.m_BoomLength, 0.0f, kEpsilon);
        EXPECT_FALSE(firstPerson.m_CollisionEnabled) << "nothing to pull in at zero length";
        EXPECT_NEAR(firstPerson.m_PositionSmoothTime, 0.0f, kEpsilon)
            << "a first-person view must track the head 1:1 or it reads as input lag";

        const CameraRigComponent thirdPerson = PlayerRigPresets::ThirdPersonCamera();
        EXPECT_GT(thirdPerson.m_BoomLength, 0.0f);
        EXPECT_TRUE(thirdPerson.m_CollisionEnabled);
        EXPECT_GT(thirdPerson.m_MinBoomLength, 0.0f);
        EXPECT_LT(thirdPerson.m_MinBoomLength, thirdPerson.m_BoomLength);
    }

    TEST(PlayerRigPresets, TheTwoBodyFacingModesAreMutuallyExclusive)
    {
        // m_YawBodyWithLook is absolute and wins over m_FaceMoveDirection, so a
        // preset that set both would silently ignore one of them.
        const PlayerRigComponent firstPerson = PlayerRigPresets::FirstPersonPlayer();
        EXPECT_TRUE(firstPerson.m_YawBodyWithLook);
        EXPECT_FALSE(firstPerson.m_FaceMoveDirection);

        const PlayerRigComponent thirdPerson = PlayerRigPresets::ThirdPersonPlayer();
        EXPECT_FALSE(thirdPerson.m_YawBodyWithLook);
        EXPECT_TRUE(thirdPerson.m_FaceMoveDirection);
    }

    TEST(PlayerRigPresets, PitchLimitsStayInsideTheDegenerateBasisGuard)
    {
        for (const PlayerRigComponent& rig : { PlayerRigPresets::FirstPersonPlayer(),
                                               PlayerRigPresets::ThirdPersonPlayer() })
        {
            EXPECT_GT(rig.m_MinPitchDeg, -90.0f);
            EXPECT_LT(rig.m_MaxPitchDeg, 90.0f);
            EXPECT_LT(rig.m_MinPitchDeg, rig.m_MaxPitchDeg);
        }
    }
} // namespace OloEngine::Tests
