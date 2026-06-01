#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Cinematic/CinematicPlayer.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Core/Ref.h"

// =============================================================================
// CinematicPlayerTest — unit tests for the pure playback math: time advance
// (normal / clamp-on-end / loop-wrap / zero-duration / non-negative speed),
// half-open event window collection, and the composite Tick (t==0 firing,
// no double-fire, loop wrap firing tail+head). No Scene involved.
// =============================================================================

using namespace OloEngine;

namespace
{
    constexpr f32 kEps = 1e-5f;

    // A 5-second sequence with explicit Duration and three timed events.
    Ref<CinematicSequence> MakeEventSequence()
    {
        auto seq = Ref<CinematicSequence>::Create();
        seq->Duration = 5.0f;

        CinematicEventTrack track;
        track.Name = "cues";
        track.Keys.push_back({ 0.0f, "start" });
        track.Keys.push_back({ 1.0f, "mid" });
        track.Keys.push_back({ 5.0f, "end" });
        seq->EventTracks.push_back(std::move(track));
        return seq;
    }
} // namespace

// ----------------------------- AdvanceTime -----------------------------------

TEST(CinematicPlayerTest, AdvanceTimeNormalStep)
{
    const auto r = CinematicPlayer::AdvanceTime(0.0f, 0.1f, 1.0f, 5.0f, /*loop*/ false);
    EXPECT_NEAR(r.NewTime, 0.1f, kEps);
    EXPECT_FALSE(r.Looped);
    EXPECT_FALSE(r.JustFinished);
}

TEST(CinematicPlayerTest, AdvanceTimeSpeedScales)
{
    const auto r = CinematicPlayer::AdvanceTime(1.0f, 0.5f, 2.0f, 10.0f, false);
    EXPECT_NEAR(r.NewTime, 2.0f, kEps); // 1.0 + 0.5*2.0
}

TEST(CinematicPlayerTest, AdvanceTimeNegativeSpeedHolds)
{
    const auto r = CinematicPlayer::AdvanceTime(2.0f, 0.5f, -3.0f, 10.0f, false);
    EXPECT_NEAR(r.NewTime, 2.0f, kEps); // reverse playback treated as hold (for now)
    EXPECT_FALSE(r.JustFinished);
}

TEST(CinematicPlayerTest, AdvanceTimeClampsAtEndWhenNotLooping)
{
    const auto r = CinematicPlayer::AdvanceTime(4.9f, 0.5f, 1.0f, 5.0f, false);
    EXPECT_NEAR(r.NewTime, 5.0f, kEps);
    EXPECT_FALSE(r.Looped);
    EXPECT_TRUE(r.JustFinished);
}

TEST(CinematicPlayerTest, AdvanceTimeWrapsWhenLooping)
{
    const auto r = CinematicPlayer::AdvanceTime(4.8f, 0.5f, 1.0f, 5.0f, /*loop*/ true);
    EXPECT_NEAR(r.NewTime, 0.3f, kEps); // 5.3 wrapped into [0,5)
    EXPECT_TRUE(r.Looped);
    EXPECT_FALSE(r.JustFinished);
}

TEST(CinematicPlayerTest, AdvanceTimeZeroDurationFinishesImmediately)
{
    const auto r = CinematicPlayer::AdvanceTime(0.0f, 0.1f, 1.0f, 0.0f, false);
    EXPECT_NEAR(r.NewTime, 0.0f, kEps);
    EXPECT_TRUE(r.JustFinished);
}

// ----------------------------- CollectEvents ---------------------------------

TEST(CinematicPlayerTest, CollectEventsHalfOpenWindow)
{
    auto seq = MakeEventSequence();

    std::vector<std::string> fired;
    // (lowerExclusive, upperInclusive] => (0.0, 1.0] includes "mid" at 1.0,
    // excludes "start" at 0.0.
    CinematicPlayer::CollectEvents(*seq, 0.0f, 1.0f, fired);
    ASSERT_EQ(fired.size(), 1u);
    EXPECT_EQ(fired[0], "mid");

    fired.clear();
    // (-1.0, 0.0] includes "start" at 0.0.
    CinematicPlayer::CollectEvents(*seq, -1.0f, 0.0f, fired);
    ASSERT_EQ(fired.size(), 1u);
    EXPECT_EQ(fired[0], "start");
}

