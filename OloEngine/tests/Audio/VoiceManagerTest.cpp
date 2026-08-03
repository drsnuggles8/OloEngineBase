// OLO_TEST_LAYER: unit
//
// Headless coverage of the concurrent-voice budget (issue #730, acceptance criterion 3).
//
// Everything here runs with no audio device and no miniaudio object: VoiceManager is a
// pure policy unit over VoiceParams, driven through the IVoiceHost callback interface.
// That separation is deliberate — a real AudioSource cannot be constructed in a headless
// unit test (it needs a live ma_engine), so a budget implemented inside AudioSource would
// have been untestable in CI.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Audio/VoiceManager.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

using OloEngine::Audio::IVoiceHost;
using OloEngine::Audio::kInvalidVoiceHandle;
using OloEngine::Audio::VoiceHandle;
using OloEngine::Audio::VoiceManager;
using OloEngine::Audio::VoiceParams;

namespace
{
    /// Recording stand-in for a backend voice. Counts the transitions the budget drove and
    /// remembers the position it was last asked to (re)start from — the assertion that
    /// separates "resumed" from "restarted".
    class FakeVoiceHost final : public IVoiceHost
    {
      public:
        explicit FakeVoiceHost(std::string name = {})
            : Name(std::move(name))
        {
        }

        bool OnVoiceStart(f64 positionSeconds) const override
        {
            ++StartCount;
            LastStartPosition = positionSeconds;
            Running = true;
            return !RefuseToStart;
        }

        f64 OnVoiceStop() const override
        {
            ++StopCount;
            Running = false;
            return ReportedStopPosition;
        }

        f64 OnVoiceQueryPosition() const override
        {
            return DevicePosition;
        }

        std::string Name;
        /// Simulates a backend that cannot start (device lost, decoder failure).
        bool RefuseToStart = false;
        /// What OnVoiceStop reports back; negative means "no transport", which makes the
        /// manager keep its own logical position.
        f64 ReportedStopPosition = -1.0;
        /// What OnVoiceQueryPosition reports while audible; negative means "not tracked".
        f64 DevicePosition = -1.0;

        mutable u32 StartCount = 0;
        mutable u32 StopCount = 0;
        mutable bool Running = false;
        mutable f64 LastStartPosition = -1.0;
    };

    VoiceParams MakeParams(f32 priority, f32 volume = 1.0f)
    {
        VoiceParams params;
        params.Priority = priority;
        params.Volume = volume;
        return params;
    }

    VoiceParams MakeSpatialParams(f32 priority, const glm::vec3& position, f32 maxDistance = 100.0f)
    {
        VoiceParams params;
        params.Priority = priority;
        params.Spatialized = true;
        params.Position = position;
        params.MinDistance = 1.0f;
        params.MaxDistance = maxDistance;
        return params;
    }
} // namespace

// ===========================================================================
// Scoring
// ===========================================================================

TEST(VoiceManagerScoreTest, ScoreIsPriorityTimesGainForA2DVoice)
{
    const glm::vec3 listener(0.0f);
    EXPECT_NEAR(VoiceManager::ComputeScore(MakeParams(1.0f, 1.0f), listener), 1.0f, 1e-5f);
    EXPECT_NEAR(VoiceManager::ComputeScore(MakeParams(0.5f, 1.0f), listener), 0.5f, 1e-5f);
    EXPECT_NEAR(VoiceManager::ComputeScore(MakeParams(1.0f, 0.25f), listener), 0.25f, 1e-5f);
}

TEST(VoiceManagerScoreTest, DistanceOnlyAttenuatesASpatializedVoice)
{
    const glm::vec3 listener(0.0f);

    VoiceParams flat = MakeParams(1.0f);
    flat.Position = glm::vec3(500.0f, 0.0f, 0.0f);
    // A 2D voice (music, UI) must not lose its slot because the entity carrying it happens
    // to sit far from the listener.
    EXPECT_NEAR(VoiceManager::ComputeScore(flat, listener), 1.0f, 1e-5f);

    // NB: not named `near` / `far` — those are still macros in the Windows SDK headers.
    const f32 nearScore = VoiceManager::ComputeScore(MakeSpatialParams(1.0f, glm::vec3(5.0f, 0.0f, 0.0f)), listener);
    const f32 farScore = VoiceManager::ComputeScore(MakeSpatialParams(1.0f, glm::vec3(50.0f, 0.0f, 0.0f)), listener);
    EXPECT_GT(nearScore, farScore);
    EXPECT_GT(farScore, 0.0f);
}

