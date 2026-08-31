#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1

// =============================================================================
// WaterWakeShapeTest — L1 property tests for the wake-SHAPE contract (#968).
//
// The contract is Renderer/Water/WaterWake.h, mirrored by
// include/WaterWakeCommon.glsl. These pin the CPU half headlessly, so they gate
// CI; WaterWakeParityTest pins that the GLSL half agrees, and needs a GPU.
//
// What these are for, specifically: every defect in this feature renders a
// perfectly plausible sea.
//
//   * a wake that never fades with distance    -> a ridge across open ocean;
//   * a footprint mask that never reaches 1    -> water through the deck;
//   * a mask that never reaches 0              -> a rectangle of dead calm;
//   * arms placed by a constant spread rate    -> a V that narrows with speed;
//   * an unbounded sum of hulls                -> a boat launched by its own wake.
//
// None of them is an error, a NaN or a crash, so the assertions have to be
// about SHAPE — where the height is, relative to where it is not — rather than
// about whether a number came back.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Water/WaterWake.h"
#include "OloEngine/Renderer/Water/WaterWakeSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace OloEngine;

namespace
{
    constexpr f32 kHalfBeam = 1.2f;
    constexpr f32 kHalfLength = 3.0f;
    constexpr f32 kSpeed = 8.0f;
    constexpr f32 kFlatten = 0.9f;

    /// A hull running dead straight along +Z at kSpeed, with a full arm history.
    /// Ages match BoatWakeSystem's sampling range.
    [[nodiscard]] WaterWakeHullDesc StraightRunner(glm::vec2 centre = { 0.0f, 0.0f }, f32 speed = kSpeed)
    {
        WaterWakeHullDesc desc;
        desc.m_CentreXZ = centre;
        desc.m_ForwardXZ = { 0.0f, 1.0f };
        desc.m_HalfBeam = kHalfBeam;
        desc.m_HalfLength = kHalfLength;
        desc.m_Speed = speed;
        desc.m_Gate = 1.0f;
        desc.m_ArmSampleCount = WaterWake::kMaxArmSamples;
        for (u32 i = 0; i < WaterWake::kMaxArmSamples; ++i)
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(WaterWake::kMaxArmSamples - 1u);
            const f32 age = 0.15f + t * (2.5f - 0.15f);
            WaterWakeArmSample& s = desc.m_Arms[i];
            // Travelling along +Z, so a pose `age` seconds ago is `speed * age`
            // BEHIND the current centre.
            s.m_CentreXZ = centre - glm::vec2(0.0f, speed * age);
            s.m_ForwardXZ = { 0.0f, 1.0f };
            s.m_AgeSeconds = age;
            s.m_Speed = speed;
            s.m_Gate = 1.0f;
        }
        return desc;
    }

    /// Submit `desc` into a freshly reset system and evaluate at `worldXZ`.
    [[nodiscard]] WaterWake::Sample EvaluateOne(const WaterWakeHullDesc& desc, glm::vec2 worldXZ,
                                                f32 heightScale = 1.0f, f32 vertexSpacing = 0.0f)
    {
        WaterWakeSystem::Reset();
        WaterWakeSystem::BeginFrame();
        EXPECT_TRUE(WaterWakeSystem::SubmitHull(desc));
        return WaterWake::Evaluate(WaterWakeSystem::GetHullData(), WaterWakeSystem::GetHullCount(),
                                   heightScale, kFlatten, worldXZ, vertexSpacing);
    }
} // namespace

/// Clears the process-wide records after each case.
///
/// `EvaluateOne` and the record-level tests below Reset() on the way IN, which
/// protects these tests from each other but leaves hulls standing for whatever
/// runs next in the binary. WaterWakeBuoyancyTest resets in its TearDown for the
/// same reason; these are free functions, so they get a fixture.
class WaterWakeShapeTest : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        WaterWakeSystem::Reset();
    }
};

// =============================================================================
// 1. Hull exclusion — the acceptance criterion "no water clipping through the
//    boat's deck or hull interior".
// =============================================================================

