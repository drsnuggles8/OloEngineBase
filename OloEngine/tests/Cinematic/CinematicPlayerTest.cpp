#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Cinematic/CinematicPlayer.h"
#include "OloEngine/Cinematic/CinematicSequence.h"
#include "OloEngine/Core/Ref.h"

// =============================================================================
// CinematicPlayerTest — unit tests for the pure playback math: time advance
// (forward / reverse / clamp-on-end / finish-at-0 / loop-wrap both directions /
// zero-duration / zero-speed hold / forward-backward symmetry), half-open event
// window collection (ascending and the reverse descending mirror), and the
// composite Tick (t==0 firing, no double-fire, loop wrap firing tail+head
// forward and head+tail backward). No Scene involved.
//
// Reverse playback (negative PlaybackSpeed) mirrors forward: the playhead
// recedes toward 0, finishes at 0 (non-looping) / wraps 0 -> duration (looping),
// and events fire in descending crossing order. The half-open window flips from
// the forward (lowerExclusive, upperInclusive] to the reverse
// [lowerInclusive, upperExclusive) so an event lands on the new (lower) playhead
// and leaves the old (higher) one — preventing double-fire across a direction
// change exactly as the forward window does between consecutive forward steps.
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

TEST(CinematicPlayerTest, AdvanceTimeNegativeSpeedStepsBackward)
{
    const auto r = CinematicPlayer::AdvanceTime(2.0f, 0.5f, -3.0f, 10.0f, false);
    EXPECT_NEAR(r.NewTime, 0.5f, kEps); // 2.0 + 0.5*(-3.0): playhead recedes
    EXPECT_FALSE(r.Looped);
    EXPECT_FALSE(r.JustFinished);
}

TEST(CinematicPlayerTest, AdvanceTimeReverseFinishesAtZeroWhenNotLooping)
{
    // Step that overshoots 0 going backward clamps to 0 and finishes — the
    // mirror of the forward clamp-at-duration.
    const auto r = CinematicPlayer::AdvanceTime(0.1f, 0.5f, -1.0f, 5.0f, /*loop*/ false);
    EXPECT_NEAR(r.NewTime, 0.0f, kEps);
    EXPECT_FALSE(r.Looped);
    EXPECT_TRUE(r.JustFinished);
}

TEST(CinematicPlayerTest, AdvanceTimeReverseWrapsWhenLooping)
{
    // 0.2 - 0.5 = -0.3 wraps into (0,5] as 4.7 — mirror of the forward wrap.
    const auto r = CinematicPlayer::AdvanceTime(0.2f, 0.5f, -1.0f, 5.0f, /*loop*/ true);
    EXPECT_NEAR(r.NewTime, 4.7f, kEps);
    EXPECT_TRUE(r.Looped);
    EXPECT_FALSE(r.JustFinished);
}

TEST(CinematicPlayerTest, AdvanceTimeReverseWrapLandsOnDurationAtExactBoundary)
{
    // raw == 0 exactly while looping backward maps to `duration` (the start of
    // a backward lap), mirroring the forward "land exactly on 0" wrap.
    const auto r = CinematicPlayer::AdvanceTime(0.5f, 0.5f, -1.0f, 5.0f, /*loop*/ true);
    EXPECT_NEAR(r.NewTime, 5.0f, kEps);
    EXPECT_TRUE(r.Looped);
    EXPECT_EQ(r.LoopCount, 1u);
}

TEST(CinematicPlayerTest, AdvanceTimeZeroSpeedHolds)
{
    const auto r = CinematicPlayer::AdvanceTime(2.0f, 0.5f, 0.0f, 5.0f, /*loop*/ false);
    EXPECT_NEAR(r.NewTime, 2.0f, kEps); // speed 0 holds the playhead — no movement
    EXPECT_FALSE(r.Looped);
    EXPECT_FALSE(r.JustFinished);
}