TEST(VoiceManagerScoreTest, BeyondMaxDistanceScoresZero)
{
    const glm::vec3 listener(0.0f);
    EXPECT_NEAR(VoiceManager::ComputeScore(MakeSpatialParams(1.0f, glm::vec3(1000.0f, 0.0f, 0.0f), 100.0f), listener), 0.0f, 1e-6f);
}

TEST(VoiceManagerScoreTest, NonFiniteInputsScoreZeroRatherThanPoisoningTheRanking)
{
    // A NaN score would make the ranking comparison non-transitive, which is UB for the
    // sort/selection the rebalance performs — and it would corrupt the whole audible set,
    // not just the offending voice.
    const glm::vec3 listener(0.0f);
    EXPECT_NEAR(VoiceManager::ComputeScore(MakeParams(std::numeric_limits<f32>::quiet_NaN()), listener), 0.0f, 1e-6f);
    EXPECT_NEAR(VoiceManager::ComputeScore(MakeParams(1.0f, std::numeric_limits<f32>::infinity()), listener), 0.0f, 1e-6f);

    VoiceParams nanPosition = MakeSpatialParams(1.0f, glm::vec3(std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f));
    EXPECT_NEAR(VoiceManager::ComputeScore(nanPosition, listener), 0.0f, 1e-6f);
}

// ===========================================================================
// The cap — acceptance criterion 1
// ===========================================================================

TEST(VoiceManagerBudgetTest, AudibleVoicesNeverExceedTheCap)
{
    VoiceManager manager;
    manager.SetMaxVoices(8);

    std::vector<FakeVoiceHost> hosts(200);
    std::vector<VoiceHandle> handles;
    handles.reserve(hosts.size());

    for (sizet i = 0; i < hosts.size(); ++i)
    {
        handles.push_back(manager.Acquire(&hosts[i], MakeParams(0.5f)));
        // Checked after EVERY acquire, not just at the end — a cap that only holds once
        // the dust settles is not a cap.
        EXPECT_LE(manager.GetStats().Playing, 8u) << "cap exceeded after acquiring voice " << i;
    }

    const auto stats = manager.GetStats();
    EXPECT_EQ(stats.Playing, 8u);
    EXPECT_EQ(stats.Virtual, 192u);

    // And the ones that lost are genuinely silent, not merely unranked.
    u32 running = 0;
    for (const auto& host : hosts)
    {
        if (host.Running)
        {
            ++running;
        }
    }
    EXPECT_EQ(running, 8u);
}

TEST(VoiceManagerBudgetTest, LoweringTheCapUnderAFullMixVirtualizesTheWorstVoices)
{
    VoiceManager manager;
    manager.SetMaxVoices(4);

    FakeVoiceHost hosts[4];
    VoiceHandle handles[4]{};
    const f32 priorities[4] = { 0.1f, 0.9f, 0.4f, 0.7f };
    for (sizet i = 0; i < 4; ++i)
    {
        handles[i] = manager.Acquire(&hosts[i], MakeParams(priorities[i]));
    }
    ASSERT_EQ(manager.GetStats().Playing, 4u);

    manager.SetMaxVoices(2);

    EXPECT_EQ(manager.GetStats().Playing, 2u);
    EXPECT_TRUE(manager.IsAudible(handles[1])); // 0.9
    EXPECT_TRUE(manager.IsAudible(handles[3])); // 0.7
    EXPECT_TRUE(manager.IsVirtual(handles[2])); // 0.4
    EXPECT_TRUE(manager.IsVirtual(handles[0])); // 0.1
}