TEST_F(WaterWakeShapeTest, HullFootprintFullySuppressesTheOceanInsideAndNothingWellOutside)
{
    const WaterWakeHullDesc desc = StraightRunner();

    // Dead centre of the hull: full suppression, which is what keeps a crest
    // out of the cockpit.
    const WaterWake::Sample inside = EvaluateOne(desc, { 0.0f, 0.0f });
    EXPECT_NEAR(inside.m_Flatten, kFlatten, 1e-4f)
        << "the ocean is not fully suppressed under the hull — a crest can rise through the deck";

    // Four metres off the beam is far outside kFootprintFadeEnd * halfBeam
    // (1.62 m): the open sea must be completely untouched, or the wake reads as
    // the whole water plane having sagged rather than as a boat sitting in it.
    const WaterWake::Sample outside = EvaluateOne(desc, { 4.0f, 0.0f });
    EXPECT_FLOAT_EQ(outside.m_Flatten, 0.0f) << "the hull footprint reaches into open water";

    // NEGATIVE CONTROL for the two above: they would both pass on a mask that
    // is identically zero. The rim must carry an intermediate value, which is
    // also what makes the waterline a blend rather than a crease.
    const WaterWake::Sample rim = EvaluateOne(desc, { kHalfBeam * 1.15f, 0.0f });
    EXPECT_GT(rim.m_Flatten, 0.0f) << "no fade band at the hull edge — the waterline is a hard step";
    EXPECT_LT(rim.m_Flatten, kFlatten) << "the mask has not begun to fall by the middle of its fade band";
}

TEST_F(WaterWakeShapeTest, FootprintIsORIENTEDWithTheHullRatherThanAxisAligned)
{
    // A hull heading north-east. A point off its BEAM must be outside the
    // footprint while an equally distant point along its LENGTH is inside —
    // which is exactly what an axis-aligned box test gets wrong, and an
    // axis-aligned box happens to be right for the +Z-heading hull every other
    // case here uses.
    WaterWakeHullDesc desc = StraightRunner();
    const f32 invSqrt2 = 1.0f / std::sqrt(2.0f);
    desc.m_ForwardXZ = { invSqrt2, invSqrt2 };
    desc.m_ArmSampleCount = 0; // arms are irrelevant here and would only add height
    desc.m_Speed = 0.0f;
    desc.m_Gate = 0.0f;

    const f32 d = 2.0f; // between halfBeam*1.35 (1.62) and halfLength (3.0)
    const glm::vec2 alongHull = glm::vec2(invSqrt2, invSqrt2) * d;
    const glm::vec2 acrossHull = glm::vec2(-invSqrt2, invSqrt2) * d;

    EXPECT_NEAR(EvaluateOne(desc, alongHull).m_Flatten, kFlatten, 1e-4f)
        << "a point 2 m along a 3 m half-length hull is outside its footprint";
    EXPECT_FLOAT_EQ(EvaluateOne(desc, acrossHull).m_Flatten, 0.0f)
        << "a point 2 m off a 1.2 m half-beam hull is inside its footprint — the mask is not oriented";
}

// =============================================================================
// 2. The Kelvin V — arm geometry.
// =============================================================================