TEST(CinematicPlayerTest, CollectEventsOrdersByTimeAcrossTracks)
{
    auto seq = Ref<CinematicSequence>::Create();
    seq->Duration = 10.0f;
    {
        CinematicEventTrack a;
        a.Keys.push_back({ 3.0f, "late" });
        a.Keys.push_back({ 1.0f, "early" });
        seq->EventTracks.push_back(std::move(a));
    }
    {
        CinematicEventTrack b;
        b.Keys.push_back({ 2.0f, "middle" });
        seq->EventTracks.push_back(std::move(b));
    }

    std::vector<std::string> fired;
    CinematicPlayer::CollectEvents(*seq, 0.0f, 5.0f, fired);
    ASSERT_EQ(fired.size(), 3u);
    EXPECT_EQ(fired[0], "early");
    EXPECT_EQ(fired[1], "middle");
    EXPECT_EQ(fired[2], "late");
}

// -------------------------------- Tick ---------------------------------------

TEST(CinematicPlayerTest, TickFiresZeroTimeEventOnFirstStep)
{
    auto seq = MakeEventSequence();
    // First tick: eventFloor = -1 sentinel so t==0 "start" fires.
    const auto r = CinematicPlayer::Tick(*seq, /*fromTime*/ 0.0f, /*eventFloor*/ -1.0f, 0.1f, 1.0f, false);
    ASSERT_EQ(r.FiredEvents.size(), 1u);
    EXPECT_EQ(r.FiredEvents[0], "start");
    EXPECT_NEAR(r.NewTime, 0.1f, kEps);
}

TEST(CinematicPlayerTest, TickDoesNotDoubleFireAcrossConsecutiveSteps)
{
    auto seq = MakeEventSequence();

    // Step 1: 0.0 -> 0.6 (floor -1): fires "start".
    auto r1 = CinematicPlayer::Tick(*seq, 0.0f, -1.0f, 0.6f, 1.0f, false);
    ASSERT_EQ(r1.FiredEvents.size(), 1u);
    EXPECT_EQ(r1.FiredEvents[0], "start");

    // Step 2: 0.6 -> 1.2 (floor = previous NewTime 0.6): fires "mid" at 1.0
    // exactly once, and does NOT re-fire "start".
    auto r2 = CinematicPlayer::Tick(*seq, r1.NewTime, /*floor*/ r1.NewTime, 0.6f, 1.0f, false);
    ASSERT_EQ(r2.FiredEvents.size(), 1u);
    EXPECT_EQ(r2.FiredEvents[0], "mid");
}

TEST(CinematicPlayerTest, TickClampFinishFiresFinalEvent)
{
    auto seq = MakeEventSequence();
    // From 4.6, floor 4.6, dt that overshoots 5.0 with no loop.
    const auto r = CinematicPlayer::Tick(*seq, 4.6f, 4.6f, 1.0f, 1.0f, /*loop*/ false);
    EXPECT_TRUE(r.JustFinished);
    EXPECT_NEAR(r.NewTime, 5.0f, kEps);
    ASSERT_EQ(r.FiredEvents.size(), 1u);
    EXPECT_EQ(r.FiredEvents[0], "end"); // event at t==5 fires on the clamp
}

TEST(CinematicPlayerTest, TickLoopWrapFiresTailThenHead)
{
    auto seq = MakeEventSequence();
    // From 4.8, floor 4.8, dt 0.5 -> raw 5.3 wraps to 0.3 with loop.
    // Should fire "end" (t=5, tail of old lap) then "start" (t=0, head of new).
    const auto r = CinematicPlayer::Tick(*seq, 4.8f, 4.8f, 0.5f, 1.0f, /*loop*/ true);
    EXPECT_TRUE(r.Looped);
    EXPECT_NEAR(r.NewTime, 0.3f, kEps);
    ASSERT_EQ(r.FiredEvents.size(), 2u);
    EXPECT_EQ(r.FiredEvents[0], "end");
    EXPECT_EQ(r.FiredEvents[1], "start");
}

TEST(CinematicPlayerTest, EffectiveDurationDerivesFromKeysWhenUnset)
{
    auto seq = Ref<CinematicSequence>::Create();
    seq->Duration = 0.0f; // not set -> derive
    CinematicEventTrack track;
    track.Keys.push_back({ 3.5f, "boom" });
    seq->EventTracks.push_back(std::move(track));

    EXPECT_NEAR(seq->ComputeDuration(), 3.5f, kEps);
    EXPECT_NEAR(seq->GetEffectiveDuration(), 3.5f, kEps);
}