TEST(VoiceManagerBudgetTest, CapOfZeroIsClampedToOneRatherThanSilencingEverything)
{
    VoiceManager manager;
    manager.SetMaxVoices(0);
    EXPECT_EQ(manager.GetMaxVoices(), 1u);
}

// ===========================================================================
// Stealing — acceptance criterion 1
// ===========================================================================

TEST(VoiceManagerStealingTest, TheNearestSpatializedVoicesKeepTheSlots)
{
    VoiceManager manager;
    manager.SetMaxVoices(3);
    manager.SetListenerPosition(glm::vec3(0.0f));

    // Deliberately triggered furthest-first, so passing this test cannot be an artifact of
    // arrival order.
    constexpr f32 kDistances[6] = { 90.0f, 75.0f, 60.0f, 40.0f, 20.0f, 5.0f };
    FakeVoiceHost hosts[6];
    VoiceHandle handles[6]{};
    for (sizet i = 0; i < 6; ++i)
    {
        handles[i] = manager.Acquire(&hosts[i], MakeSpatialParams(0.5f, glm::vec3(kDistances[i], 0.0f, 0.0f)));
    }

    EXPECT_EQ(manager.GetStats().Playing, 3u);
    EXPECT_TRUE(manager.IsAudible(handles[5])); // 5 m
    EXPECT_TRUE(manager.IsAudible(handles[4])); // 20 m
    EXPECT_TRUE(manager.IsAudible(handles[3])); // 40 m
    EXPECT_TRUE(manager.IsVirtual(handles[2]));
    EXPECT_TRUE(manager.IsVirtual(handles[1]));
    EXPECT_TRUE(manager.IsVirtual(handles[0]));
}

TEST(VoiceManagerStealingTest, AHigherPriorityVoiceStealsTheWorstAudibleSlot)
{
    VoiceManager manager;
    manager.SetMaxVoices(2);

    FakeVoiceHost quiet("quiet");
    FakeVoiceHost loud("loud");
    FakeVoiceHost important("important");

    const VoiceHandle quietVoice = manager.Acquire(&quiet, MakeParams(0.2f));
    const VoiceHandle loudVoice = manager.Acquire(&loud, MakeParams(0.8f));
    ASSERT_TRUE(manager.IsAudible(quietVoice));
    ASSERT_TRUE(manager.IsAudible(loudVoice));

    const VoiceHandle importantVoice = manager.Acquire(&important, MakeParams(0.95f));

    EXPECT_TRUE(manager.IsAudible(importantVoice));
    EXPECT_TRUE(manager.IsAudible(loudVoice));
    EXPECT_TRUE(manager.IsVirtual(quietVoice)) << "the lowest-priority voice should have yielded, not an arbitrary one";
    EXPECT_EQ(quiet.StopCount, 1u);
    EXPECT_EQ(loud.StopCount, 0u);
    EXPECT_EQ(manager.GetStats().Steals, 1u);
}

TEST(VoiceManagerStealingTest, ALowerScoringVoiceCannotTakeASlot)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost incumbent;
    FakeVoiceHost challenger;

    const VoiceHandle incumbentVoice = manager.Acquire(&incumbent, MakeParams(0.9f));
    const VoiceHandle challengerVoice = manager.Acquire(&challenger, MakeParams(0.1f));

    EXPECT_TRUE(manager.IsAudible(incumbentVoice));
    EXPECT_TRUE(manager.IsVirtual(challengerVoice));
    EXPECT_EQ(challenger.StartCount, 0u) << "a refused voice must never have been started";
    EXPECT_EQ(incumbent.StopCount, 0u);
}