TEST_F(WaterWakeShapeTest, ArmRidgeSitsAtTheKelvinHalfAngleAndIsSpeedIndependent)
{
    // Walk down the trail behind the hull and find, at each along-track
    // distance, the lateral offset where the wake height peaks. The ratio of
    // the two is tan(half-angle), and the Kelvin result says it is 19.47 deg at
    // EVERY speed.
    //
    // Measured from the field itself rather than from ArmOffset, so this covers
    // the packing and the evaluator rather than only the formula — a record
    // packed with the port and starboard points swapped, or with the offset
    // applied along `forward` instead of `starboard`, still satisfies a test
    // written against ArmOffset alone.
    for (const f32 speed : { 4.0f, 8.0f, 16.0f })
    {
        const WaterWakeHullDesc desc = StraightRunner({ 0.0f, 0.0f }, speed);

        // Sample at an age comfortably inside the arm range so the polyline is
        // well defined at both ends of the bracket.
        const f32 age = 1.5f;
        const f32 behind = -speed * age; // world Z of that pose
        f32 bestLateral = 0.0f;
        f32 bestHeight = 0.0f;
        for (f32 x = 0.0f; x <= 20.0f; x += 0.02f)
        {
            const f32 h = EvaluateOne(desc, { x, behind }).m_Height;
            if (h > bestHeight)
            {
                bestHeight = h;
                bestLateral = x;
            }
        }

        ASSERT_GT(bestHeight, 0.0f) << "no arm ridge found at " << speed << " m/s";

        // The ridge centre is halfBeam + tan(19.47 deg) * distance run.
        const f32 alongTrack = speed * age;
        const f32 expected = kHalfBeam + WaterWake::kKelvinTanHalfAngle * alongTrack;
        // Loose, as the issue asks: the peak is found on a 2 cm grid over a
        // ridge that is metres wide, and the polyline chords the true curve.
        EXPECT_NEAR(bestLateral, expected, 0.5f)
            << "arm ridge at " << speed << " m/s is at " << bestLateral << " m, not " << expected << " m";

        // And state the angle itself, which is the thing the issue names.
        const f32 halfAngleDeg = std::atan2(bestLateral - kHalfBeam, alongTrack) * 180.0f / 3.14159265f;
        EXPECT_NEAR(halfAngleDeg, 19.47f, 3.0f)
            << "half-angle at " << speed << " m/s is " << halfAngleDeg << " deg";
    }
}

TEST_F(WaterWakeShapeTest, ArmsAreSymmetricAboutTheTrackAndAbsentOnIt)
{
    const WaterWakeHullDesc desc = StraightRunner();
    const f32 age = 1.5f;
    const f32 behind = -kSpeed * age;
    const f32 offset = kHalfBeam + WaterWake::kKelvinTanHalfAngle * kSpeed * age;

    const f32 starboard = EvaluateOne(desc, { offset, behind }).m_Height;
    const f32 port = EvaluateOne(desc, { -offset, behind }).m_Height;
    EXPECT_GT(starboard, 0.0f) << "no starboard arm";
    EXPECT_NEAR(starboard, port, 1e-4f) << "the V is not symmetric — one arm is placed differently";

    // Between the arms, well behind the stern trough's reach, the water is
    // undisturbed. This is the assertion that fails on a wake laid along the
    // track instead of either side of it — which looks like a wake from
    // directly astern and like a stripe from anywhere else.
    const f32 onTrack = EvaluateOne(desc, { 0.0f, behind }).m_Height;
    EXPECT_LT(std::abs(onTrack), starboard * 0.5f)
        << "the trail centre is as raised as the arms — the V has collapsed onto the track";
}