TEST(CinematicPlayerTest, AdvanceTimeForwardBackwardSymmetry)
{
    // Advancing forward then backward over the same dt returns to the start
    // (no clamp/wrap involved). Proves reverse is the exact inverse of forward.
    constexpr f32 kStart = 2.0f;
    constexpr f32 kDt = 0.3f;
    constexpr f32 kDuration = 5.0f;
    const auto fwd = CinematicPlayer::AdvanceTime(kStart, kDt, 1.0f, kDuration, /*loop*/ false);
    const auto back = CinematicPlayer::AdvanceTime(fwd.NewTime, kDt, -1.0f, kDuration, /*loop*/ false);
    EXPECT_NEAR(back.NewTime, kStart, kEps);
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

// -------------------------- CollectEventsReverse -----------------------------

TEST(CinematicPlayerTest, CollectEventsReverseHalfOpenWindowDescending)
{
    auto seq = MakeEventSequence(); // start@0, mid@1, end@5

    std::vector<std::string> fired;
    // [lowerInclusive, upperExclusive) => [0.0, 5.0) includes "start" at 0.0
    // (inclusive low) and "mid" at 1.0, excludes "end" at 5.0 (exclusive high).
    // Reverse mirror of CollectEvents' (0.0, 5.0] which would do the opposite.
    CinematicPlayer::CollectEventsReverse(*seq, 0.0f, 5.0f, fired);
    ASSERT_EQ(fired.size(), 2u);
    EXPECT_EQ(fired[0], "mid"); // descending: 1.0 before 0.0
    EXPECT_EQ(fired[1], "start");

    fired.clear();
    // [1.0, 6.0) includes "mid" at 1.0 (inclusive low) and "end" at 5.0.
    CinematicPlayer::CollectEventsReverse(*seq, 1.0f, 6.0f, fired);
    ASSERT_EQ(fired.size(), 2u);
    EXPECT_EQ(fired[0], "end"); // descending
    EXPECT_EQ(fired[1], "mid");
}

TEST(CinematicPlayerTest, CollectEventsReverseOrdersByTimeAcrossTracks)
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
    CinematicPlayer::CollectEventsReverse(*seq, 0.0f, 5.0f, fired);
    ASSERT_EQ(fired.size(), 3u);
    EXPECT_EQ(fired[0], "late"); // descending across tracks
    EXPECT_EQ(fired[1], "middle");
    EXPECT_EQ(fired[2], "early");
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

TEST(CinematicPlayerTest, TickMultiLapFiresEachCompletedLapEvents)
{
    auto seq = MakeEventSequence(); // 5s; events start@0, mid@1, end@5

    // One huge step (dt=12s) wraps the 5s timeline twice and lands at t=2.
    // The old single-wrap logic dropped the middle lap's events entirely.
    const auto r = CinematicPlayer::Tick(*seq, 0.0f, -1.0f, 12.0f, 1.0f, /*loop*/ true);
    EXPECT_TRUE(r.Looped);
    EXPECT_NEAR(r.NewTime, 2.0f, kEps);

    const auto count = [&](const char* name)
    {
        u32 n = 0;
        for (const auto& e : r.FiredEvents)
            if (e == name)
                ++n;
        return n;
    };
    EXPECT_EQ(count("end"), 2u);   // one per completed lap
    EXPECT_EQ(count("mid"), 3u);   // lap 1, lap 2, and the final head (t=2 >= 1)
    EXPECT_EQ(count("start"), 3u); // t==0 re-fires at each lap head
}

// ---------------------------- Tick (reverse) ---------------------------------

TEST(CinematicPlayerTest, TickReverseFiresEventsInDescendingOrderAndFinishesAtZero)
{
    auto seq = MakeEventSequence(); // start@0, mid@1, end@5
    // From 4.0 backward with a dt big enough to overshoot 0: lands on 0 and
    // finishes. Crosses mid@1 then start@0 (descending); never reaches end@5.
    const auto r = CinematicPlayer::Tick(*seq, /*fromTime*/ 4.0f, /*eventFloor*/ 4.0f, 4.0f, -1.0f, /*loop*/ false);
    EXPECT_TRUE(r.JustFinished);
    EXPECT_NEAR(r.NewTime, 0.0f, kEps);
    ASSERT_EQ(r.FiredEvents.size(), 2u);
    EXPECT_EQ(r.FiredEvents[0], "mid"); // descending crossing order
    EXPECT_EQ(r.FiredEvents[1], "start");
}

TEST(CinematicPlayerTest, TickReverseDoesNotDoubleFireAcrossConsecutiveSteps)
{
    auto seq = MakeEventSequence();

    // Step 1: 1.5 -> 0.5 (floor 1.5): crosses "mid" at 1.0 exactly once.
    auto r1 = CinematicPlayer::Tick(*seq, 1.5f, 1.5f, 1.0f, -1.0f, /*loop*/ false);
    ASSERT_EQ(r1.FiredEvents.size(), 1u);
    EXPECT_EQ(r1.FiredEvents[0], "mid");

    // Step 2: 0.5 -> 0 (floor = previous NewTime 0.5): lands on 0 firing
    // "start", and does NOT re-fire "mid" (1.0 is outside [0, 0.5)).
    auto r2 = CinematicPlayer::Tick(*seq, r1.NewTime, /*floor*/ r1.NewTime, 1.0f, -1.0f, /*loop*/ false);
    EXPECT_TRUE(r2.JustFinished);
    ASSERT_EQ(r2.FiredEvents.size(), 1u);
    EXPECT_EQ(r2.FiredEvents[0], "start");
}

TEST(CinematicPlayerTest, TickReverseLoopWrapFiresHeadThenTail)
{
    auto seq = MakeEventSequence();
    // From 0.2, dt 0.5 -> raw -0.3 wraps to 4.7 with loop. Mirror of the
    // forward wrap: descend 0.2 -> 0 fires "start" (t=0, head of the lap we're
    // leaving), wrap, descend 5 -> 4.7 fires "end" (t=5, tail of the new lap).
    const auto r = CinematicPlayer::Tick(*seq, 0.2f, 0.2f, 0.5f, -1.0f, /*loop*/ true);
    EXPECT_TRUE(r.Looped);
    EXPECT_NEAR(r.NewTime, 4.7f, kEps);
    ASSERT_EQ(r.FiredEvents.size(), 2u);
    EXPECT_EQ(r.FiredEvents[0], "start");
    EXPECT_EQ(r.FiredEvents[1], "end");
}

TEST(CinematicPlayerTest, TickReverseMultiLapFiresEachCompletedLapEvents)
{
    auto seq = MakeEventSequence(); // 5s; events start@0, mid@1, end@5

    // One huge backward step (dt=12s) from 4.0 wraps the 5s timeline twice and
    // lands at t=2 (raw -8 -> fmod -3 -> +5 = 2). Mirror of the forward
    // multi-lap case: each completed lap re-fires the whole timeline.
    const auto r = CinematicPlayer::Tick(*seq, 4.0f, 4.0f, 12.0f, -1.0f, /*loop*/ true);
    EXPECT_TRUE(r.Looped);
    EXPECT_NEAR(r.NewTime, 2.0f, kEps);

    const auto count = [&](const char* name)
    {
        u32 n = 0;
        for (const auto& e : r.FiredEvents)
            if (e == name)
                ++n;
        return n;
    };
    // head 4.0->0 fires mid+start; one full middle lap fires end+mid+start;
    // tail 5->2 fires end. => end 2, mid 2, start 2.
    EXPECT_EQ(count("end"), 2u);
    EXPECT_EQ(count("mid"), 2u);
    EXPECT_EQ(count("start"), 2u);
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