TEST(VoiceManagerStealingTest, NearlyEqualScoresDoNotSwapEveryTick)
{
    // Without the promotion margin two voices scoring 0.500 and 0.501 trade the slot on
    // every single tick — an audible stutter that no "is it playing?" assertion catches.
    VoiceManager manager;
    manager.SetMaxVoices(1);
    manager.SetPromotionMargin(0.02f);

    FakeVoiceHost incumbent;
    FakeVoiceHost challenger;
    const VoiceHandle incumbentVoice = manager.Acquire(&incumbent, MakeParams(0.500f));
    const VoiceHandle challengerVoice = manager.Acquire(&challenger, MakeParams(0.501f));

    for (int tick = 0; tick < 100; ++tick)
    {
        manager.Update(1.0f / 60.0f);
    }

    EXPECT_TRUE(manager.IsAudible(incumbentVoice));
    EXPECT_TRUE(manager.IsVirtual(challengerVoice));
    EXPECT_EQ(incumbent.StopCount, 0u);
    EXPECT_EQ(incumbent.StartCount, 1u);
    EXPECT_EQ(manager.GetStats().Steals, 0u);
}

TEST(VoiceManagerStealingTest, ClearlyBetterScoreStillWinsDespiteTheMargin)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);
    manager.SetPromotionMargin(0.02f);

    FakeVoiceHost incumbent;
    FakeVoiceHost challenger;
    const VoiceHandle incumbentVoice = manager.Acquire(&incumbent, MakeParams(0.5f));
    const VoiceHandle challengerVoice = manager.Acquire(&challenger, MakeParams(0.9f));

    EXPECT_TRUE(manager.IsAudible(challengerVoice));
    EXPECT_TRUE(manager.IsVirtual(incumbentVoice));
}

TEST(VoiceManagerStealingTest, AMovingListenerRerankesVoicesOnTheNextTick)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);
    manager.SetPromotionMargin(0.0f);
    manager.SetListenerPosition(glm::vec3(0.0f));

    FakeVoiceHost nearHost;
    FakeVoiceHost farHost;
    const VoiceHandle nearVoice = manager.Acquire(&nearHost, MakeSpatialParams(0.5f, glm::vec3(10.0f, 0.0f, 0.0f)));
    const VoiceHandle farVoice = manager.Acquire(&farHost, MakeSpatialParams(0.5f, glm::vec3(90.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(manager.IsAudible(nearVoice));

    // Walk past the far emitter. Nothing about the voices changed — only the listener.
    manager.SetListenerPosition(glm::vec3(100.0f, 0.0f, 0.0f));
    manager.Update(1.0f / 60.0f);

    EXPECT_TRUE(manager.IsAudible(farVoice));
    EXPECT_TRUE(manager.IsVirtual(nearVoice));
}

// ===========================================================================
// Virtualization and resume — acceptance criterion 2
// ===========================================================================

TEST(VoiceManagerVirtualizationTest, AVirtualizedLoopKeepsAdvancingItsPosition)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost ambience;
    FakeVoiceHost stinger;

    VoiceParams loopParams = MakeParams(0.3f);
    loopParams.Looping = true;
    loopParams.DurationSeconds = 4.0;
    const VoiceHandle loop = manager.Acquire(&ambience, loopParams);
    ASSERT_TRUE(manager.IsAudible(loop));

    const VoiceHandle stolenBy = manager.Acquire(&stinger, MakeParams(0.9f));
    ASSERT_TRUE(manager.IsAudible(stolenBy));
    ASSERT_TRUE(manager.IsVirtual(loop));

    manager.Update(1.5f);
    EXPECT_NEAR(manager.GetPlaybackPosition(loop), 1.5, 1e-6);

    manager.Update(1.0f);
    EXPECT_NEAR(manager.GetPlaybackPosition(loop), 2.5, 1e-6);
}

TEST(VoiceManagerVirtualizationTest, AVirtualizedLoopWrapsInPhaseAndResumesThereNotAtZero)
{
    // The bug this pins: a stolen loop that is stopped and later restarted from the
    // beginning. It "is playing" again, so a naive assertion passes — but the ambience
    // audibly jumps back to its start, and two copies of the same loop drift apart.
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost ambience;
    FakeVoiceHost stinger;

    VoiceParams loopParams = MakeParams(0.3f);
    loopParams.Looping = true;
    loopParams.DurationSeconds = 4.0;
    const VoiceHandle loop = manager.Acquire(&ambience, loopParams);
    const VoiceHandle stolenBy = manager.Acquire(&stinger, MakeParams(0.9f));
    ASSERT_TRUE(manager.IsVirtual(loop));
    ASSERT_EQ(ambience.StartCount, 1u);

    // 5 s of a 4 s loop while inaudible → one wrap, 1 s into the second pass.
    manager.Update(5.0f);
    EXPECT_NEAR(manager.GetPlaybackPosition(loop), 1.0, 1e-6);

    manager.Release(stolenBy);

    ASSERT_TRUE(manager.IsAudible(loop));
    EXPECT_EQ(ambience.StartCount, 2u);
    EXPECT_NEAR(ambience.LastStartPosition, 1.0, 1e-6)
        << "the loop resumed at the wrong phase — this is the failure a 'is it playing again?' assertion misses";
    EXPECT_GT(ambience.LastStartPosition, 0.0);
}

TEST(VoiceManagerVirtualizationTest, PitchScalesHowFastAVirtualizedVoiceAdvances)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost fast;
    FakeVoiceHost blocker;

    VoiceParams params = MakeParams(0.2f);
    params.Looping = true;
    params.DurationSeconds = 100.0;
    params.Pitch = 2.0f;
    const VoiceHandle voice = manager.Acquire(&fast, params);
    manager.Acquire(&blocker, MakeParams(0.9f));
    ASSERT_TRUE(manager.IsVirtual(voice));

    manager.Update(1.0f);
    EXPECT_NEAR(manager.GetPlaybackPosition(voice), 2.0, 1e-6);
}