TEST_F(WaterWakeShapeTest, ArmsFollowACurvedHistoryRatherThanTheCurrentHeading)
{
    // An S-turn: the hull is heading +Z now, but its recent poses curve away to
    // starboard. The arms must follow THOSE poses. A wake laid from the current
    // heading would put the ridge behind the boat on the -Z axis; a wake laid
    // from history puts it wherever the boat actually went.
    WaterWakeHullDesc desc = StraightRunner();
    glm::vec2 pos{ 0.0f, 0.0f };
    for (u32 i = 0; i < WaterWake::kMaxArmSamples; ++i)
    {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(WaterWake::kMaxArmSamples - 1u);
        const f32 age = 0.15f + t * (2.5f - 0.15f);
        // Historical heading swings from +Z toward +X as we look further back.
        const f32 theta = t * 1.0f; // radians
        const glm::vec2 fwd{ std::sin(theta), std::cos(theta) };
        WaterWakeArmSample& s = desc.m_Arms[i];
        s.m_CentreXZ = -fwd * (kSpeed * age);
        s.m_ForwardXZ = fwd;
        s.m_AgeSeconds = age;
        s.m_Speed = kSpeed;
        s.m_Gate = 1.0f;
    }

    // Compare, at each historical age, the ARM POINT the curved history implies
    // against the one a straight-astern wake would have produced. Sampling
    // around the hull CENTRE instead would miss both: the arms are metres to
    // either side of it, so a box tight enough to be specific never reaches
    // them and a box wide enough to reach them stops discriminating.
    for (u32 i = 2; i < WaterWake::kMaxArmSamples; ++i)
    {
        const WaterWakeArmSample& sample = desc.m_Arms[i];
        const f32 offset = WaterWake::ArmOffset(kHalfBeam, sample.m_Speed, sample.m_AgeSeconds);
        const glm::vec2 starboard(-sample.m_ForwardXZ.y, sample.m_ForwardXZ.x);
        const glm::vec2 curvedArm = sample.m_CentreXZ + starboard * offset;
        // Where the same arm would be if the wake were laid behind the CURRENT
        // heading (+Z) instead of behind the path the hull took.
        const glm::vec2 straightArm{ offset, -kSpeed * sample.m_AgeSeconds };

        const f32 onCurve = EvaluateOne(desc, curvedArm).m_Height;
        const f32 onStraight = EvaluateOne(desc, straightArm).m_Height;

        EXPECT_GT(onCurve, 0.01f) << "no ridge on the path the hull actually took, at age "
                                  << sample.m_AgeSeconds << " s";
        EXPECT_GT(onCurve, onStraight * 4.0f)
            << "the ridge is as strong straight astern as on the curved path at age "
            << sample.m_AgeSeconds << " s (curve " << onCurve << " m, straight " << onStraight << " m)";
    }
}

// =============================================================================
// 3. Bow and stern.
// =============================================================================

TEST_F(WaterWakeShapeTest, BowRisesAndSternDipsAndBothVanishWhenStopped)
{
    const WaterWakeHullDesc moving = StraightRunner();

    const f32 bowY = kHalfLength;                         // the bow itself
    const f32 sternY = -kHalfLength - kHalfLength * 1.2f; // one stern reach behind
    EXPECT_GT(EvaluateOne(moving, { 0.0f, bowY }).m_Height, 0.05f) << "no bow rise under way";
    EXPECT_LT(EvaluateOne(moving, { 0.0f, sternY }).m_Height, -0.02f) << "no stern trough under way";

    // Stopped: the hull still displaces (the footprint still flattens the sea),
    // but nothing is being pushed, so the bow and stern features go to zero.
    // A bow bump that survives a stop is the tell for an amplitude that forgot
    // its speed gate, and it looks like the boat permanently squatting.
    WaterWakeHullDesc stopped = StraightRunner();
    stopped.m_Speed = 0.0f;
    stopped.m_Gate = 0.0f;
    for (auto& arm : stopped.m_Arms)
    {
        arm.m_Speed = 0.0f;
        arm.m_Gate = 0.0f;
    }
    EXPECT_NEAR(EvaluateOne(stopped, { 0.0f, bowY }).m_Height, 0.0f, 1e-5f)
        << "the bow bump survives a full stop";
    EXPECT_NEAR(EvaluateOne(stopped, { 0.0f, sternY }).m_Height, 0.0f, 1e-5f)
        << "the stern trough survives a full stop";
    EXPECT_NEAR(EvaluateOne(stopped, { 0.0f, 0.0f }).m_Flatten, kFlatten, 1e-4f)
        << "a stopped hull stopped keeping the sea out of itself";
}