TEST(VoiceManagerVirtualizationTest, TheBackendsOwnStopPositionWinsOverTheLogicalOne)
{
    // A backend with a real transport (a clip-backed AudioSource reading miniaudio's
    // sample cursor) knows exactly where it stopped; the manager's integrated position is
    // only the fallback for backends that cannot report one.
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost tracked;
    tracked.ReportedStopPosition = 7.25;
    FakeVoiceHost stinger;

    VoiceParams params = MakeParams(0.3f);
    params.Looping = true;
    params.DurationSeconds = 30.0;
    const VoiceHandle voice = manager.Acquire(&tracked, params);
    const VoiceHandle stolenBy = manager.Acquire(&stinger, MakeParams(0.9f));

    ASSERT_TRUE(manager.IsVirtual(voice));
    EXPECT_NEAR(manager.GetPlaybackPosition(voice), 7.25, 1e-6);

    manager.Release(stolenBy);
    EXPECT_NEAR(tracked.LastStartPosition, 7.25, 1e-6);
}

TEST(VoiceManagerVirtualizationTest, AudiblePositionTracksTheBackendCursor)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost host;
    host.DevicePosition = 3.5;
    const VoiceHandle voice = manager.Acquire(&host, MakeParams(0.5f));
    ASSERT_TRUE(manager.IsAudible(voice));

    manager.Update(1.0f / 60.0f);
    // Not 1/60 — the device cursor is authoritative while audible, so integration drift
    // can never accumulate against it.
    EXPECT_NEAR(manager.GetPlaybackPosition(voice), 3.5, 1e-6);
}

TEST(VoiceManagerVirtualizationTest, AOneShotThatEndsWhileVirtualIsRetiredAndFreesItsRecord)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost blocker;
    FakeVoiceHost oneShot;

    manager.Acquire(&blocker, MakeParams(0.9f));

    VoiceParams params = MakeParams(0.2f);
    params.Looping = false;
    params.DurationSeconds = 0.5;
    const VoiceHandle voice = manager.Acquire(&oneShot, params);
    ASSERT_TRUE(manager.IsVirtual(voice));

    manager.Update(1.0f);

    EXPECT_FALSE(manager.IsActive(voice)) << "a finished one-shot must not keep competing for a slot";
    EXPECT_EQ(manager.GetStats().Completions, 1u);
    EXPECT_EQ(oneShot.StartCount, 0u) << "a one-shot that expired while inaudible must never become audible";
}