TEST_F(WaterWakeShapeTest, TheSternTroughDoesNotCancelTheBowBumpUnderTheHull)
{
    // A trough centred ON the stern is symmetric in the along-hull coordinate,
    // so it digs the same depression forward under the hull and eats the bow
    // bump. The fix — centring it one reach BEHIND — is invisible in any test
    // that only samples the two extremes, so assert the bow is still net
    // POSITIVE at the bow with the trough present.
    const WaterWakeHullDesc desc = StraightRunner();
    for (f32 z = 0.0f; z <= kHalfLength; z += 0.25f)
    {
        EXPECT_GE(EvaluateOne(desc, { 0.0f, z }).m_Height, 0.0f)
            << "the surface is depressed at z = " << z << ", forward of amidships";
    }
}

// =============================================================================
// 4. Boundedness, disablement and the band limit.
// =============================================================================

TEST_F(WaterWakeShapeTest, TheHeightIsBoundedEvenWithEveryHullStackedOnOneSpot)
{
    // Four hulls at the same place, at the height scale's ceiling. Nothing about
    // this is realistic; the point is that the clamp exists, because this term
    // is ADDED to an already-choppy ocean and an unbounded sum is how water ends
    // up above the deck it was added to keep water off.
    WaterWakeSystem::Reset();
    WaterWakeSystem::BeginFrame();
    for (u32 i = 0; i < WaterWake::kMaxHulls; ++i)
        ASSERT_TRUE(WaterWakeSystem::SubmitHull(StraightRunner({ 0.0f, 0.0f }, 40.0f)));

    // NEGATIVE CONTROL: a cap only means something if the unclamped sum would
    // exceed it. Four hulls' bow bumps at the ceiling, times the maximum height
    // scale, must be over the limit for this to be testing anything.
    ASSERT_GT(4.0f * 0.45f * 4.0f, WaterWake::kMaxWakeHeightMetres)
        << "the fixture cannot reach the clamp — this test asserts nothing";

    for (f32 z = -25.0f; z <= 10.0f; z += 0.5f)
    {
        for (f32 x = -12.0f; x <= 12.0f; x += 0.5f)
        {
            const WaterWake::Sample s =
                WaterWake::Evaluate(WaterWakeSystem::GetHullData(), WaterWakeSystem::GetHullCount(),
                                    4.0f, kFlatten, { x, z }, 0.0f);
            ASSERT_LE(std::abs(s.m_Height), WaterWake::kMaxWakeHeightMetres + 1e-4f)
                << "unbounded wake height at (" << x << ", " << z << ")";
            ASSERT_TRUE(std::isfinite(s.m_Height));
            ASSERT_GE(s.m_Flatten, 0.0f);
            ASSERT_LE(s.m_Flatten, 1.0f);
        }
    }
}

TEST_F(WaterWakeShapeTest, AZeroHeightScaleDisablesTheFlattenToo)
{
    // The disabled state has to remove the footprint as well as the height. A
    // scale that only zeroed the height would leave a hull-shaped patch of dead
    // calm on the sea with no boat visibly making it — which reads as a
    // rendering artefact and is exactly the "publish the disabled state" rule
    // docs/agent-rules/persistent-world-space-fields.md section 6 is about.
    const WaterWakeHullDesc desc = StraightRunner();
    const WaterWake::Sample on = EvaluateOne(desc, { 0.0f, 0.0f }, 1.0f);
    ASSERT_GT(on.m_Flatten, 0.0f) << "the fixture never flattened anything";

    const WaterWake::Sample off = EvaluateOne(desc, { 0.0f, 0.0f }, 0.0f);
    EXPECT_FLOAT_EQ(off.m_Height, 0.0f);
    EXPECT_FLOAT_EQ(off.m_Flatten, 0.0f) << "a disabled wake still presses its hull into the sea";
}