TEST(VoiceManagerVirtualizationTest, AVoiceOfUnknownLengthIsNeverAutoRetired)
{
    // A stream or a SoundGraph voice has no length to complete against; only its owner can
    // decide it is done. Auto-retiring it would silently free the slot mid-playback.
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost stream;
    VoiceParams params = MakeParams(0.5f);
    params.DurationSeconds = 0.0;
    const VoiceHandle voice = manager.Acquire(&stream, params);

    for (int tick = 0; tick < 1000; ++tick)
    {
        manager.Update(1.0f);
    }

    EXPECT_TRUE(manager.IsActive(voice));
    EXPECT_TRUE(manager.IsAudible(voice));
}

// ===========================================================================
// Refusal — a losing one-shot must not be seeked into halfway
// ===========================================================================

TEST(VoiceManagerRefusalTest, AOneShotRefusedAtTheDoorNeverStartsMidway)
{
    // Issue #730: a sound that cannot win a slot "is refused". Promoting it later would
    // seek past however long it spent silent and play only its tail — an audible fragment
    // of a sound the player never heard begin. Loops resume; one-shots are refused.
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost blocker;
    FakeVoiceHost impact;

    const VoiceHandle blockerVoice = manager.Acquire(&blocker, MakeParams(0.9f));

    VoiceParams oneShot = MakeParams(0.2f);
    oneShot.Looping = false;
    oneShot.DurationSeconds = 2.0;
    const VoiceHandle impactVoice = manager.Acquire(&impact, oneShot);
    ASSERT_TRUE(manager.IsVirtual(impactVoice));
    ASSERT_EQ(impact.StartCount, 0u);

    // It has now been running silently for 1 s of its 2 s.
    manager.Update(1.0f);
    ASSERT_NEAR(manager.GetPlaybackPosition(impactVoice), 1.0, 1e-6);

    // A slot frees. The one-shot must NOT take it — starting now would emit its last 1 s.
    manager.Release(blockerVoice);

    EXPECT_EQ(impact.StartCount, 0u) << "a refused one-shot must never become audible mid-way";
    EXPECT_TRUE(manager.IsVirtual(impactVoice));
    EXPECT_EQ(manager.GetStats().Playing, 0u);

    // It still retires on schedule rather than lingering as a zombie record.
    manager.Update(1.5f);
    EXPECT_FALSE(manager.IsActive(impactVoice));
}

TEST(VoiceManagerRefusalTest, AOneShotStillStartsNormallyWhenASlotIsFree)
{
    // The refusal rule keys on "has advanced while silent", not on "is a one-shot" — a
    // fresh one-shot at position 0 has missed nothing and must play.
    VoiceManager manager;
    manager.SetMaxVoices(4);

    FakeVoiceHost impact;
    VoiceParams oneShot = MakeParams(0.2f);
    oneShot.DurationSeconds = 2.0;

    const VoiceHandle voice = manager.Acquire(&impact, oneShot);

    EXPECT_TRUE(manager.IsAudible(voice));
    EXPECT_EQ(impact.StartCount, 1u);
    EXPECT_NEAR(impact.LastStartPosition, 0.0, 1e-6);
}

TEST(VoiceManagerRefusalTest, ALoopIsResumedWhereAOneShotWouldBeRefused)
{
    // Same setup as AOneShotRefusedAtTheDoorNeverStartsMidway, differing only in Looping —
    // the two cases must diverge, or the refusal rule is really just "never promote".
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost blocker;
    FakeVoiceHost ambience;

    const VoiceHandle blockerVoice = manager.Acquire(&blocker, MakeParams(0.9f));

    VoiceParams loop = MakeParams(0.2f);
    loop.Looping = true;
    loop.DurationSeconds = 2.0;
    const VoiceHandle loopVoice = manager.Acquire(&ambience, loop);
    ASSERT_TRUE(manager.IsVirtual(loopVoice));

    manager.Update(1.0f);
    manager.Release(blockerVoice);

    EXPECT_TRUE(manager.IsAudible(loopVoice));
    EXPECT_EQ(ambience.StartCount, 1u);
    EXPECT_NEAR(ambience.LastStartPosition, 1.0, 1e-6);
}