TEST_F(WaterWakeShapeTest, TheBandLimitFadesFeaturesBelowTheMeshVertexSpacing)
{
    const WaterWakeHullDesc desc = StraightRunner();
    const f32 age = 1.5f;
    const f32 behind = -kSpeed * age;
    const f32 offset = kHalfBeam + WaterWake::kKelvinTanHalfAngle * kSpeed * age;

    // The arm radius at this age is ~0.96 m + 0.35 * 1.5 = ~1.5 m, so a mesh
    // whose vertices are 0.5 m apart resolves it fully and one at 10 m cannot
    // resolve it at all. The middle case is what proves the fade is continuous
    // rather than a switch — a switch pops the ridge in and out as the water
    // LODs, which looks like flickering geometry.
    const f32 fine = EvaluateOne(desc, { offset, behind }, 1.0f, 0.5f).m_Height;
    const f32 mid = EvaluateOne(desc, { offset, behind }, 1.0f, 2.5f).m_Height;
    const f32 coarse = EvaluateOne(desc, { offset, behind }, 1.0f, 10.0f).m_Height;

    ASSERT_GT(fine, 0.0f) << "the fixture has no ridge to band-limit";
    EXPECT_GT(fine, mid) << "a coarser mesh did not attenuate the ridge";
    EXPECT_GT(mid, coarse) << "the fade is a switch rather than a ramp";
    EXPECT_FLOAT_EQ(coarse, 0.0f) << "a sub-vertex ridge is still being displaced — it will alias";

    // And spacing 0 means "no mesh", not "infinitely coarse": physics passes 0
    // and must get the full signal.
    EXPECT_GE(EvaluateOne(desc, { offset, behind }, 1.0f, 0.0f).m_Height, fine)
        << "the physics path (vertexSpacing 0) is being band-limited";
}

// =============================================================================
// 5. The record itself — the validation boundary and the bounded array.
// =============================================================================

TEST_F(WaterWakeShapeTest, NonFiniteHullInputIsRejectedRatherThanRepaired)
{
    // WaterWake.h section 3: this is the ONE validation boundary, so that the
    // two evaluators can stay literal mirrors. A hull with a garbage pose is a
    // bug upstream; substituting a plausible one would hide it while still
    // drawing a wake somewhere wrong.
    WaterWakeSystem::Reset();
    WaterWakeSystem::BeginFrame();

    WaterWakeHullDesc nanCentre = StraightRunner();
    nanCentre.m_CentreXZ.x = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(WaterWakeSystem::SubmitHull(nanCentre));

    WaterWakeHullDesc infSpeed = StraightRunner();
    infSpeed.m_Speed = std::numeric_limits<f32>::infinity();
    EXPECT_FALSE(WaterWakeSystem::SubmitHull(infSpeed));

    // A hull pointing straight up has no horizontal heading, so there is no
    // direction to lay a wake along.
    WaterWakeHullDesc vertical = StraightRunner();
    vertical.m_ForwardXZ = { 0.0f, 0.0f };
    EXPECT_FALSE(WaterWakeSystem::SubmitHull(vertical));

    EXPECT_EQ(WaterWakeSystem::GetHullCount(), 0u) << "a rejected hull was packed anyway";

    // NEGATIVE CONTROL: the same fixture, unmangled, must be accepted — three
    // rejections prove nothing if SubmitHull rejects everything.
    EXPECT_TRUE(WaterWakeSystem::SubmitHull(StraightRunner()));
    EXPECT_EQ(WaterWakeSystem::GetHullCount(), 1u);
}

TEST_F(WaterWakeShapeTest, HullsPastTheCapAreDroppedAndCountedRatherThanEvicting)
{
    WaterWakeSystem::Reset();
    WaterWakeSystem::BeginFrame();
    for (u32 i = 0; i < WaterWake::kMaxHulls; ++i)
        ASSERT_TRUE(WaterWakeSystem::SubmitHull(StraightRunner({ static_cast<f32>(i) * 50.0f, 0.0f })));

    ASSERT_EQ(WaterWakeSystem::GetHullCount(), WaterWake::kMaxHulls)
        << "the array is not full — the drop assertions below are vacuous";

    EXPECT_FALSE(WaterWakeSystem::SubmitHull(StraightRunner({ 999.0f, 0.0f })));
    EXPECT_EQ(WaterWakeSystem::GetHullCount(), WaterWake::kMaxHulls) << "an over-cap hull evicted one";
    EXPECT_EQ(WaterWakeSystem::GetDroppedHullCount(), 1u);

    // First-come is what makes the visible wake independent of submission
    // interleaving; the first hull must still be at its own position.
    const WaterWake::Sample first =
        WaterWake::Evaluate(WaterWakeSystem::GetHullData(), WaterWakeSystem::GetHullCount(), 1.0f,
                            kFlatten, { 0.0f, 0.0f }, 0.0f);
    EXPECT_GT(first.m_Flatten, 0.0f) << "the first-submitted hull was displaced by a later one";
}