TEST(VoiceManagerRefusalTest, AnUnknownLengthVoiceStaysPromotableSoItIsNotStrandedSilent)
{
    // A stream or SoundGraph voice reports DurationSeconds == 0: the manager cannot
    // schedule its end, so refusing it permanently would leave it silent forever rather
    // than merely skipping it. Such a voice must stay a promotion candidate.
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost blocker;
    FakeVoiceHost stream;

    const VoiceHandle blockerVoice = manager.Acquire(&blocker, MakeParams(0.9f));

    VoiceParams params = MakeParams(0.2f);
    params.Looping = false;
    params.DurationSeconds = 0.0;
    const VoiceHandle streamVoice = manager.Acquire(&stream, params);
    ASSERT_TRUE(manager.IsVirtual(streamVoice));

    manager.Update(1.0f);
    manager.Release(blockerVoice);

    EXPECT_TRUE(manager.IsAudible(streamVoice));
    EXPECT_EQ(stream.StartCount, 1u);
}

// ===========================================================================
// Slot lifecycle
// ===========================================================================

TEST(VoiceManagerLifecycleTest, ReleasingAnAudibleVoicePromotesTheBestVirtualOne)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost incumbent;
    FakeVoiceHost runnerUp;
    FakeVoiceHost alsoRan;

    // Unknown-duration voices, so they stay promotable — a known-length one-shot that has
    // already advanced while silent is deliberately refused for good (see the refusal
    // tests below).
    const VoiceHandle incumbentVoice = manager.Acquire(&incumbent, MakeParams(0.9f));
    const VoiceHandle runnerUpVoice = manager.Acquire(&runnerUp, MakeParams(0.6f));
    const VoiceHandle alsoRanVoice = manager.Acquire(&alsoRan, MakeParams(0.3f));

    manager.Release(incumbentVoice);

    EXPECT_TRUE(manager.IsAudible(runnerUpVoice));
    EXPECT_TRUE(manager.IsVirtual(alsoRanVoice));
    EXPECT_EQ(runnerUp.StartCount, 1u);
    EXPECT_EQ(manager.GetStats().Playing, 1u);
}

TEST(VoiceManagerLifecycleTest, ReleaseIsSafeForInvalidAndAlreadyReleasedHandles)
{
    VoiceManager manager;
    FakeVoiceHost host;
    const VoiceHandle voice = manager.Acquire(&host, MakeParams(0.5f));

    manager.Release(voice);
    manager.Release(voice);
    manager.Release(kInvalidVoiceHandle);
    manager.Release(999999);

    EXPECT_EQ(manager.GetStats().Playing, 0u);
    EXPECT_FALSE(manager.IsActive(voice));
}

TEST(VoiceManagerLifecycleTest, ANullHostIsRejected)
{
    VoiceManager manager;
    EXPECT_EQ(manager.Acquire(nullptr, MakeParams(1.0f)), kInvalidVoiceHandle);
    EXPECT_EQ(manager.GetStats().Playing, 0u);
}

TEST(VoiceManagerLifecycleTest, HandlesAreNeverReusedSoAStaleHandleCannotAliasANewVoice)
{
    VoiceManager manager;
    FakeVoiceHost first;
    FakeVoiceHost second;

    const VoiceHandle firstVoice = manager.Acquire(&first, MakeParams(0.5f));
    manager.Release(firstVoice);
    const VoiceHandle secondVoice = manager.Acquire(&second, MakeParams(0.5f));

    EXPECT_NE(firstVoice, secondVoice);
    EXPECT_FALSE(manager.IsActive(firstVoice));
    EXPECT_TRUE(manager.IsActive(secondVoice));
}

TEST(VoiceManagerLifecycleTest, AHostThatRefusesToStartFallsBackToVirtual)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);

    FakeVoiceHost broken;
    broken.RefuseToStart = true;

    const VoiceHandle voice = manager.Acquire(&broken, MakeParams(0.9f));

    EXPECT_TRUE(manager.IsVirtual(voice)) << "a slot held by a voice that never started would be a silent slot leak";
    EXPECT_EQ(manager.GetStats().Playing, 0u);
}

TEST(VoiceManagerLifecycleTest, UpdateParamsRerankesWithoutRestartingAnAudibleVoice)
{
    VoiceManager manager;
    manager.SetMaxVoices(1);
    manager.SetPromotionMargin(0.0f);

    FakeVoiceHost fading;
    FakeVoiceHost waiting;

    const VoiceHandle fadingVoice = manager.Acquire(&fading, MakeParams(0.8f, 1.0f));
    const VoiceHandle waitingVoice = manager.Acquire(&waiting, MakeParams(0.5f, 1.0f));
    ASSERT_TRUE(manager.IsAudible(fadingVoice));
    ASSERT_EQ(fading.StartCount, 1u);

    // Fade the incumbent to near-silence: gain feeds the score, so it should now yield.
    manager.UpdateParams(fadingVoice, MakeParams(0.8f, 0.01f));
    manager.Update(1.0f / 60.0f);

    EXPECT_TRUE(manager.IsAudible(waitingVoice));
    EXPECT_TRUE(manager.IsVirtual(fadingVoice));
    EXPECT_EQ(fading.StartCount, 1u) << "re-ranking must not have restarted the incumbent before stealing from it";
}

TEST(VoiceManagerLifecycleTest, ResetDropsEveryVoiceAndCounter)
{
    VoiceManager manager;
    manager.SetMaxVoices(2);

    FakeVoiceHost hosts[4];
    VoiceHandle handles[4]{};
    for (sizet i = 0; i < 4; ++i)
    {
        handles[i] = manager.Acquire(&hosts[i], MakeParams(0.5f));
    }
    ASSERT_GT(manager.GetStats().Playing, 0u);

    manager.Reset();

    const auto stats = manager.GetStats();
    EXPECT_EQ(stats.Playing, 0u);
    EXPECT_EQ(stats.Virtual, 0u);
    EXPECT_EQ(stats.Steals, 0u);
    for (const auto handle : handles)
    {
        EXPECT_FALSE(manager.IsActive(handle));
    }
}

// ===========================================================================
// Oversubscription end-to-end — the shape of acceptance criterion 1
// ===========================================================================

TEST(VoiceManagerBudgetTest, OversubscribedSceneKeepsTheNearestAndMostImportantAudibleOverManyTicks)
{
    VoiceManager manager;
    manager.SetMaxVoices(6);
    manager.SetListenerPosition(glm::vec3(0.0f));

    constexpr sizet kVoiceCount = 120;
    std::vector<FakeVoiceHost> hosts(kVoiceCount);
    std::vector<VoiceHandle> handles;
    handles.reserve(kVoiceCount);

    // One deliberately dominant voice: max priority, right on top of the listener. It must
    // still be audible after everything else has piled in.
    VoiceParams heroParams = MakeSpatialParams(1.0f, glm::vec3(0.0f));
    const VoiceHandle hero = manager.Acquire(&hosts[0], heroParams);
    handles.push_back(hero);

    for (sizet i = 1; i < kVoiceCount; ++i)
    {
        const f32 distance = 5.0f + static_cast<f32>(i);
        handles.push_back(manager.Acquire(&hosts[i], MakeSpatialParams(0.4f, glm::vec3(distance, 0.0f, 0.0f))));
    }

    for (int tick = 0; tick < 240; ++tick)
    {
        manager.Update(1.0f / 60.0f);
        ASSERT_LE(manager.GetStats().Playing, 6u) << "cap exceeded on tick " << tick;
    }

    EXPECT_EQ(manager.GetStats().Playing, 6u);
    EXPECT_TRUE(manager.IsAudible(hero));

    // The audible set is the six nearest of the equal-priority crowd, plus the hero.
    for (sizet i = 1; i <= 5; ++i)
    {
        EXPECT_TRUE(manager.IsAudible(handles[i])) << "voice " << i << " should be among the nearest";
    }
    for (sizet i = 6; i < kVoiceCount; ++i)
    {
        EXPECT_TRUE(manager.IsVirtual(handles[i])) << "voice " << i << " should have been virtualized";
    }
}