TEST_F(WaterWakeShapeTest, BeginFrameDropsLastFrameSHullsSoAStoppedProducerLeavesNothing)
{
    // The failure this prevents: a boat beaches, BoatWakeSystem stops submitting
    // for it, and its footprint stays pressed into the sea forever because
    // nothing overwrites the slot.
    WaterWakeSystem::Reset();
    WaterWakeSystem::BeginFrame();
    ASSERT_TRUE(WaterWakeSystem::SubmitHull(StraightRunner()));
    ASSERT_GT(WaterWake::Evaluate(WaterWakeSystem::GetHullData(), WaterWakeSystem::GetHullCount(), 1.0f,
                                  kFlatten, { 0.0f, 0.0f }, 0.0f)
                  .m_Flatten,
              0.0f);

    WaterWakeSystem::BeginFrame(); // the next tick, nobody submits
    EXPECT_EQ(WaterWakeSystem::GetHullCount(), 0u);
    const WaterWake::Sample after =
        WaterWake::Evaluate(WaterWakeSystem::GetHullData(), WaterWakeSystem::GetHullCount(), 1.0f,
                            kFlatten, { 0.0f, 0.0f }, 0.0f);
    EXPECT_FLOAT_EQ(after.m_Flatten, 0.0f) << "last tick's hull is still pressed into the sea";
    EXPECT_FLOAT_EQ(after.m_Height, 0.0f);
}

TEST_F(WaterWakeShapeTest, ABoundingCircleRejectionNeverClipsALiveFeature)
{
    // The bounding circle is a pure optimisation, so it must be conservative:
    // anywhere it rejects, the full evaluation must have produced nothing.
    // Getting it slightly too SMALL clips the far end of the wake off, which
    // reads as the trail simply ending rather than as a bug.
    const WaterWakeHullDesc desc = StraightRunner();
    WaterWakeSystem::Reset();
    WaterWakeSystem::BeginFrame();
    ASSERT_TRUE(WaterWakeSystem::SubmitHull(desc));

    const glm::vec4 bound = WaterWakeSystem::GetHullData()[WaterWake::kOffsetBound];
    const glm::vec2 boundCentre(bound.x, bound.y);
    const f32 boundRadius = bound.z;
    ASSERT_GT(boundRadius, 0.0f);

    u32 outsideWithSignal = 0;
    u32 insideWithSignal = 0;
    for (f32 z = -40.0f; z <= 20.0f; z += 0.5f)
    {
        for (f32 x = -25.0f; x <= 25.0f; x += 0.5f)
        {
            const glm::vec2 p{ x, z };
            const WaterWake::Sample s =
                WaterWake::Evaluate(WaterWakeSystem::GetHullData(), 1u, 1.0f, kFlatten, p, 0.0f);
            const bool hasSignal = std::abs(s.m_Height) > 1e-6f || s.m_Flatten > 1e-6f;
            if (!hasSignal)
                continue;
            if (glm::length(p - boundCentre) > boundRadius)
                ++outsideWithSignal;
            else
                ++insideWithSignal;
        }
    }
    // Every signal Evaluate produces is by definition inside the circle (the
    // rejection is the first thing it does), so what this actually pins is that
    // the circle is not so tight that the wake got clipped to nothing.
    EXPECT_EQ(outsideWithSignal, 0u);
    EXPECT_GT(insideWithSignal, 100u)
        << "the bounding circle admits almost nothing — the wake has been clipped away";
}
